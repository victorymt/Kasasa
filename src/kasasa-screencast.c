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
#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>

#include "kasasa-hyprland-stream.h"
#include "kasasa-screencast.h"
#include "kasasa-screencast-pipeline.h"

// Default dimensions
#define DEFAULT_WIDTH  360
#define DEFAULT_HEIGHT 200

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
  SIGNAL_CPU_FALLBACK,

  N_SIGNALS
};

static guint obj_signals[N_SIGNALS];

struct _KasasaScreencast
{
  AdwBin                   parent_instance;
  GtkStack                *stack;
  AdwStatusPage           *no_screencast_page;
  GtkPicture              *picture;

  /* Instance variables */
  GstElement              *pipeline;
  GstElement              *appsrc;
  GstBufferPool           *frame_pool;
  GstBus                  *bus;
  XdpSession              *session;
  KasasaHyprlandStream    *hypr_stream;
  gulong                   closed_handler_id;
  gboolean                 finished;
  gboolean                 using_gpu_pipeline;
  gboolean                 gpu_fallback_attempted;
  gboolean                 cpu_fallback_notified;
  guint                    portal_node_id;
  guint                    fallback_source;
  gint                     dimension[DIMENSION_N_ELEMENTS];
  gint                     stream_width;
  gint                     stream_height;
  KasasaHyprlandStreamFormat stream_format;
};

static void kasasa_screencast_content_interface_init (KasasaContentInterface *iface);

G_DEFINE_TYPE_WITH_CODE (KasasaScreencast, kasasa_screencast, ADW_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (KASASA_TYPE_CONTENT,
                                                kasasa_screencast_content_interface_init))

static void clear_gstreamer_pipeline (KasasaScreencast *self);
static gboolean retry_portal_cpu_idle (gpointer user_data);

static gchar *
get_screencast_pipeline_preference (void)
{
  GSettingsSchemaSource *schema_source;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GSettings) settings = NULL;

  schema_source = g_settings_schema_source_get_default ();
  if (schema_source != NULL)
    schema = g_settings_schema_source_lookup (
      schema_source,
      "io.github.kelvinnovais.Kasasa",
      TRUE);

  if (schema == NULL
      || !g_settings_schema_has_key (schema, "screencast-pipeline"))
    {
      g_message ("The installed settings schema has no screencast-pipeline "
                 "key; using the GPU pipeline preference");
      return g_strdup ("gpu");
    }

  settings = g_settings_new_full (schema, NULL, NULL);
  return g_settings_get_string (settings, "screencast-pipeline");
}

static void
emit_cpu_fallback (KasasaScreencast *self)
{
  if (self->cpu_fallback_notified)
    return;

  self->cpu_fallback_notified = TRUE;
  g_signal_emit (self, obj_signals[SIGNAL_CPU_FALLBACK], 0);
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
  *width = self->dimension[DIMENSION_WIDTH];
  *height = self->dimension[DIMENSION_HEIGHT];
}

static void
set_no_screencast (KasasaScreencast *self)
{
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

  if (self->fallback_source != 0)
    {
      g_source_remove (self->fallback_source);
      self->fallback_source = 0;
    }

  set_no_screencast (self);

  if (self->hypr_stream != NULL)
    {
      kasasa_hyprland_stream_stop (self->hypr_stream);
      self->hypr_stream = NULL;
    }

  clear_gstreamer_pipeline (self);

  if (self->session)
    {
      if (self->closed_handler_id != 0)
        {
          g_signal_handler_disconnect (self->session, self->closed_handler_id);
          self->closed_handler_id = 0;
        }
      xdp_session_close (self->session);
      g_clear_object (&self->session);
    }
}

static void
on_session_closed (XdpSession *session,
                    gpointer    user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (user_data);

  g_info ("Session closed");
  self->closed_handler_id = 0;
  g_clear_object (&self->session);

  if (!self->finished)
    g_signal_emit (self,
                   obj_signals[SIGNAL_EOS],
                   0);
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
error_cb (GstBus           *bus,
          GstMessage       *msg,
          KasasaScreencast *self)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *debug_info = NULL;

  gst_message_parse_error (msg, &error, &debug_info);
  g_warning ("Error received from element %s: %s",
             GST_OBJECT_NAME (msg->src), error->message);
  g_warning ("Debugging information: %s", debug_info ? debug_info : "none");

  if (self->using_gpu_pipeline && self->fallback_source != 0)
    return;

  if (self->using_gpu_pipeline
      && !self->gpu_fallback_attempted
      && self->session != NULL
      && !self->finished)
    {
      self->gpu_fallback_attempted = TRUE;
      g_warning ("GPU screencast failed; retrying with the CPU pipeline");
      self->fallback_source = g_idle_add_full (G_PRIORITY_DEFAULT,
                                               retry_portal_cpu_idle,
                                               g_object_ref (self),
                                               g_object_unref);
      return;
    }

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
  new_width = MAX (new_width, DEFAULT_WIDTH);
  new_height = MAX (new_height, DEFAULT_HEIGHT);

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
clear_gstreamer_pipeline (KasasaScreencast *self)
{
  KasasaScreencastPipeline pipeline = { 0 };

  g_clear_object (&self->appsrc);

  if (self->bus != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->bus, self);
      gst_bus_remove_signal_watch (self->bus);
      gst_object_unref (self->bus);
      self->bus = NULL;
    }

  pipeline.pipeline = self->pipeline;
  self->pipeline = NULL;
  self->using_gpu_pipeline = FALSE;
  kasasa_screencast_pipeline_clear (&pipeline);
  clear_frame_pool (self);

  if (self->picture != NULL)
    gtk_picture_set_paintable (self->picture, NULL);
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

static void
screencast_weak_ref_free (GWeakRef *screencast_ref)
{
  g_weak_ref_clear (screencast_ref);
  g_free (screencast_ref);
}

static GstPadProbeReturn
log_pipewire_caps (GstPad          *pad,
                   GstPadProbeInfo *info,
                   gpointer         user_data)
{
  GWeakRef *screencast_ref = user_data;
  KasasaScreencast *self;
  GstEvent *event;
  GstCaps *caps = NULL;
  const GstStructure *structure;
  g_autofree gchar *caps_string = NULL;
  gint width;
  gint height;

  event = GST_PAD_PROBE_INFO_EVENT (info);
  if (event == NULL || GST_EVENT_TYPE (event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;

  gst_event_parse_caps (event, &caps);
  caps_string = gst_caps_to_string (caps);
  g_info ("PipeWire negotiated caps: %s", caps_string);

  if (gst_caps_is_empty (caps))
    return GST_PAD_PROBE_OK;

  structure = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (structure, "width", &width)
      || !gst_structure_get_int (structure, "height", &height)
      || width <= 0 || height <= 0)
    return GST_PAD_PROBE_OK;

  self = g_weak_ref_get (screencast_ref);
  if (self == NULL)
    return GST_PAD_PROBE_OK;

  queue_stream_size_update (self, width, height);
  g_object_unref (self);

  return GST_PAD_PROBE_OK;
}

static gboolean
activate_portal_pipeline (KasasaScreencast             *self,
                          gint                          fd,
                          guint                         node_id,
                          KasasaScreencastPipelineMode  mode,
                          GError                      **error)
{
  KasasaScreencastPipeline pipeline = { 0 };
  g_autoptr (GstElement) pipewire = NULL;
  g_autoptr (GstPad) pipewire_src_pad = NULL;
  GWeakRef *screencast_ref;
  GstStateChangeReturn ret;

  if (!kasasa_screencast_pipeline_build_portal (fd,
                                                node_id,
                                                mode,
                                                &pipeline,
                                                error))
    return FALSE;

  self->pipeline = pipeline.pipeline;
  pipeline.pipeline = NULL;

  self->bus = gst_element_get_bus (self->pipeline);
  if (self->bus == NULL)
    {
      kasasa_screencast_pipeline_clear (&pipeline);
      clear_gstreamer_pipeline (self);
      return set_screencast_error (error,
                                   G_IO_ERROR_FAILED,
                                   _("The screencast pipeline has no message bus"));
    }

  gst_bus_add_signal_watch (self->bus);
  g_signal_connect (self->bus, "message::error", G_CALLBACK (error_cb), self);
  g_signal_connect (self->bus, "message::eos", G_CALLBACK (eos_cb), self);

  pipewire = gst_bin_get_by_name (GST_BIN (self->pipeline),
                                  "pipewire_element");
  if (pipewire != NULL)
    pipewire_src_pad = gst_element_get_static_pad (pipewire, "src");
  if (pipewire_src_pad != NULL)
    {
      screencast_ref = g_new0 (GWeakRef, 1);
      g_weak_ref_init (screencast_ref, self);
      gst_pad_add_probe (pipewire_src_pad,
                         GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                         log_pipewire_caps,
                         screencast_ref,
                         (GDestroyNotify) screencast_weak_ref_free);
    }

  self->using_gpu_pipeline = mode == KASASA_SCREENCAST_PIPELINE_GPU;
  ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      kasasa_screencast_pipeline_clear (&pipeline);
      clear_gstreamer_pipeline (self);
      return set_screencast_error (error,
                                   G_IO_ERROR_FAILED,
                                   _("The screencast pipeline could not be started"));
    }

  gtk_picture_set_paintable (self->picture, pipeline.paintable);
  kasasa_screencast_pipeline_clear (&pipeline);
  g_info ("Screencast pipeline uses %s path",
          mode == KASASA_SCREENCAST_PIPELINE_GPU
          ? "DMA-BUF/GL display without CPU analysis"
          : "system-memory fallback");

  return TRUE;
}

static void
emit_pipeline_error (KasasaScreencast *self,
                     const GError     *error)
{
  g_warning ("CPU screencast fallback failed: %s",
             error != NULL ? error->message : "unknown error");
  adw_status_page_set_title (self->no_screencast_page,
                             _("Screencast ended with error"));
  if (!self->finished)
    g_signal_emit (self, obj_signals[SIGNAL_EOS], 0);
}

static gboolean
retry_portal_cpu_idle (gpointer user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (user_data);
  g_autoptr (GError) error = NULL;
  gint fd;

  self->fallback_source = 0;
  if (self->finished || self->session == NULL)
    return G_SOURCE_REMOVE;

  clear_gstreamer_pipeline (self);
  fd = xdp_session_open_pipewire_remote (self->session);
  if (fd < 0)
    {
      g_set_error_literal (&error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Unable to reopen the PipeWire connection");
      emit_pipeline_error (self, error);
      return G_SOURCE_REMOVE;
    }

  if (!activate_portal_pipeline (self,
                                 fd,
                                 self->portal_node_id,
                                 KASASA_SCREENCAST_PIPELINE_CPU,
                                 &error))
    {
      emit_pipeline_error (self, error);
      return G_SOURCE_REMOVE;
    }

  emit_cpu_fallback (self);
  return G_SOURCE_REMOVE;
}

gboolean
kasasa_screencast_show (KasasaScreencast *self,
                        XdpSession       *session,
                        gint              fd,
                        guint             node_id,
                        gint              expected_width,
                        gint              expected_height,
                        GError           **error)

{
  KasasaScreencastPipelineMode mode;
  g_autoptr (GError) pipeline_error = NULL;
  g_autofree gchar *pipeline_preference = NULL;
  gboolean gpu_requested;
  gint fallback_fd;

  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (session == NULL || fd < 0 || self->pipeline != NULL
      || self->session != NULL || self->hypr_stream != NULL)
    {
      if (fd >= 0)
        close (fd);
      return set_screencast_error (error,
                                   G_IO_ERROR_INVALID_ARGUMENT,
                                   _("Invalid screencast connection"));
    }

  self->finished = FALSE;
  self->session = session;
  self->portal_node_id = node_id;
  self->gpu_fallback_attempted = FALSE;
  self->cpu_fallback_notified = FALSE;

  if (expected_width > 0 && expected_height > 0)
    new_dimension (self, expected_width, expected_height);

  pipeline_preference = get_screencast_pipeline_preference ();
  gpu_requested = g_strcmp0 (pipeline_preference, "gpu") == 0;
  mode = kasasa_screencast_pipeline_select_mode (pipeline_preference);
  if (!activate_portal_pipeline (self, fd, node_id, mode, &pipeline_error)
      && mode == KASASA_SCREENCAST_PIPELINE_GPU)
    {
      g_warning ("Unable to start the GPU screencast pipeline: %s",
                 pipeline_error != NULL ? pipeline_error->message : "unknown error");
      g_clear_error (&pipeline_error);
      self->gpu_fallback_attempted = TRUE;
      fallback_fd = xdp_session_open_pipewire_remote (self->session);
      if (fallback_fd >= 0)
        activate_portal_pipeline (self,
                                  fallback_fd,
                                  node_id,
                                  KASASA_SCREENCAST_PIPELINE_CPU,
                                  &pipeline_error);
    }

  if (self->pipeline == NULL)
    {
      if (pipeline_error != NULL)
        g_propagate_error (error, g_steal_pointer (&pipeline_error));
      else
        set_screencast_error (error,
                              G_IO_ERROR_FAILED,
                              _("The screencast pipeline could not be started"));
      xdp_session_close (self->session);
      g_clear_object (&self->session);
      self->finished = TRUE;
      return FALSE;
    }

  if (gpu_requested && !self->using_gpu_pipeline)
    emit_cpu_fallback (self);

  gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->picture));

  self->closed_handler_id = g_signal_connect (self->session,
                                              "closed",
                                              G_CALLBACK (on_session_closed),
                                              self);

  return TRUE;
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

static void
on_hypr_stream_frame (gpointer                     user_data,
                      const guint8                *data,
                      gint                         width,
                      gint                         height,
                      gint                         stride,
                      KasasaHyprlandStreamFormat   format,
                      gboolean                     y_invert)
{
  KasasaScreencast *self = user_data;
  GstBuffer *buffer;
  GstFlowReturn ret;
  GstFlowReturn acquire_ret;
  GstMapInfo map = GST_MAP_INFO_INIT;
  g_autoptr (GstCaps) caps = NULL;
  const gchar *format_name;
  gsize row_size;
  gsize size;
  gint y;

  if (self == NULL || self->appsrc == NULL || data == NULL
      || width <= 0 || height <= 0 || stride <= 0
      || (gsize) width > G_MAXSIZE / 4)
    return;

  row_size = (gsize) width * 4;
  if ((gsize) stride < row_size || (gsize) height > G_MAXSIZE / row_size)
    return;

  format_name = hypr_stream_format_name (format);
  if (format_name == NULL)
    return;

  if (self->stream_width != width
      || self->stream_height != height
      || self->stream_format != format)
    {
      self->stream_width = width;
      self->stream_height = height;
      self->stream_format = format;
      caps = gst_caps_new_simple ("video/x-raw",
                                  "format", G_TYPE_STRING, format_name,
                                  "width", G_TYPE_INT, width,
                                  "height", G_TYPE_INT, height,
                                  "framerate", GST_TYPE_FRACTION, 30, 1,
                                  NULL);
      gst_app_src_set_caps (GST_APP_SRC (self->appsrc), caps);
      if (!configure_frame_pool (self, caps,
                                 (gsize) width * 4 * (gsize) height))
        g_warning ("Unable to configure the Hyprland frame buffer pool");
      queue_stream_size_update (self, width, height);
    }

  size = row_size * (gsize) height;
  buffer = NULL;
  if (self->frame_pool != NULL)
    {
      acquire_ret = gst_buffer_pool_acquire_buffer (self->frame_pool,
                                                    &buffer,
                                                    NULL);
      if (acquire_ret != GST_FLOW_OK)
        buffer = NULL;
    }
  if (buffer == NULL)
    buffer = gst_buffer_new_allocate (NULL, size, NULL);
  if (buffer == NULL || !gst_buffer_map (buffer, &map, GST_MAP_WRITE))
    {
      if (buffer != NULL)
        gst_buffer_unref (buffer);
      return;
    }

  for (y = 0; y < height; y++)
    {
      gint source_y = y_invert ? height - 1 - y : y;
      memcpy (map.data + (gsize) y * row_size,
              data + (gsize) source_y * (gsize) stride,
              row_size);
    }
  gst_buffer_unmap (buffer, &map);

  GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale_int (1, GST_SECOND, 30);
  ret = gst_app_src_push_buffer (GST_APP_SRC (self->appsrc), buffer);
  if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING)
    g_debug ("appsrc push returned %s", gst_flow_get_name (ret));
}

gboolean
kasasa_screencast_show_hyprland (KasasaScreencast *self,
                                 guint32           window_handle,
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
  self->stream_width = 0;
  self->stream_height = 0;
  self->stream_format = KASASA_HYPRLAND_STREAM_FORMAT_BGRX;

  if (self->pipeline != NULL || self->hypr_stream != NULL)
    {
      return set_screencast_error (error,
                                   G_IO_ERROR_INVALID_ARGUMENT,
                                   _("Screencast is already active"));
    }

  if (expected_width > 0 && expected_height > 0)
    new_dimension (self, expected_width, expected_height);

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
                                       "framerate", GST_TYPE_FRACTION, 30, 1,
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

  g_info ("Screencast pipeline uses Hyprland toplevel-export appsrc path");

  gst_bin_add_many (GST_BIN (self->pipeline),
                    appsrc, display_queue, gtksink,
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

  gtk_picture_set_paintable (self->picture, paintable);

  self->bus = gst_element_get_bus (self->pipeline);
  if (self->bus == NULL)
    return set_screencast_error (error,
                                 G_IO_ERROR_FAILED,
                                 _("The screencast pipeline has no message bus"));

  gst_bus_add_signal_watch (self->bus);
  g_signal_connect (self->bus, "message::error", G_CALLBACK (error_cb), self);
  g_signal_connect (self->bus, "message::eos", G_CALLBACK (eos_cb), self);

  ret = gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE)
    {
      return set_screencast_error (error,
                                   G_IO_ERROR_FAILED,
                                   _("The screencast pipeline could not be started"));
    }

  self->appsrc = gst_object_ref (appsrc);
  self->hypr_stream = kasasa_hyprland_stream_start (window_handle,
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
                              _("Couldn't start Hyprland window capture"));
      return FALSE;
    }

  gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->picture));

  return TRUE;

ELEMENT_ERROR:
  unref_element (appsrc);
  unref_element (gtksink);
  unref_element (display_queue);

  return set_screencast_error (error,
                               G_IO_ERROR_NOT_SUPPORTED,
                               _("Required GStreamer plugins are unavailable"));
}

static void
kasasa_screencast_dispose (GObject *object)
{
  KasasaScreencast *self = KASASA_SCREENCAST (object);

  kasasa_screencast_finish (KASASA_CONTENT (self));
  clear_gstreamer_pipeline (self);

  G_OBJECT_CLASS (kasasa_screencast_parent_class)->dispose (object);
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

  obj_signals[SIGNAL_CPU_FALLBACK] =
    g_signal_new ("cpu-fallback",
                  KASASA_TYPE_SCREENCAST,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE,
                  0);

  object_class->dispose = kasasa_screencast_dispose;

  widget_class->measure = kasasa_screencast_measure;
  widget_class->size_allocate = kasasa_screencast_size_allocate;
}

static void
kasasa_screencast_init (KasasaScreencast *self)
{
  self->pipeline = NULL;
  self->bus = NULL;
  self->fallback_source = 0;
  self->closed_handler_id = 0;
  self->finished = TRUE;

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

  gtk_widget_set_hexpand (GTK_WIDGET (self->stack), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->stack), TRUE);
  gtk_stack_add_child (self->stack, GTK_WIDGET (self->picture));

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
