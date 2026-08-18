/* kasasa-screencast.c
 *
 * Copyright 2025-2026 Kelvin Novais
 * Copyright 2026 victorymt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/gstdebugutils.h>
#include <glib/gi18n.h>
#include <string.h>
#include <wayland-client-protocol.h>

#include "kasasa-crop-paintable.h"
#include "kasasa-dmabuf-paintable.h"
#include "kasasa-frame-transform.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-screencast.h"

// Default dimensions
#define DEFAULT_WIDTH  360
#define DEFAULT_HEIGHT 200
#define DEFAULT_SCREENCAST_FRAME_RATE 30U
#define MAX_SCREENCAST_FRAME_RATE     120U
#define MIN_CROP_DIMENSION             32
#define CROP_HANDLE_RADIUS             12.0

enum
{
  DIMENSION_WIDTH,
  DIMENSION_HEIGHT,

  DIMENSION_N_ELEMENTS
};

// Signals
enum
{
  SIGNAL_NEW_DIMENSION,
  SIGNAL_EOS,
  SIGNAL_DMABUF_FALLBACK,

  N_SIGNALS
};

static guint obj_signals[N_SIGNALS];

typedef struct
{
  GBytes *bytes;
  GdkMemoryFormat memory_format;
  gint fd;
  gint width;
  gint height;
  gint stride;
  guint32 offset;
  guint32 fourcc;
  guint64 modifier;
  gboolean y_invert;
  guint32 transform;
  GDestroyNotify release;
  gpointer release_data;
} PreviewFrameUpdate;

struct _KasasaScreencast
{
  AdwBin                   parent_instance;
  GtkStack                *stack;
  AdwStatusPage           *no_screencast_page;
  GtkPicture              *picture;
  GtkOverlay              *picture_overlay;
  GtkDrawingArea          *crop_overlay;
  GtkGestureDrag          *crop_drag;

  /* Instance variables */
  GstElement              *pipeline;
  GstElement              *appsrc;
  GstBufferPool           *frame_pool;
  GstBus                  *bus;
  gboolean                 gst_trace;
  gint64                   pipeline_started_at_usec;
  gint64                   first_frame_at_usec;
  gint                      gst_frames_pushed;
  KasasaHyprlandStream    *hypr_stream;
  gboolean                 finished;
  gboolean                 dmabuf_fallback_notified;
  gint                     dimension[DIMENSION_N_ELEMENTS];
  gint                     stream_width;
  gint                     stream_height;
  guint                    frame_rate;
  KasasaHyprlandStreamFormat stream_format;
  KasasaDmabufPaintable   *dmabuf_paintable;
  KasasaCropPaintable     *crop_paintable;
  GMutex                   preview_update_lock;
  PreviewFrameUpdate      *pending_preview_update;
  gboolean                 preview_update_scheduled;
  gboolean                 direct_dmabuf_failed;
  gboolean                 direct_dmabuf_active;
  gboolean                 crop_supported;
  gboolean                 crop_mode;
  gboolean                 crop_applied;
  gdouble                  crop_x;
  gdouble                  crop_y;
  gdouble                  crop_width;
  gdouble                  crop_height;
  gdouble                  crop_original_x;
  gdouble                  crop_original_y;
  gdouble                  crop_original_width;
  gdouble                  crop_original_height;
  gdouble                  crop_drag_start_x;
  gdouble                  crop_drag_start_y;
  gdouble                  crop_drag_x;
  gdouble                  crop_drag_y;
  gdouble                  crop_drag_width;
  gdouble                  crop_drag_height;
  guint                    crop_drag_mode;
};

static void kasasa_screencast_content_interface_init (KasasaContentInterface *iface);

G_DEFINE_TYPE_WITH_CODE (KasasaScreencast, kasasa_screencast, ADW_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (KASASA_TYPE_CONTENT,
                                                kasasa_screencast_content_interface_init))

static void clear_gstreamer_pipeline (KasasaScreencast *self);

static gboolean
gst_trace_enabled (void)
{
  const gchar *value = g_getenv ("KASASA_GST_TRACE");

  return value != NULL && *value != '\0' && g_strcmp0 (value, "0") != 0;
}

static void
dump_gstreamer_pipeline (KasasaScreencast *self,
                         const gchar      *name)
{
  if (self->gst_trace && self->pipeline != NULL)
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (self->pipeline),
                                       GST_DEBUG_GRAPH_SHOW_ALL,
                                       name);
}

static guint
get_screencast_frame_rate (void)
{
  GSettingsSchemaSource *schema_source;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GSettings) settings = NULL;
  guint frame_rate;

  schema_source = g_settings_schema_source_get_default ();
  if (schema_source != NULL)
    schema = g_settings_schema_source_lookup (
      schema_source,
      "io.github.kelvinnovais.Kasasa",
      TRUE);

  if (schema == NULL
      || !g_settings_schema_has_key (schema, "screencast-framerate"))
    {
      g_message ("The installed settings schema has no screencast-framerate "
                 "key; using 30 FPS");
      return DEFAULT_SCREENCAST_FRAME_RATE;
    }

  settings = g_settings_new_full (schema, NULL, NULL);
  frame_rate = g_settings_get_uint (settings, "screencast-framerate");
  return CLAMP (frame_rate, 1U, MAX_SCREENCAST_FRAME_RATE);
}

static void
emit_dmabuf_fallback (KasasaScreencast *self)
{
  if (self->dmabuf_fallback_notified)
    return;

  self->dmabuf_fallback_notified = TRUE;
  g_signal_emit (self, obj_signals[SIGNAL_DMABUF_FALLBACK], 0);
}

static void
clear_frame_pool (KasasaScreencast *self)
{
  if (self->frame_pool == NULL)
    return;

  gst_buffer_pool_set_active (self->frame_pool, FALSE);
  gst_object_unref (self->frame_pool);
  self->frame_pool = NULL;
}

static gboolean
configure_frame_pool (KasasaScreencast *self,
                      GstCaps          *caps,
                      gsize             size)
{
  GstStructure *config;

  clear_frame_pool (self);
  if (size > G_MAXUINT)
    return FALSE;

  self->frame_pool = gst_buffer_pool_new ();
  config = gst_buffer_pool_get_config (self->frame_pool);
  gst_buffer_pool_config_set_params (config, caps, size, 3, 0);

  if (!gst_buffer_pool_set_config (self->frame_pool, config)
      || !gst_buffer_pool_set_active (self->frame_pool, TRUE))
    {
      clear_frame_pool (self);
      return FALSE;
    }

  return TRUE;
}

static void
kasasa_screencast_get_dimensions (KasasaContent *content,
                                  gint          *height,
                                  gint          *width)
{
  KasasaScreencast *self = NULL;

  g_return_if_fail (KASASA_IS_SCREENCAST (content));

  self = KASASA_SCREENCAST (content);
  if (self->crop_mode
      && self->stream_width > 0
      && self->stream_height > 0)
    {
      *width = self->stream_width;
      *height = self->stream_height;
    }
  else
    {
      *width = self->dimension[DIMENSION_WIDTH];
      *height = self->dimension[DIMENSION_HEIGHT];
    }
}

static void
set_no_screencast (KasasaScreencast *self)
{
  self->crop_mode = FALSE;
  self->crop_supported = FALSE;
  self->crop_applied = FALSE;
  self->crop_x = 0;
  self->crop_y = 0;
  self->crop_width = 1;
  self->crop_height = 1;
  if (self->crop_paintable != NULL)
    kasasa_crop_paintable_reset (self->crop_paintable);
  if (self->crop_overlay != NULL)
    gtk_widget_set_visible (GTK_WIDGET (self->crop_overlay), FALSE);

  self->dimension[DIMENSION_WIDTH] = DEFAULT_WIDTH;
  self->dimension[DIMENSION_HEIGHT] = DEFAULT_HEIGHT;

  gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->no_screencast_page));
}

static void
kasasa_screencast_finish (KasasaContent *content)
{
  KasasaScreencast *self = NULL;

  g_return_if_fail (KASASA_IS_SCREENCAST (content));

  self = KASASA_SCREENCAST (content);
  if (self->finished)
    return;

  self->finished = TRUE;

  set_no_screencast (self);

  if (self->hypr_stream != NULL)
    {
      kasasa_hyprland_stream_stop (self->hypr_stream);
      self->hypr_stream = NULL;
    }

  clear_gstreamer_pipeline (self);
}

static void
eos_cb (GstBus           *bus,
        GstMessage       *msg,
        KasasaScreencast *self)
{
  g_info ("End-Of-Stream reached");
  if (!self->finished)
    g_signal_emit (self,
                   obj_signals[SIGNAL_EOS],
                   0);
}

static void
warning_cb (GstBus           *bus,
            GstMessage       *msg,
            KasasaScreencast *self)
{
  g_autoptr (GError) warning = NULL;
  g_autofree gchar *debug_info = NULL;

  gst_message_parse_warning (msg, &warning, &debug_info);
  g_debug ("GStreamer warning from %s: %s%s%s",
           GST_OBJECT_NAME (msg->src),
           warning != NULL ? warning->message : "unknown warning",
           debug_info != NULL ? " debug=" : "",
           debug_info != NULL ? debug_info : "");
}

static void
state_changed_cb (GstBus           *bus,
                  GstMessage       *msg,
                  KasasaScreencast *self)
{
  GstState old_state;
  GstState new_state;
  GstState pending_state;

  if (self->pipeline == NULL || GST_MESSAGE_SRC (msg) != GST_OBJECT (self->pipeline))
    return;

  gst_message_parse_state_changed (msg,
                                   &old_state,
                                   &new_state,
                                   &pending_state);
  g_debug ("GStreamer pipeline state %s -> %s (pending=%s, elapsed=%.1f ms)",
           gst_state_get_name (old_state),
           gst_state_get_name (new_state),
           gst_state_get_name (pending_state),
           self->pipeline_started_at_usec != 0
             ? (gdouble) (g_get_monotonic_time ()
                          - self->pipeline_started_at_usec) / 1000.0
             : 0.0);
}

static void
async_done_cb (GstBus           *bus,
               GstMessage       *msg,
               KasasaScreencast *self)
{
  GstClockTime running_time = GST_CLOCK_TIME_NONE;

  gst_message_parse_async_done (msg, &running_time);
  g_debug ("GStreamer pipeline async-done running-time=%" GST_TIME_FORMAT
           " elapsed=%.1f ms",
           GST_TIME_ARGS (running_time),
           self->pipeline_started_at_usec != 0
             ? (gdouble) (g_get_monotonic_time ()
                          - self->pipeline_started_at_usec) / 1000.0
             : 0.0);
}

static void
qos_cb (GstBus           *bus,
        GstMessage       *msg,
        KasasaScreencast *self)
{
  GstFormat format = GST_FORMAT_UNDEFINED;
  guint64 processed = 0;
  guint64 dropped = 0;

  gst_message_parse_qos_stats (msg, &format, &processed, &dropped);
  g_debug ("GStreamer QOS from %s: format=%s processed=%" G_GUINT64_FORMAT
           " dropped=%" G_GUINT64_FORMAT,
           GST_OBJECT_NAME (msg->src),
           gst_format_get_name (format),
           processed,
           dropped);
}

static void
error_cb (GstBus           *bus,
          GstMessage       *msg,
          KasasaScreencast *self)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *debug_info = NULL;

  gst_message_parse_error (msg, &error, &debug_info);
  dump_gstreamer_pipeline (self, "kasasa-error");
  g_warning ("Error received from element %s: %s",
             GST_OBJECT_NAME (msg->src), error->message);
  g_warning ("Debugging information: %s", debug_info ? debug_info : "none");

  adw_status_page_set_title (self->no_screencast_page,
                             _("Screencast ended with error"));
  if (!self->finished)
    g_signal_emit (self,
                   obj_signals[SIGNAL_EOS],
                   0);
}

static void
new_dimension (KasasaScreencast *self,
               gint              new_width,
               gint              new_height)
{
  if (self->crop_applied)
    {
      new_width = MAX (MIN_CROP_DIMENSION,
                       (gint) llround ((gdouble) new_width
                                      * self->crop_width));
      new_height = MAX (MIN_CROP_DIMENSION,
                        (gint) llround ((gdouble) new_height
                                       * self->crop_height));
    }
  else
    {
      new_width = MAX (new_width, DEFAULT_WIDTH);
      new_height = MAX (new_height, DEFAULT_HEIGHT);
    }

  if (self->dimension[DIMENSION_WIDTH] != new_width
      || self->dimension[DIMENSION_HEIGHT] != new_height)
    {
      g_signal_emit (self,
                     obj_signals[SIGNAL_NEW_DIMENSION],
                     0,
                     new_width,
                     new_height);
    }

  self->dimension[DIMENSION_WIDTH] = new_width;
  self->dimension[DIMENSION_HEIGHT] = new_height;
}

enum
{
  CROP_DRAG_NONE,
  CROP_DRAG_NEW,
  CROP_DRAG_MOVE,
  CROP_DRAG_TOP_LEFT,
  CROP_DRAG_TOP_RIGHT,
  CROP_DRAG_BOTTOM_LEFT,
  CROP_DRAG_BOTTOM_RIGHT,
};

static void
set_crop_draft_rect (KasasaScreencast *self,
                     gdouble            x,
                     gdouble            y,
                     gdouble            width,
                     gdouble            height)
{
  x = CLAMP (x, 0.0, 1.0);
  y = CLAMP (y, 0.0, 1.0);
  width = CLAMP (width, 0.0, 1.0 - x);
  height = CLAMP (height, 0.0, 1.0 - y);

  self->crop_x = x;
  self->crop_y = y;
  self->crop_width = width;
  self->crop_height = height;
  if (self->crop_overlay != NULL)
    gtk_widget_queue_draw (GTK_WIDGET (self->crop_overlay));
}

static void
set_crop_rect_from_pixels (KasasaScreencast *self,
                           gdouble            x,
                           gdouble            y,
                           gdouble            width,
                           gdouble            height)
{
  gdouble area_width = gtk_widget_get_width (GTK_WIDGET (self->crop_overlay));
  gdouble area_height = gtk_widget_get_height (GTK_WIDGET (self->crop_overlay));
  gdouble minimum_width;
  gdouble minimum_height;

  if (area_width <= 0 || area_height <= 0)
    return;

  minimum_width = MIN ((gdouble) MIN_CROP_DIMENSION, area_width);
  minimum_height = MIN ((gdouble) MIN_CROP_DIMENSION, area_height);
  width = MAX (width, minimum_width);
  height = MAX (height, minimum_height);
  x = CLAMP (x, 0.0, area_width - width);
  y = CLAMP (y, 0.0, area_height - height);
  width = MIN (width, area_width - x);
  height = MIN (height, area_height - y);

  set_crop_draft_rect (self,
                       x / area_width,
                       y / area_height,
                       width / area_width,
                       height / area_height);
}

static gboolean
point_near (gdouble x,
            gdouble y,
            gdouble target_x,
            gdouble target_y)
{
  return hypot (x - target_x, y - target_y) <= CROP_HANDLE_RADIUS;
}

static void
draw_crop_overlay (GtkDrawingArea *area,
                   cairo_t        *cr,
                   gint            width,
                   gint            height,
                   KasasaScreencast *self)
{
  gdouble left;
  gdouble top;
  gdouble right;
  gdouble bottom;

  if (!self->crop_mode || width <= 0 || height <= 0)
    return;

  left = self->crop_x * width;
  top = self->crop_y * height;
  right = (self->crop_x + self->crop_width) * width;
  bottom = (self->crop_y + self->crop_height) * height;

  cairo_set_source_rgba (cr, 0, 0, 0, 0.58);
  cairo_rectangle (cr, 0, 0, width, top);
  cairo_rectangle (cr, 0, bottom, width, height - bottom);
  cairo_rectangle (cr, 0, top, left, bottom - top);
  cairo_rectangle (cr, right, top, width - right, bottom - top);
  cairo_fill (cr);

  cairo_set_line_width (cr, 2.0);
  cairo_set_source_rgba (cr, 1, 1, 1, 0.95);
  cairo_rectangle (cr, left, top, right - left, bottom - top);
  cairo_stroke (cr);

  cairo_set_line_width (cr, 4.0);
  cairo_move_to (cr, left, top + 16);
  cairo_line_to (cr, left, top);
  cairo_line_to (cr, left + 16, top);
  cairo_move_to (cr, right - 16, top);
  cairo_line_to (cr, right, top);
  cairo_line_to (cr, right, top + 16);
  cairo_move_to (cr, left, bottom - 16);
  cairo_line_to (cr, left, bottom);
  cairo_line_to (cr, left + 16, bottom);
  cairo_move_to (cr, right - 16, bottom);
  cairo_line_to (cr, right, bottom);
  cairo_line_to (cr, right, bottom - 16);
  cairo_stroke (cr);
}

static void
crop_drag_begin (GtkGestureDrag  *gesture,
                 gdouble          start_x,
                 gdouble          start_y,
                 KasasaScreencast *self)
{
  gdouble area_width = gtk_widget_get_width (GTK_WIDGET (self->crop_overlay));
  gdouble area_height = gtk_widget_get_height (GTK_WIDGET (self->crop_overlay));
  gdouble left = self->crop_x * area_width;
  gdouble top = self->crop_y * area_height;
  gdouble right = (self->crop_x + self->crop_width) * area_width;
  gdouble bottom = (self->crop_y + self->crop_height) * area_height;

  if (!self->crop_mode || area_width <= 0 || area_height <= 0)
    return;

  /* Keep the outer GtkWindowHandle from treating this same sequence as a
   * request to move the preview window while crop mode is active. */
  gtk_gesture_set_state (GTK_GESTURE (gesture),
                         GTK_EVENT_SEQUENCE_CLAIMED);

  self->crop_drag_start_x = start_x;
  self->crop_drag_start_y = start_y;
  self->crop_drag_x = left;
  self->crop_drag_y = top;
  self->crop_drag_width = right - left;
  self->crop_drag_height = bottom - top;

  if (point_near (start_x, start_y, left, top))
    self->crop_drag_mode = CROP_DRAG_TOP_LEFT;
  else if (point_near (start_x, start_y, right, top))
    self->crop_drag_mode = CROP_DRAG_TOP_RIGHT;
  else if (point_near (start_x, start_y, left, bottom))
    self->crop_drag_mode = CROP_DRAG_BOTTOM_LEFT;
  else if (point_near (start_x, start_y, right, bottom))
    self->crop_drag_mode = CROP_DRAG_BOTTOM_RIGHT;
  else if (start_x >= left && start_x <= right
           && start_y >= top && start_y <= bottom)
    self->crop_drag_mode = CROP_DRAG_MOVE;
  else
    self->crop_drag_mode = CROP_DRAG_NEW;
}

static void
crop_drag_update (GtkGestureDrag  *gesture,
                  gdouble          offset_x,
                  gdouble          offset_y,
                  KasasaScreencast *self)
{
  gdouble x = self->crop_drag_x;
  gdouble y = self->crop_drag_y;
  gdouble width = self->crop_drag_width;
  gdouble height = self->crop_drag_height;
  gdouble current_x = self->crop_drag_start_x + offset_x;
  gdouble current_y = self->crop_drag_start_y + offset_y;

  if (!self->crop_mode)
    return;

  switch (self->crop_drag_mode)
    {
    case CROP_DRAG_NEW:
      x = MIN (self->crop_drag_start_x, current_x);
      y = MIN (self->crop_drag_start_y, current_y);
      width = fabs (current_x - self->crop_drag_start_x);
      height = fabs (current_y - self->crop_drag_start_y);
      break;
    case CROP_DRAG_MOVE:
      x += offset_x;
      y += offset_y;
      break;
    case CROP_DRAG_TOP_LEFT:
      x = MIN (current_x, self->crop_drag_x + self->crop_drag_width - MIN_CROP_DIMENSION);
      y = MIN (current_y, self->crop_drag_y + self->crop_drag_height - MIN_CROP_DIMENSION);
      width = self->crop_drag_x + self->crop_drag_width - x;
      height = self->crop_drag_y + self->crop_drag_height - y;
      break;
    case CROP_DRAG_TOP_RIGHT:
      y = MIN (current_y, self->crop_drag_y + self->crop_drag_height - MIN_CROP_DIMENSION);
      width = current_x - self->crop_drag_x;
      height = self->crop_drag_y + self->crop_drag_height - y;
      break;
    case CROP_DRAG_BOTTOM_LEFT:
      x = MIN (current_x, self->crop_drag_x + self->crop_drag_width - MIN_CROP_DIMENSION);
      width = self->crop_drag_x + self->crop_drag_width - x;
      height = current_y - self->crop_drag_y;
      break;
    case CROP_DRAG_BOTTOM_RIGHT:
      width = current_x - self->crop_drag_x;
      height = current_y - self->crop_drag_y;
      break;
    default:
      return;
    }

  set_crop_rect_from_pixels (self, x, y, width, height);
}

static void
crop_drag_end (GtkGestureDrag  *gesture,
               gdouble          offset_x,
               gdouble          offset_y,
               KasasaScreencast *self)
{
  self->crop_drag_mode = CROP_DRAG_NONE;
}

static void
set_picture_source (KasasaScreencast *self,
                    GdkPaintable     *source)
{
  kasasa_crop_paintable_set_source (self->crop_paintable, source);
  gtk_picture_set_paintable (self->picture,
                             GDK_PAINTABLE (self->crop_paintable));
}

static void
set_committed_crop_rect (KasasaScreencast *self,
                         gdouble            x,
                         gdouble            y,
                         gdouble            width,
                         gdouble            height)
{
  self->crop_x = x;
  self->crop_y = y;
  self->crop_width = width;
  self->crop_height = height;
  kasasa_crop_paintable_set_rect (self->crop_paintable,
                                  x, y, width, height);
}

gboolean
kasasa_screencast_is_crop_available (KasasaScreencast *self)
{
  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);

  return !self->finished && self->crop_supported;
}

gboolean
kasasa_screencast_is_cropping (KasasaScreencast *self)
{
  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);

  return self->crop_mode;
}

gboolean
kasasa_screencast_begin_crop (KasasaScreencast *self)
{
  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);

  if (!kasasa_screencast_is_crop_available (self) || self->crop_mode)
    return FALSE;

  self->crop_original_x = self->crop_applied ? self->crop_x : 0;
  self->crop_original_y = self->crop_applied ? self->crop_y : 0;
  self->crop_original_width = self->crop_applied ? self->crop_width : 1;
  self->crop_original_height = self->crop_applied ? self->crop_height : 1;

  self->crop_mode = TRUE;
  /* Keep the source stable while editing. The draft rectangle is rendered by
   * the overlay and is committed to the paintable only after confirmation. */
  kasasa_crop_paintable_reset (self->crop_paintable);
  if (self->crop_applied)
    set_crop_draft_rect (self,
                         self->crop_x,
                         self->crop_y,
                         self->crop_width,
                         self->crop_height);
  else
    set_crop_draft_rect (self, 0.05, 0.05, 0.9, 0.9);

  gtk_widget_set_visible (GTK_WIDGET (self->crop_overlay), TRUE);
  gtk_widget_set_can_target (GTK_WIDGET (self->crop_overlay), TRUE);
  gtk_widget_queue_draw (GTK_WIDGET (self->crop_overlay));
  return TRUE;
}

gboolean
kasasa_screencast_confirm_crop (KasasaScreencast *self)
{
  gint source_width;
  gint source_height;

  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);

  if (!self->crop_mode)
    return FALSE;

  self->crop_mode = FALSE;
  self->crop_applied = !(self->crop_x == 0 && self->crop_y == 0
                          && self->crop_width == 1
                          && self->crop_height == 1);
  set_committed_crop_rect (self,
                           self->crop_x,
                           self->crop_y,
                           self->crop_width,
                           self->crop_height);

  gtk_widget_set_visible (GTK_WIDGET (self->crop_overlay), FALSE);
  gtk_widget_set_can_target (GTK_WIDGET (self->crop_overlay), FALSE);

  source_width = self->stream_width > 0
                 ? self->stream_width
                 : (self->crop_applied && self->crop_width > 0
                    ? (gint) llround ((gdouble) self->dimension[DIMENSION_WIDTH]
                                      / self->crop_width)
                    : self->dimension[DIMENSION_WIDTH]);
  source_height = self->stream_height > 0
                  ? self->stream_height
                  : (self->crop_applied && self->crop_height > 0
                     ? (gint) llround ((gdouble) self->dimension[DIMENSION_HEIGHT]
                                       / self->crop_height)
                     : self->dimension[DIMENSION_HEIGHT]);
  new_dimension (self, source_width, source_height);
  return TRUE;
}

void
kasasa_screencast_cancel_crop (KasasaScreencast *self)
{
  g_return_if_fail (KASASA_IS_SCREENCAST (self));

  if (!self->crop_mode)
    return;

  self->crop_mode = FALSE;
  set_committed_crop_rect (self,
                           self->crop_original_x,
                           self->crop_original_y,
                           self->crop_original_width,
                           self->crop_original_height);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_overlay), FALSE);
  gtk_widget_set_can_target (GTK_WIDGET (self->crop_overlay), FALSE);
}

void
kasasa_screencast_reset_crop (KasasaScreencast *self)
{
  g_return_if_fail (KASASA_IS_SCREENCAST (self));

  if (!self->crop_mode)
    return;

  set_crop_draft_rect (self, 0, 0, 1, 1);
}

static gboolean
set_screencast_error (GError      **error,
                      GIOErrorEnum  code,
                      const gchar  *message)
{
  g_set_error_literal (error, G_IO_ERROR, code, message);
  return FALSE;
}

static void
unref_element (GstElement *element)
{
  if (element != NULL)
    gst_object_unref (element);
}

static void
preview_frame_update_free (PreviewFrameUpdate *update)
{
  if (update == NULL)
    return;

  if (update->release != NULL)
    update->release (update->release_data);
  g_clear_pointer (&update->bytes, g_bytes_unref);
  g_free (update);
}

static void
clear_pending_preview_update (KasasaScreencast *self)
{
  PreviewFrameUpdate *update;

  g_mutex_lock (&self->preview_update_lock);
  update = g_steal_pointer (&self->pending_preview_update);
  g_mutex_unlock (&self->preview_update_lock);

  preview_frame_update_free (update);
}

static gboolean
apply_pending_preview_update (gpointer user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (user_data);
  PreviewFrameUpdate *update;
  GdkDmabufFormats *formats;
  g_autoptr (GdkDmabufTextureBuilder) builder = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  g_autoptr (GError) error = NULL;

  g_mutex_lock (&self->preview_update_lock);
  update = g_steal_pointer (&self->pending_preview_update);
  if (update == NULL)
    self->preview_update_scheduled = FALSE;
  g_mutex_unlock (&self->preview_update_lock);

  if (update == NULL)
    return G_SOURCE_REMOVE;

  if (self->finished
      || (update->bytes == NULL && self->direct_dmabuf_failed))
    {
      preview_frame_update_free (update);
      return G_SOURCE_CONTINUE;
    }

  if (update->bytes != NULL)
    {
      texture = gdk_memory_texture_new (update->width,
                                        update->height,
                                        update->memory_format,
                                        update->bytes,
                                        (gsize) update->stride);
      kasasa_dmabuf_paintable_set_texture (self->dmabuf_paintable,
                                           texture,
                                           update->transform,
                                           update->y_invert);
      emit_dmabuf_fallback (self);
      if (self->direct_dmabuf_active)
        {
          g_info ("Hyprland window preview switched to wl_shm/GTK memory textures");
          self->direct_dmabuf_active = FALSE;
        }
      preview_frame_update_free (update);
      return G_SOURCE_CONTINUE;
    }

  formats = gdk_display_get_dmabuf_formats (
    gtk_widget_get_display (GTK_WIDGET (self)));
  if (formats == NULL
      || !gdk_dmabuf_formats_contains (formats,
                                       update->fourcc,
                                       update->modifier))
    {
      g_warning ("GTK cannot import Hyprland DMA-BUF 0x%08x:0x%016" G_GINT64_MODIFIER "x; using wl_shm",
                 update->fourcc,
                 update->modifier);
      goto fallback;
    }

  builder = gdk_dmabuf_texture_builder_new ();
  gdk_dmabuf_texture_builder_set_display (
    builder,
    gtk_widget_get_display (GTK_WIDGET (self)));
  gdk_dmabuf_texture_builder_set_width (builder, (guint) update->width);
  gdk_dmabuf_texture_builder_set_height (builder, (guint) update->height);
  gdk_dmabuf_texture_builder_set_fourcc (builder, update->fourcc);
  gdk_dmabuf_texture_builder_set_modifier (builder, update->modifier);
  gdk_dmabuf_texture_builder_set_premultiplied (builder, TRUE);
  gdk_dmabuf_texture_builder_set_n_planes (builder, 1);
  gdk_dmabuf_texture_builder_set_fd (builder, 0, update->fd);
  gdk_dmabuf_texture_builder_set_stride (builder, 0, (guint) update->stride);
  gdk_dmabuf_texture_builder_set_offset (builder, 0, update->offset);

  texture = gdk_dmabuf_texture_builder_build (builder,
                                              update->release,
                                              update->release_data,
                                              &error);
  if (texture == NULL)
    {
      g_warning ("GTK failed to import the Hyprland DMA-BUF: %s; using wl_shm",
                 error != NULL ? error->message : "unknown error");
      goto fallback;
    }

  /* The texture owns the release notification and therefore the pool slot. */
  update->release = NULL;
  update->release_data = NULL;
  kasasa_dmabuf_paintable_set_texture (self->dmabuf_paintable,
                                       texture,
                                       update->transform,
                                       update->y_invert);
  if (gtk_picture_get_paintable (self->picture)
      != GDK_PAINTABLE (self->crop_paintable))
    set_picture_source (self, GDK_PAINTABLE (self->dmabuf_paintable));

  if (!self->direct_dmabuf_active)
    {
      g_info ("Hyprland window preview directly imports DMA-BUF 0x%08x:0x%016" G_GINT64_MODIFIER "x into GTK",
              update->fourcc,
              update->modifier);
      self->direct_dmabuf_active = TRUE;
    }

  preview_frame_update_free (update);
  return G_SOURCE_CONTINUE;

fallback:
  self->direct_dmabuf_failed = TRUE;
  if (self->hypr_stream != NULL)
    kasasa_hyprland_stream_disable_dmabuf (self->hypr_stream);
  preview_frame_update_free (update);
  return G_SOURCE_CONTINUE;
}

static void
queue_preview_frame_update (KasasaScreencast *self,
                            PreviewFrameUpdate *update)
{
  PreviewFrameUpdate *old_update;
  gboolean schedule_update = FALSE;

  g_mutex_lock (&self->preview_update_lock);
  old_update = self->pending_preview_update;
  self->pending_preview_update = update;
  if (!self->preview_update_scheduled)
    {
      self->preview_update_scheduled = TRUE;
      schedule_update = TRUE;
    }
  g_mutex_unlock (&self->preview_update_lock);

  /* Keep at most one not-yet-imported frame. For DMA-BUF this immediately
   * returns a replaced frame's slot to the three-buffer capture pool. */
  preview_frame_update_free (old_update);

  if (schedule_update)
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_HIGH_IDLE,
                                apply_pending_preview_update,
                                g_object_ref (self),
                                g_object_unref);
}

static void
clear_gstreamer_pipeline (KasasaScreencast *self)
{
  if (self->pipeline != NULL && self->gst_trace)
    {
      dump_gstreamer_pipeline (self, "kasasa-stop");
      g_debug ("Stopping GStreamer pipeline after %.1f ms (frames=%d)",
               self->pipeline_started_at_usec != 0
                 ? (gdouble) (g_get_monotonic_time ()
                              - self->pipeline_started_at_usec) / 1000.0
                 : 0.0,
               g_atomic_int_get (&self->gst_frames_pushed));
    }

  g_clear_object (&self->appsrc);
  clear_pending_preview_update (self);
  if (self->dmabuf_paintable != NULL)
    kasasa_dmabuf_paintable_set_texture (self->dmabuf_paintable,
                                         NULL,
                                         WL_OUTPUT_TRANSFORM_NORMAL,
                                         FALSE);
  self->direct_dmabuf_failed = FALSE;
  self->direct_dmabuf_active = FALSE;

  if (self->bus != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->bus, self);
      gst_bus_remove_signal_watch (self->bus);
      gst_object_unref (self->bus);
      self->bus = NULL;
    }

  if (self->pipeline != NULL)
    {
      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
    }
  clear_frame_pool (self);

  if (self->picture != NULL)
    gtk_picture_set_paintable (self->picture, NULL);

  self->pipeline_started_at_usec = 0;
  self->first_frame_at_usec = 0;
  g_atomic_int_set (&self->gst_frames_pushed, 0);
}

typedef struct
{
  KasasaScreencast *self;
  gint width;
  gint height;
} StreamSizeUpdate;

static gboolean
apply_stream_size_update (gpointer user_data)
{
  StreamSizeUpdate *update = user_data;

  if (!update->self->finished)
    new_dimension (update->self, update->width, update->height);

  return G_SOURCE_REMOVE;
}

static void
stream_size_update_free (StreamSizeUpdate *update)
{
  g_object_unref (update->self);
  g_free (update);
}

static void
queue_stream_size_update (KasasaScreencast *self,
                          gint              width,
                          gint              height)
{
  StreamSizeUpdate *update;

  update = g_new0 (StreamSizeUpdate, 1);
  update->self = g_object_ref (self);
  update->width = width;
  update->height = height;
  g_idle_add_full (G_PRIORITY_DEFAULT,
                   apply_stream_size_update,
                   update,
                   (GDestroyNotify) stream_size_update_free);
}

typedef struct
{
  KasasaScreencast *self;
  gchar *message;
} StreamErrorUpdate;

static gboolean
apply_stream_error (gpointer user_data)
{
  StreamErrorUpdate *update = user_data;
  KasasaScreencast *self = update->self;

  if (!self->finished)
    {
      g_warning ("Hyprland window capture ended: %s", update->message);
      adw_status_page_set_title (self->no_screencast_page,
                                 _("Screencast ended with error"));
      adw_status_page_set_description (self->no_screencast_page,
                                       update->message);
      set_no_screencast (self);
      g_signal_emit (self, obj_signals[SIGNAL_EOS], 0);
    }

  return G_SOURCE_REMOVE;
}

static void
stream_error_update_free (StreamErrorUpdate *update)
{
  g_object_unref (update->self);
  g_free (update->message);
  g_free (update);
}

static void
on_hypr_stream_error (gpointer      user_data,
                      const GError *error)
{
  KasasaScreencast *self = user_data;
  StreamErrorUpdate *update;

  if (self == NULL)
    return;

  update = g_new0 (StreamErrorUpdate, 1);
  update->self = g_object_ref (self);
  update->message = g_strdup (error != NULL
                              ? error->message
                              : _("The window capture stopped unexpectedly"));
  g_idle_add_full (G_PRIORITY_DEFAULT,
                   apply_stream_error,
                   update,
                   (GDestroyNotify) stream_error_update_free);
}


static const gchar *
hypr_stream_format_name (KasasaHyprlandStreamFormat format)
{
  switch (format)
    {
    case KASASA_HYPRLAND_STREAM_FORMAT_BGRX:
      return "BGRx";
    case KASASA_HYPRLAND_STREAM_FORMAT_BGRA:
      return "BGRA";
    case KASASA_HYPRLAND_STREAM_FORMAT_RGBX:
      return "RGBx";
    case KASASA_HYPRLAND_STREAM_FORMAT_RGBA:
      return "RGBA";
    default:
      return NULL;
    }
}

static gboolean
hypr_stream_memory_format (KasasaHyprlandStreamFormat  format,
                           GdkMemoryFormat             *memory_format)
{
  switch (format)
    {
    case KASASA_HYPRLAND_STREAM_FORMAT_BGRX:
      *memory_format = GDK_MEMORY_B8G8R8X8;
      return TRUE;
    case KASASA_HYPRLAND_STREAM_FORMAT_BGRA:
      *memory_format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
      return TRUE;
    case KASASA_HYPRLAND_STREAM_FORMAT_RGBX:
      *memory_format = GDK_MEMORY_R8G8B8X8;
      return TRUE;
    case KASASA_HYPRLAND_STREAM_FORMAT_RGBA:
      *memory_format = GDK_MEMORY_R8G8B8A8_PREMULTIPLIED;
      return TRUE;
    default:
      return FALSE;
    }
}

static void
on_hypr_stream_dmabuf_frame (gpointer   user_data,
                             gint       fd,
                             gint       width,
                             gint       height,
                             gint       stride,
                             guint32    offset,
                             guint32    fourcc,
                             guint64    modifier,
                             gboolean   y_invert,
                             guint32    transform,
                             GDestroyNotify release,
                             gpointer   release_data)
{
  KasasaScreencast *self = user_data;
  PreviewFrameUpdate *update;
  gint output_width;
  gint output_height;

  if (self == NULL || release == NULL || fd < 0
      || width <= 0 || height <= 0 || stride <= 0
      || (gsize) height > (G_MAXSIZE - offset) / (gsize) stride)
    {
      if (release != NULL)
        release (release_data);
      return;
    }

  if (transform == WL_OUTPUT_TRANSFORM_90
      || transform == WL_OUTPUT_TRANSFORM_270
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
      output_width = height;
      output_height = width;
    }
  else
    {
      output_width = width;
      output_height = height;
    }

  if (self->stream_width != output_width
      || self->stream_height != output_height)
    {
      self->stream_width = output_width;
      self->stream_height = output_height;
      queue_stream_size_update (self, output_width, output_height);
    }

  update = g_new0 (PreviewFrameUpdate, 1);
  update->fd = fd;
  update->width = width;
  update->height = height;
  update->stride = stride;
  update->offset = offset;
  update->fourcc = fourcc;
  update->modifier = modifier;
  update->y_invert = y_invert;
  update->transform = transform;
  update->release = release;
  update->release_data = release_data;
  queue_preview_frame_update (self, update);
}

static void
on_hypr_stream_direct_shm_frame (gpointer                     user_data,
                                 const guint8                *data,
                                 gint                         width,
                                 gint                         height,
                                 gint                         stride,
                                 KasasaHyprlandStreamFormat   format,
                                 gboolean                     y_invert,
                                 guint32                      transform)
{
  KasasaScreencast *self = user_data;
  PreviewFrameUpdate *update;
  GdkMemoryFormat memory_format;
  gsize size;
  gint output_width;
  gint output_height;

  if (self == NULL || data == NULL
      || width <= 0 || height <= 0 || stride <= 0
      || (gsize) width > G_MAXSIZE / 4
      || (gsize) stride < (gsize) width * 4
      || (gsize) height > G_MAXSIZE / (gsize) stride
      || !hypr_stream_memory_format (format, &memory_format))
    return;

  if (transform == WL_OUTPUT_TRANSFORM_90
      || transform == WL_OUTPUT_TRANSFORM_270
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
      output_width = height;
      output_height = width;
    }
  else
    {
      output_width = width;
      output_height = height;
    }

  if (self->stream_width != output_width
      || self->stream_height != output_height)
    {
      self->stream_width = output_width;
      self->stream_height = output_height;
      queue_stream_size_update (self, output_width, output_height);
    }

  size = (gsize) stride * (gsize) height;
  update = g_new0 (PreviewFrameUpdate, 1);
  update->bytes = g_bytes_new (data, size);
  update->memory_format = memory_format;
  update->width = width;
  update->height = height;
  update->stride = stride;
  update->y_invert = y_invert;
  update->transform = transform;
  queue_preview_frame_update (self, update);
}

#ifdef KASASA_ENABLE_TESTS
void
kasasa_screencast_test_push_shm_fallback_frame (
  KasasaScreencast *self)
{
  static const guint8 pixels[4] = { 0, 0, 0, 0xff };

  g_return_if_fail (KASASA_IS_SCREENCAST (self));

  self->finished = FALSE;
  on_hypr_stream_direct_shm_frame (
    self,
    pixels,
    1,
    1,
    4,
    KASASA_HYPRLAND_STREAM_FORMAT_RGBA,
    FALSE,
    WL_OUTPUT_TRANSFORM_NORMAL);
}
#endif

static gboolean
validate_hypr_stream_frame (KasasaScreencast             *self,
                            const guint8                 *data,
                            gint                          width,
                            gint                          height,
                            gint                          stride,
                            KasasaHyprlandStreamFormat    format,
                            const gchar                 **format_name)
{
  gsize row_size;

  if (self == NULL || self->appsrc == NULL || data == NULL
      || width <= 0 || height <= 0 || stride <= 0
      || (gsize) width > G_MAXSIZE / 4)
    return FALSE;

  row_size = (gsize) width * 4;
  if ((gsize) stride < row_size || (gsize) height > G_MAXSIZE / row_size)
    return FALSE;

  *format_name = hypr_stream_format_name (format);
  return *format_name != NULL;
}

static void
update_hypr_stream_caps (KasasaScreencast           *self,
                         KasasaHyprlandStreamFormat  format,
                         const gchar                *format_name,
                         gint                        width,
                         gint                        height)
{
  g_autoptr (GstCaps) caps = NULL;

  if (self->stream_width == width
      && self->stream_height == height
      && self->stream_format == format)
    return;

  self->stream_width = width;
  self->stream_height = height;
  self->stream_format = format;
  caps = gst_caps_new_simple ("video/x-raw",
                              "format", G_TYPE_STRING, format_name,
                              "width", G_TYPE_INT, width,
                              "height", G_TYPE_INT, height,
                              "framerate", GST_TYPE_FRACTION,
                              (gint) self->frame_rate, 1,
                              NULL);
  gst_app_src_set_caps (GST_APP_SRC (self->appsrc), caps);
  if (!configure_frame_pool (self,
                             caps,
                             (gsize) width * 4 * (gsize) height))
    g_warning ("Unable to configure the Hyprland frame buffer pool");
  queue_stream_size_update (self, width, height);
}

static GstBuffer *
acquire_hypr_frame_buffer (KasasaScreencast *self,
                           gsize              size)
{
  GstBuffer *buffer = NULL;

  if (self->frame_pool != NULL
      && gst_buffer_pool_acquire_buffer (self->frame_pool,
                                         &buffer,
                                         NULL) != GST_FLOW_OK)
    buffer = NULL;
  if (buffer == NULL)
    buffer = gst_buffer_new_allocate (NULL, size, NULL);
  return buffer;
}

static void
copy_hypr_frame_pixels (guint8       *destination,
                        const guint8 *source,
                        gint          width,
                        gint          height,
                        gint          source_stride,
                        gint          output_width,
                        gint          output_height,
                        gboolean      y_invert,
                        guint32       transform)
{
  gsize output_stride = (gsize) output_width * 4;
  gint x;
  gint y;

  if (transform == WL_OUTPUT_TRANSFORM_NORMAL)
    {
      for (y = 0; y < height; y++)
        {
          gint source_y = y_invert ? height - 1 - y : y;

          memcpy (destination + (gsize) y * output_stride,
                  source + (gsize) source_y * (gsize) source_stride,
                  output_stride);
        }
      return;
    }

  for (y = 0; y < output_height; y++)
    {
      for (x = 0; x < output_width; x++)
        {
          gint source_x;
          gint source_y;

          kasasa_frame_transform_source_position (transform,
                                                  width,
                                                  height,
                                                  x,
                                                  y,
                                                  &source_x,
                                                  &source_y);
          if (y_invert)
            source_y = height - 1 - source_y;
          memcpy (destination + (gsize) y * output_stride + (gsize) x * 4,
                  source + (gsize) source_y * (gsize) source_stride
                         + (gsize) source_x * 4,
                  4);
        }
    }
}

static void
on_hypr_stream_frame (gpointer                     user_data,
                      const guint8                *data,
                      gint                         width,
                      gint                         height,
                      gint                         stride,
                      KasasaHyprlandStreamFormat   format,
                      gboolean                     y_invert,
                      guint32                      transform)
{
  KasasaScreencast *self = user_data;
  GstBuffer *buffer;
  GstFlowReturn ret;
  GstMapInfo map = GST_MAP_INFO_INIT;
  const gchar *format_name;
  gsize row_size;
  gsize size;
  gint output_width;
  gint output_height;

  if (!validate_hypr_stream_frame (self,
                                   data,
                                   width,
                                   height,
                                   stride,
                                   format,
                                   &format_name))
    return;

  transform = kasasa_frame_transform_normalize (transform);
  kasasa_frame_transform_dimensions (width,
                                     height,
                                     transform,
                                     &output_width,
                                     &output_height);
  update_hypr_stream_caps (self,
                           format,
                           format_name,
                           output_width,
                           output_height);

  row_size = (gsize) output_width * 4;
  if ((gsize) output_height > G_MAXSIZE / row_size)
    return;
  size = row_size * (gsize) output_height;
  buffer = acquire_hypr_frame_buffer (self, size);
  if (buffer == NULL || !gst_buffer_map (buffer, &map, GST_MAP_WRITE))
    {
      if (buffer != NULL)
        gst_buffer_unref (buffer);
      return;
    }

  copy_hypr_frame_pixels (map.data,
                          data,
                          width,
                          height,
                          stride,
                          output_width,
                          output_height,
                          y_invert,
                          transform);
  gst_buffer_unmap (buffer, &map);

  GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale_int (
    GST_SECOND,
    1,
    self->frame_rate);
  ret = gst_app_src_push_buffer (GST_APP_SRC (self->appsrc), buffer);
  if (self->gst_trace && ret == GST_FLOW_OK)
    {
      gint frame_number = g_atomic_int_add (&self->gst_frames_pushed, 1) + 1;

      if (frame_number == 1)
        {
          self->first_frame_at_usec = g_get_monotonic_time ();
          g_debug ("GStreamer pipeline received first frame after %.1f ms",
                   self->pipeline_started_at_usec != 0
                     ? (gdouble) (self->first_frame_at_usec
                                  - self->pipeline_started_at_usec) / 1000.0
                     : 0.0);
        }
    }
  if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING)
    g_debug ("appsrc push returned %s", gst_flow_get_name (ret));
}

static gboolean
show_hyprland_source (KasasaScreencast *self,
                      guint32           window_handle,
                      const gchar      *output_name,
                      gint              expected_width,
                      gint              expected_height,
                      GError          **error)
{
  GstElement *appsrc = NULL;
  GstElement *gtksink = NULL;
  g_autoptr (GstCaps) appsrc_caps = NULL;
  GstElement *display_queue = NULL;
  g_autoptr (GdkPaintable) paintable = NULL;
  GstStateChangeReturn ret;
  g_autoptr (GError) stream_error = NULL;

  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  self->finished = FALSE;
  self->crop_supported = output_name == NULL;
  self->crop_mode = FALSE;
  self->crop_applied = FALSE;
  set_committed_crop_rect (self, 0, 0, 1, 1);
  self->stream_width = 0;
  self->stream_height = 0;
  self->stream_format = KASASA_HYPRLAND_STREAM_FORMAT_BGRX;
  self->direct_dmabuf_failed = FALSE;
  self->dmabuf_fallback_notified = FALSE;
  self->gst_trace = gst_trace_enabled ();
  self->pipeline_started_at_usec = 0;
  self->first_frame_at_usec = 0;
  g_atomic_int_set (&self->gst_frames_pushed, 0);

  if (self->pipeline != NULL || self->hypr_stream != NULL)
    {
      return set_screencast_error (error,
                                   G_IO_ERROR_INVALID_ARGUMENT,
                                   _("Screencast is already active"));
    }

  self->frame_rate = get_screencast_frame_rate ();

  if (expected_width > 0 && expected_height > 0)
    new_dimension (self, expected_width, expected_height);

  if (output_name == NULL)
    {
      g_info ("Hyprland native window preview uses direct GTK DMA-BUF "
              "import with wl_shm memory-texture fallback at up to %u FPS",
              self->frame_rate);
      set_picture_source (self, GDK_PAINTABLE (self->dmabuf_paintable));

      self->hypr_stream = kasasa_hyprland_stream_start_dmabuf (
        window_handle,
        self->frame_rate,
        on_hypr_stream_dmabuf_frame,
        on_hypr_stream_direct_shm_frame,
        on_hypr_stream_error,
        self,
        NULL,
        &stream_error);
      if (self->hypr_stream == NULL)
        {
          gtk_picture_set_paintable (self->picture, NULL);
          if (stream_error != NULL)
            g_propagate_error (error, g_steal_pointer (&stream_error));
          else
            set_screencast_error (error,
                                  G_IO_ERROR_FAILED,
                                  _("Couldn't start Hyprland window capture"));
          return FALSE;
        }

      gtk_stack_set_visible_child (self->stack,
                                   GTK_WIDGET (self->picture_overlay));
      return TRUE;
    }

  self->pipeline = gst_pipeline_new ("hyprland-pipeline");
  appsrc = gst_element_factory_make ("appsrc", "appsrc");
  gtksink = gst_element_factory_make ("gtk4paintablesink", "sink");
  display_queue = gst_element_factory_make ("queue", "display_queue");

  if (self->pipeline == NULL || appsrc == NULL || display_queue == NULL
      || gtksink == NULL)
    goto ELEMENT_ERROR;

  g_object_get (gtksink, "paintable", &paintable, NULL);
  if (paintable == NULL)
    goto ELEMENT_ERROR;

  /* Placeholder size until the first Hyprland frame arrives with real dims. */
  {
    gint cap_w = expected_width > 0 ? expected_width : DEFAULT_WIDTH;
    gint cap_h = expected_height > 0 ? expected_height : DEFAULT_HEIGHT;

    appsrc_caps = gst_caps_new_simple ("video/x-raw",
                                       "format", G_TYPE_STRING, "BGRx",
                                       "width", G_TYPE_INT, cap_w,
                                       "height", G_TYPE_INT, cap_h,
                                       "framerate", GST_TYPE_FRACTION,
                                       (gint) self->frame_rate, 1,
                                       NULL);
  }
  g_object_set (appsrc,
                "is-live", TRUE,
                "format", GST_FORMAT_TIME,
                "stream-type", GST_APP_STREAM_TYPE_STREAM,
                "block", FALSE,
                "do-timestamp", TRUE,
                "emit-signals", FALSE,
                "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM,
                "max-buffers", (guint64) 2,
                "max-bytes", (guint64) 0,
                "max-time", (guint64) 0,
                NULL);
  gst_app_src_set_caps (GST_APP_SRC (appsrc), appsrc_caps);
  g_object_set (display_queue,
                "leaky", 2,
                "max-size-buffers", 2,
                "max-size-bytes", 0,
                "max-size-time", (guint64) 0,
                NULL);
  g_object_set (gtksink, "sync", FALSE, NULL);

  g_info ("Hyprland native monitor preview uses the wl_shm appsrc path "
          "at up to %u FPS",
          self->frame_rate);

  gst_bin_add_many (GST_BIN (self->pipeline),
                    appsrc,
                    display_queue,
                    gtksink,
                    NULL);

  if (!gst_element_link_many (appsrc,
                              display_queue,
                              gtksink,
                              NULL))
    {
      return set_screencast_error (error,
                                   G_IO_ERROR_FAILED,
                                   _("The screencast pipeline could not be linked"));
    }
  set_picture_source (self, paintable);

  self->bus = gst_element_get_bus (self->pipeline);
  if (self->bus == NULL)
    return set_screencast_error (error,
                                 G_IO_ERROR_FAILED,
                                 _("The screencast pipeline has no message bus"));

  gst_bus_add_signal_watch (self->bus);
  g_signal_connect (self->bus, "message::error", G_CALLBACK (error_cb), self);
  g_signal_connect (self->bus, "message::eos", G_CALLBACK (eos_cb), self);
  if (self->gst_trace)
    {
      g_signal_connect (self->bus,
                        "message::warning",
                        G_CALLBACK (warning_cb),
                        self);
      g_signal_connect (self->bus,
                        "message::state-changed",
                        G_CALLBACK (state_changed_cb),
                        self);
      g_signal_connect (self->bus,
                        "message::async-done",
                        G_CALLBACK (async_done_cb),
                        self);
      g_signal_connect (self->bus,
                        "message::qos",
                        G_CALLBACK (qos_cb),
                        self);
    }

  self->pipeline_started_at_usec = g_get_monotonic_time ();
  ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  if (self->gst_trace)
    {
      g_debug ("GStreamer pipeline PLAYING request returned %s",
               gst_state_change_return_get_name (ret));
      dump_gstreamer_pipeline (self, "kasasa-start");
    }
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      return set_screencast_error (error,
                                   G_IO_ERROR_FAILED,
                                   _("The screencast pipeline could not be started"));
    }

  self->appsrc = gst_object_ref (appsrc);
  self->hypr_stream = kasasa_hyprland_stream_start_output (
    output_name,
    self->frame_rate,
    on_hypr_stream_frame,
    on_hypr_stream_error,
    self,
    NULL,
    &stream_error);
  if (self->hypr_stream == NULL)
    {
      if (stream_error != NULL)
        g_propagate_error (error, g_steal_pointer (&stream_error));
      else
        set_screencast_error (error,
                              G_IO_ERROR_FAILED,
                              _("Couldn't start Hyprland monitor capture"));
      return FALSE;
    }

  gtk_stack_set_visible_child (self->stack,
                               GTK_WIDGET (self->picture_overlay));

  return TRUE;

ELEMENT_ERROR:
  unref_element (appsrc);
  unref_element (gtksink);
  unref_element (display_queue);

  return set_screencast_error (error,
                               G_IO_ERROR_NOT_SUPPORTED,
                               _("Required GStreamer plugins are unavailable"));
}

gboolean
kasasa_screencast_show_hyprland (KasasaScreencast *self,
                                 guint32           window_handle,
                                 gint              expected_width,
                                 gint              expected_height,
                                 GError          **error)
{
  return show_hyprland_source (self,
                               window_handle,
                               NULL,
                               expected_width,
                               expected_height,
                               error);
}

gboolean
kasasa_screencast_show_hyprland_output (KasasaScreencast *self,
                                        const gchar      *output_name,
                                        gint              expected_width,
                                        gint              expected_height,
                                        GError          **error)
{
  g_return_val_if_fail (output_name != NULL && *output_name != '\0', FALSE);

  return show_hyprland_source (self,
                               0,
                               output_name,
                               expected_width,
                               expected_height,
                               error);
}

static void
kasasa_screencast_dispose (GObject *object)
{
  KasasaScreencast *self = KASASA_SCREENCAST (object);

  kasasa_screencast_finish (KASASA_CONTENT (self));
  clear_gstreamer_pipeline (self);
  g_clear_object (&self->crop_paintable);
  g_clear_object (&self->dmabuf_paintable);

  G_OBJECT_CLASS (kasasa_screencast_parent_class)->dispose (object);
}

static void
kasasa_screencast_finalize (GObject *object)
{
  KasasaScreencast *self = KASASA_SCREENCAST (object);

  g_mutex_clear (&self->preview_update_lock);

  G_OBJECT_CLASS (kasasa_screencast_parent_class)->finalize (object);
}

static void
kasasa_screencast_content_interface_init (KasasaContentInterface *iface)
{
  iface->get_dimensions = kasasa_screencast_get_dimensions;
  iface->finish = kasasa_screencast_finish;
}

static void
kasasa_screencast_measure (GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
  *minimum = 0;
  *natural = 0;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
kasasa_screencast_size_allocate (GtkWidget *widget,
                                 int        width,
                                 int        height,
                                 int        baseline)
{
  KasasaScreencast *self = KASASA_SCREENCAST (widget);
  GtkWidget *child = adw_bin_get_child (ADW_BIN (widget));
  GtkContentFit fit;

  fit = kasasa_content_should_fill_allocation (
          self->dimension[DIMENSION_HEIGHT],
          self->dimension[DIMENSION_WIDTH],
          height,
          width)
        ? GTK_CONTENT_FIT_FILL
        : GTK_CONTENT_FIT_CONTAIN;
  if (gtk_picture_get_content_fit (self->picture) != fit)
    gtk_picture_set_content_fit (self->picture, fit);

  if (child != NULL)
    gtk_widget_allocate (child, width, height, baseline, NULL);
}

static void
kasasa_screencast_class_init (KasasaScreencastClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  // Signals
  obj_signals[SIGNAL_NEW_DIMENSION] =
    g_signal_new ("new-dimension",
                  KASASA_TYPE_SCREENCAST,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,            // no return value
                  2,                      // 2 arguments
                  G_TYPE_INT,             // width
                  G_TYPE_INT);            // height

  obj_signals[SIGNAL_EOS] =
    g_signal_new ("eos",
                  KASASA_TYPE_SCREENCAST,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,            // no return value
                  0);                     // no argument

  obj_signals[SIGNAL_DMABUF_FALLBACK] =
    g_signal_new ("dmabuf-fallback",
                  KASASA_TYPE_SCREENCAST,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  0);

  object_class->dispose = kasasa_screencast_dispose;
  object_class->finalize = kasasa_screencast_finalize;

  widget_class->measure = kasasa_screencast_measure;
  widget_class->size_allocate = kasasa_screencast_size_allocate;
}

static void
kasasa_screencast_init (KasasaScreencast *self)
{
  self->pipeline = NULL;
  self->bus = NULL;
  self->gst_trace = FALSE;
  self->pipeline_started_at_usec = 0;
  self->first_frame_at_usec = 0;
  self->gst_frames_pushed = 0;
  self->finished = TRUE;
  self->frame_rate = DEFAULT_SCREENCAST_FRAME_RATE;
  g_mutex_init (&self->preview_update_lock);
  self->dmabuf_paintable = kasasa_dmabuf_paintable_new ();
  self->crop_paintable = kasasa_crop_paintable_new (
    GDK_PAINTABLE (self->dmabuf_paintable));
  self->crop_x = 0;
  self->crop_y = 0;
  self->crop_width = 1;
  self->crop_height = 1;

  // Initial dimension to avoid 0 value
  self->dimension[DIMENSION_WIDTH] = DEFAULT_WIDTH;
  self->dimension[DIMENSION_HEIGHT] = DEFAULT_HEIGHT;

  self->stack = GTK_STACK (gtk_stack_new ());

  // Page 1 - No screencast
  self->no_screencast_page = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (self->no_screencast_page,
                                 "screencast-recorded-symbolic");
  adw_status_page_set_title (self->no_screencast_page, _("No screencast"));
  gtk_widget_add_css_class (GTK_WIDGET (self->no_screencast_page), "compact");
  gtk_stack_add_child (self->stack, GTK_WIDGET (self->no_screencast_page));

  // Page 2 - Screencast
  self->picture = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_can_shrink (self->picture, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->picture), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->picture), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->picture), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self->picture), GTK_ALIGN_FILL);

  self->picture_overlay = GTK_OVERLAY (gtk_overlay_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->picture_overlay), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->picture_overlay), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->picture_overlay), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self->picture_overlay), GTK_ALIGN_FILL);
  gtk_overlay_set_child (self->picture_overlay, GTK_WIDGET (self->picture));

  self->crop_overlay = GTK_DRAWING_AREA (gtk_drawing_area_new ());
  gtk_widget_set_hexpand (GTK_WIDGET (self->crop_overlay), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->crop_overlay), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->crop_overlay), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self->crop_overlay), GTK_ALIGN_FILL);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_overlay), FALSE);
  gtk_widget_set_can_target (GTK_WIDGET (self->crop_overlay), FALSE);
  gtk_drawing_area_set_draw_func (self->crop_overlay,
                                  (GtkDrawingAreaDrawFunc) draw_crop_overlay,
                                  self,
                                  NULL);
  gtk_overlay_add_overlay (self->picture_overlay,
                           GTK_WIDGET (self->crop_overlay));
  self->crop_drag = GTK_GESTURE_DRAG (gtk_gesture_drag_new ());
  gtk_event_controller_set_propagation_phase (
    GTK_EVENT_CONTROLLER (self->crop_drag),
    GTK_PHASE_CAPTURE);
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (self->crop_drag), 1);
  g_signal_connect (self->crop_drag,
                    "drag-begin",
                    G_CALLBACK (crop_drag_begin),
                    self);
  g_signal_connect (self->crop_drag,
                    "drag-update",
                    G_CALLBACK (crop_drag_update),
                    self);
  g_signal_connect (self->crop_drag,
                    "drag-end",
                    G_CALLBACK (crop_drag_end),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self->crop_overlay),
                             GTK_EVENT_CONTROLLER (self->crop_drag));

  gtk_widget_set_hexpand (GTK_WIDGET (self->stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);
  gtk_stack_add_child (self->stack, GTK_WIDGET (self->picture_overlay));

  adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (self->stack));
  gtk_widget_set_layout_manager (GTK_WIDGET (self), NULL);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_FILL);
}

KasasaScreencast *
kasasa_screencast_new (void)
{
  return KASASA_SCREENCAST (g_object_new (KASASA_TYPE_SCREENCAST, NULL));
}

gboolean
kasasa_screencast_is_active (KasasaScreencast *self)
{
  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);

  return !self->finished;
}

// https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs/-/tree/main/video/gtk4/examples?ref_type=heads
// https://github.com/bilelmoussaoui/ashpd/blob/master/examples/screen_cast_gstreamer.rs
