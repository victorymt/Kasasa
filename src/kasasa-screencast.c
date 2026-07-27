/* kasasa-screencast.c
 *
 * Copyright 2025-2026 Kelvin Novais
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
#include <gst/video/video.h>
#include <glib/gi18n.h>
#include <unistd.h>

#include "kasasa-crop.h"
#include "kasasa-screencast.h"
#include "kasasa-source.h"

#define CROP_CHEK_INTERVAL 5              // seconds
#define FIRST_CROP_CHECK_INTERVAL 200     // miliseconds
#define CROP_ASPECT_TOLERANCE 0.02

// Default dimensions
#define DEFAULT_WIDTH  360
#define DEFAULT_HEIGHT 200

typedef enum
{
  CROP_CHECK_STOP,
  CROP_CHECK_RETRY,
  CROP_CHECK_DONE,
} CropCheckResult;

typedef struct
{
  CropCheckResult result;
  KasasaCropResult crop_result;
  KasasaCrop crop;
  gint width;
  gint height;
} CropAnalysis;

enum
{
  CROP_TOP,
  CROP_RIGHT,
  CROP_BOTTOM,
  CROP_LEFT,

  CROP_N_ELEMENTS
};

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
  GstBus                  *bus;
  XdpSession              *session;
  gulong                   closed_handler_id;
  guint                    cropping_source;
  KasasaSource             first_crop_source;
  gboolean                 crop_analysis_in_progress;
  gboolean                 finished;
  gint                     crop[CROP_N_ELEMENTS];
  gint                     dimension[DIMENSION_N_ELEMENTS];
  gint                     expected_width;
  gint                     expected_height;
};

static void kasasa_screencast_content_interface_init (KasasaContentInterface *iface);

G_DEFINE_TYPE_WITH_CODE (KasasaScreencast, kasasa_screencast, ADW_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (KASASA_TYPE_CONTENT,
                                                kasasa_screencast_content_interface_init))

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

  kasasa_source_clear (&self->first_crop_source);
  if (self->cropping_source != 0)
    {
      g_source_remove (self->cropping_source);
      self->cropping_source = 0;
    }

  set_no_screencast (self);

  if (self->pipeline)
    gst_element_set_state (self->pipeline, GST_STATE_READY);

  if (self->bus)
    {
      g_signal_handlers_disconnect_by_data (self->bus, self);
      gst_bus_remove_signal_watch (self->bus);
      gst_object_unref (self->bus);
      self->bus = NULL;
    }

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

  adw_status_page_set_title (self->no_screencast_page,
                             _("Screencast ended with error"));
  if (!self->finished)
    g_signal_emit (self,
                   obj_signals[SIGNAL_EOS],
                   0);
}

static void
set_crop (KasasaScreencast *self)
{
  GstElement *videocrop = NULL;

  if (self->finished || self->pipeline == NULL)
    return;

  videocrop = gst_bin_get_by_name (GST_BIN (self->pipeline), "videocrop");

  if (!videocrop)
    {
      g_warning ("Failed to set video crop");
      return;
    }

  g_object_set (videocrop,
                "top", self->crop[CROP_TOP],
                "right", self->crop[CROP_RIGHT],
                "bottom", self->crop[CROP_BOTTOM],
                "left", self->crop[CROP_LEFT],
                NULL);

  gst_object_unref (videocrop);
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

static CropAnalysis *
analyze_crop_sample (GstSample *sample)
{
  CropAnalysis *analysis = g_new0 (CropAnalysis, 1);
  GstBuffer *buffer = NULL;
  const GstCaps *caps = NULL;
  GstVideoInfo video_info;
  GstVideoMeta *video_meta = NULL;
  GstMapInfo map;
  gsize offset;
  gsize stride;

  analysis->result = CROP_CHECK_STOP;

  caps = gst_sample_get_caps (sample);
  if (caps == NULL)
    {
      g_warning ("Sample has no caps; unable to crop to window size.");
      return analysis;
    }

  if (!gst_video_info_from_caps (&video_info, caps))
    {
      g_warning ("Couldn't parse video information from sample caps");
      return analysis;
    }

  if (GST_VIDEO_INFO_FORMAT (&video_info) != GST_VIDEO_FORMAT_BGRx
      && GST_VIDEO_INFO_FORMAT (&video_info) != GST_VIDEO_FORMAT_BGRA)
    {
      g_warning ("Expected format BGRx or BGRA, but received: %s. "\
                 "Unable to crop to window size.",
                 gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&video_info)));
      return analysis;
    }

  analysis->width = GST_VIDEO_INFO_WIDTH (&video_info);
  analysis->height = GST_VIDEO_INFO_HEIGHT (&video_info);

  if (analysis->width < 100 || analysis->height < 100)
    {
      g_warning ("Sample is too small, crop skipped");
      return analysis;
    }

  buffer = gst_sample_get_buffer (sample);
  if (buffer == NULL || !gst_buffer_map (buffer, &map, GST_MAP_READ))
    {
      g_warning ("Unable to map sample buffer for cropping");
      analysis->result = CROP_CHECK_RETRY;
      return analysis;
    }

  video_meta = gst_buffer_get_video_meta (buffer);
  if (video_meta != NULL)
    {
      if (video_meta->stride[0] <= 0)
        {
          g_warning ("Unsupported video stride: %d", video_meta->stride[0]);
          gst_buffer_unmap (buffer, &map);
          return analysis;
        }

      offset = video_meta->offset[0];
      stride = (gsize) video_meta->stride[0];
    }
  else
    {
      if (GST_VIDEO_INFO_PLANE_STRIDE (&video_info, 0) <= 0)
        {
          g_warning ("Unsupported video stride: %d",
                     GST_VIDEO_INFO_PLANE_STRIDE (&video_info, 0));
          gst_buffer_unmap (buffer, &map);
          return analysis;
        }

      offset = GST_VIDEO_INFO_PLANE_OFFSET (&video_info, 0);
      stride = (gsize) GST_VIDEO_INFO_PLANE_STRIDE (&video_info, 0);
    }

  if (offset > map.size)
    analysis->crop_result = KASASA_CROP_RESULT_INVALID;
  else
    analysis->crop_result = kasasa_crop_find_rgb32 (map.data + offset,
                                                    map.size - offset,
                                                    analysis->width,
                                                    analysis->height,
                                                    stride,
                                                    &analysis->crop);

  gst_buffer_unmap (buffer, &map);
  analysis->result = analysis->crop_result == KASASA_CROP_RESULT_INVALID
                     ? CROP_CHECK_STOP
                     : CROP_CHECK_DONE;

  return analysis;
}

static void
analyze_crop_sample_task (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
  g_task_return_pointer (task,
                         analyze_crop_sample (GST_SAMPLE (task_data)),
                         g_free);
}

static void compute_first_crop_values (gpointer user_data);

static void
stop_periodic_crop_analysis (KasasaScreencast *self)
{
  if (self->cropping_source != 0)
    {
      g_source_remove (self->cropping_source);
      self->cropping_source = 0;
    }
}

static void
apply_crop_analysis (KasasaScreencast *self,
                     CropAnalysis     *analysis)
{
  gboolean crop_changed;

  if (self->finished)
    return;

  if (analysis->result == CROP_CHECK_STOP)
    {
      kasasa_source_clear (&self->first_crop_source);
      stop_periodic_crop_analysis (self);
      return;
    }

  if (analysis->result == CROP_CHECK_RETRY)
    {
      kasasa_source_set_timeout_once (&self->first_crop_source,
                                      FIRST_CROP_CHECK_INTERVAL,
                                      compute_first_crop_values,
                                      self);
      return;
    }

  kasasa_source_clear (&self->first_crop_source);

  if (analysis->crop_result == KASASA_CROP_RESULT_EMPTY)
    {
      g_debug ("No non-black content found while computing crop");
      if (self->crop[CROP_TOP] == 0
          && self->crop[CROP_RIGHT] == 0
          && self->crop[CROP_BOTTOM] == 0
          && self->crop[CROP_LEFT] == 0)
        new_dimension (self, analysis->width, analysis->height);
      return;
    }

  if (!kasasa_crop_matches_aspect_ratio (&analysis->crop,
                                         self->expected_width,
                                         self->expected_height,
                                         CROP_ASPECT_TOLERANCE))
    {
      g_debug ("Skipping pixel crop because it does not match the Portal size");
      if (self->crop[CROP_TOP] == 0
          && self->crop[CROP_RIGHT] == 0
          && self->crop[CROP_BOTTOM] == 0
          && self->crop[CROP_LEFT] == 0)
        new_dimension (self, analysis->width, analysis->height);
      return;
    }

  crop_changed = self->crop[CROP_TOP] != analysis->crop.top
                 || self->crop[CROP_RIGHT] != analysis->crop.right
                 || self->crop[CROP_BOTTOM] != analysis->crop.bottom
                 || self->crop[CROP_LEFT] != analysis->crop.left;

  self->crop[CROP_TOP] = analysis->crop.top;
  self->crop[CROP_RIGHT] = analysis->crop.right;
  self->crop[CROP_BOTTOM] = analysis->crop.bottom;
  self->crop[CROP_LEFT] = analysis->crop.left;

  new_dimension (self, analysis->crop.width, analysis->crop.height);

  g_debug ("Crop values: top: %d, bottom: %d, left: %d, right: %d",
           self->crop[CROP_TOP], self->crop[CROP_BOTTOM],
           self->crop[CROP_LEFT], self->crop[CROP_RIGHT]);

  g_debug ("Dimensions: width %d, height: %d",
           self->dimension[DIMENSION_WIDTH], self->dimension[DIMENSION_HEIGHT]);

  if (crop_changed)
    set_crop (self);
}

static void
crop_analysis_completed (GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (source_object);
  g_autofree CropAnalysis *analysis =
    g_task_propagate_pointer (G_TASK (result), NULL);

  self->crop_analysis_in_progress = FALSE;
  if (analysis != NULL)
    apply_crop_analysis (self, analysis);
}

static gboolean
start_crop_analysis (KasasaScreencast *self)
{
  g_autoptr (GstElement) fakesink = NULL;
  g_autoptr (GstSample) sample = NULL;
  GTask *task;

  if (self->finished || self->pipeline == NULL
      || self->crop_analysis_in_progress)
    return FALSE;

  fakesink = gst_bin_get_by_name (GST_BIN (self->pipeline), "fakesink");
  if (fakesink == NULL)
    {
      g_warning ("Unable to find the screencast frame sink");
      stop_periodic_crop_analysis (self);
      return FALSE;
    }

  g_object_get (fakesink, "last-sample", &sample, NULL);
  if (sample == NULL)
    {
      g_debug ("No screencast frame available for crop analysis yet");
      return FALSE;
    }

  self->crop_analysis_in_progress = TRUE;
  task = g_task_new (self, NULL, crop_analysis_completed, NULL);
  g_task_set_task_data (task,
                        g_steal_pointer (&sample),
                        (GDestroyNotify) gst_sample_unref);
  g_task_run_in_thread (task, analyze_crop_sample_task);
  g_object_unref (task);

  return TRUE;
}

static void
compute_first_crop_values (gpointer user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (user_data);

  if (!start_crop_analysis (self) && !self->finished
      && !self->crop_analysis_in_progress)
    kasasa_source_set_timeout_once (&self->first_crop_source,
                                    FIRST_CROP_CHECK_INTERVAL,
                                    compute_first_crop_values,
                                    self);
}

static gboolean
compute_periodic_crop_values (gpointer user_data)
{
  KasasaScreencast *self = KASASA_SCREENCAST (user_data);

  if (self->finished)
    {
      self->cropping_source = 0;
      return G_SOURCE_REMOVE;
    }

  if (!self->crop_analysis_in_progress)
    {
      kasasa_source_clear (&self->first_crop_source);
      start_crop_analysis (self);
    }

  return G_SOURCE_CONTINUE;
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

gboolean
kasasa_screencast_show (KasasaScreencast *self,
                        XdpSession       *session,
                        gint              fd,
                        guint             node_id,
                        gint              expected_width,
                        gint              expected_height,
                        GError           **error)

{
  g_autofree gchar *node_id_str = NULL;
  GstElement *pipewire_element = NULL;
  GstElement *filter = NULL;
  GstElement *videocrop = NULL;
  GstElement *gtksink = NULL;
  GstElement *sink = NULL;
  GstElement *convert = NULL;
  g_autoptr (GstCaps) caps = NULL;
  GstElement *tee = NULL;
  GstElement *queue1 = NULL;
  GstElement *queue2 = NULL;
  GstElement *fakesink = NULL;
  g_autoptr (GdkGLContext) gl_context = NULL;
  g_autoptr (GdkPaintable) paintable = NULL;
  GstStateChangeReturn ret;
  gboolean use_gl;

  g_return_val_if_fail (KASASA_IS_SCREENCAST (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  self->finished = FALSE;
  self->session = session;
  self->expected_width = expected_width;
  self->expected_height = expected_height;

  if (session == NULL || fd < 0 || self->pipeline != NULL)
    {
      if (fd >= 0)
        close (fd);
      return set_screencast_error (error,
                                   G_IO_ERROR_INVALID_ARGUMENT,
                                   _("Invalid screencast connection"));
    }

  if (expected_width > 0 && expected_height > 0)
    new_dimension (self, expected_width, expected_height);

  node_id_str = g_strdup_printf ("%u", node_id);

  self->pipeline = gst_pipeline_new ("pipeline");
  pipewire_element = gst_element_factory_make ("pipewiresrc", "pipewire_element");
  gtksink = gst_element_factory_make ("gtk4paintablesink", "sink");
  videocrop = gst_element_factory_make ("videocrop", "videocrop");
  caps = gst_caps_from_string ("video/x-raw");
  filter = gst_element_factory_make ("capsfilter", "filter");
  tee = gst_element_factory_make ("tee", "tee");
  queue1 = gst_element_factory_make ("queue", "queue1");
  queue2 = gst_element_factory_make ("queue", "queue2");
  fakesink = gst_element_factory_make ("fakesink", "fakesink");

  if (self->pipeline == NULL || pipewire_element == NULL || tee == NULL
      || queue1 == NULL || filter == NULL || videocrop == NULL
      || gtksink == NULL || queue2 == NULL || fakesink == NULL || caps == NULL)
    goto ELEMENT_ERROR;

  g_object_set (filter, "caps", caps, NULL);

  g_object_get (gtksink, "paintable", &paintable, NULL);
  if (paintable == NULL)
    goto ELEMENT_ERROR;

  g_object_get (paintable, "gl-context", &gl_context, NULL);
  use_gl = gl_context != NULL;

  if (use_gl)
    sink = gst_element_factory_make ("glsinkbin", "glsinkbin");
  else
    convert = gst_element_factory_make ("videoconvert", "convert");

  if ((use_gl && sink == NULL) || (!use_gl && convert == NULL))
    goto ELEMENT_ERROR;

  g_object_set (pipewire_element,
                "fd",  fd,
                "path", node_id_str,
                NULL);

  g_debug ("fd: %d; node_id: %s", fd, node_id_str);

  if (use_gl)
    {
      g_info ("Using GL");
      g_object_set (sink, "sink", gtksink, NULL);
    }
  else
    g_info ("Not using GL");

  gst_bin_add_many (GST_BIN (self->pipeline),
                    pipewire_element, tee, queue1, filter, videocrop,
                    queue2, fakesink, NULL);
  if (use_gl)
    gst_bin_add (GST_BIN (self->pipeline), sink);
  else
    gst_bin_add_many (GST_BIN (self->pipeline), convert, gtksink, NULL);

  if (!gst_element_link_many (pipewire_element,
                              tee, queue1, filter, videocrop, NULL)
      || (use_gl && !gst_element_link (videocrop, sink))
      || (!use_gl && !gst_element_link_many (videocrop,
                                             convert,
                                             gtksink,
                                             NULL))
      || !gst_element_link_many (tee, queue2, fakesink, NULL))
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
  gtk_stack_set_visible_child (self->stack, GTK_WIDGET (self->picture));

  self->closed_handler_id = g_signal_connect (self->session,
                                              "closed",
                                              G_CALLBACK (on_session_closed),
                                              self);

  kasasa_source_set_timeout_once (&self->first_crop_source,
                                  FIRST_CROP_CHECK_INTERVAL,
                                  compute_first_crop_values,
                                  self);
  self->cropping_source = g_timeout_add_seconds (CROP_CHEK_INTERVAL,
                                                  compute_periodic_crop_values,
                                                  self);

  return TRUE;

ELEMENT_ERROR:
  unref_element (pipewire_element);
  unref_element (filter);
  unref_element (videocrop);
  unref_element (gtksink);
  unref_element (sink);
  unref_element (convert);
  unref_element (tee);
  unref_element (queue1);
  unref_element (queue2);
  unref_element (fakesink);
  close (fd);

  return set_screencast_error (error,
                               G_IO_ERROR_NOT_SUPPORTED,
                               _("Required GStreamer plugins are unavailable"));
}

static void
kasasa_screencast_dispose (GObject *object)
{
  KasasaScreencast *self = KASASA_SCREENCAST (object);

  kasasa_screencast_finish (KASASA_CONTENT (self));

  if (self->pipeline)
    {
      gst_element_set_state (self->pipeline, GST_STATE_NULL);
      gst_object_unref (self->pipeline);
      self->pipeline = NULL;
    }

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

  gst_init (NULL, NULL);

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

  object_class->dispose = kasasa_screencast_dispose;

  widget_class->measure = kasasa_screencast_measure;
  widget_class->size_allocate = kasasa_screencast_size_allocate;
}

static void
kasasa_screencast_init (KasasaScreencast *self)
{
  self->pipeline = NULL;
  self->bus = NULL;
  self->first_crop_source.id = 0;
  self->cropping_source = 0;
  self->crop_analysis_in_progress = FALSE;
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

// https://gitlab.freedesktop.org/gstreamer/gst-plugins-rs/-/tree/main/video/gtk4/examples?ref_type=heads
// https://github.com/bilelmoussaoui/ashpd/blob/master/examples/screen_cast_gstreamer.rs
