/* kasasa-screencast-pipeline.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-screencast-pipeline.h"

#include <unistd.h>

static GstElement *
add_element (GstElement  *pipeline,
             const gchar *factory,
             const gchar *name)
{
  GstElement *element;

  element = gst_element_factory_make (factory, name);
  if (element == NULL)
    return NULL;

  if (!gst_bin_add (GST_BIN (pipeline), element))
    {
      gst_object_unref (element);
      return NULL;
    }

  return element;
}

static gboolean
factory_available (const gchar *name)
{
  GstElementFactory *factory;

  factory = gst_element_factory_find (name);
  if (factory == NULL)
    return FALSE;

  gst_object_unref (factory);
  return TRUE;
}

gboolean
kasasa_screencast_pipeline_gpu_available (void)
{
  static const gchar *required_factories[] = {
    "pipewiresrc",
    "glupload",
    "glcolorconvert",
    "gtk4paintablesink",
    NULL,
  };
  guint i;

  for (i = 0; required_factories[i] != NULL; i++)
    {
      if (!factory_available (required_factories[i]))
        return FALSE;
    }

  return TRUE;
}

static gboolean
build_gpu_pipeline (GstElement *pipeline,
                    GstElement *pipewire,
                    GstElement *gtksink)
{
  GstElement *upload;
  GstElement *gl_convert;
  GstElement *gl_filter;
  GstElement *display_queue;
  g_autoptr (GstCaps) gl_caps = NULL;

  display_queue = add_element (pipeline, "queue", "display_queue");
  upload = add_element (pipeline, "glupload", "glupload");
  gl_convert = add_element (pipeline, "glcolorconvert", "glcolorconvert");
  gl_filter = add_element (pipeline, "capsfilter", "gl_filter");
  gl_caps = gst_caps_from_string ("video/x-raw(memory:GLMemory),format=RGBA");

  if (upload == NULL || gl_convert == NULL || gl_filter == NULL
      || display_queue == NULL || gl_caps == NULL)
    return FALSE;

  g_object_set (gl_filter, "caps", gl_caps, NULL);
  g_object_set (display_queue,
                "leaky", 2,
                "max-size-buffers", 2,
                "max-size-bytes", 0,
                "max-size-time", (guint64) 0,
                NULL);

  return gst_element_link_many (pipewire,
                                display_queue,
                                upload,
                                gl_convert,
                                gl_filter,
                                gtksink,
                                NULL);
}

static gboolean
build_cpu_pipeline (GstElement *pipeline,
                    GstElement *pipewire,
                    GstElement *gtksink)
{
  GstElement *display_queue;
  GstElement *display_convert;

  display_queue = add_element (pipeline, "queue", "display_queue");
  display_convert = add_element (pipeline, "videoconvert", "display_convert");

  if (display_queue == NULL || display_convert == NULL)
    return FALSE;

  g_object_set (display_queue,
                "leaky", 2,
                "max-size-buffers", 2,
                "max-size-bytes", 0,
                "max-size-time", (guint64) 0,
                NULL);

  return gst_element_link_many (pipewire,
                                display_queue,
                                display_convert,
                                gtksink,
                                NULL);
}

gboolean
kasasa_screencast_pipeline_build_portal (
  gint                         fd,
  guint                        node_id,
  KasasaScreencastPipelineMode mode,
  KasasaScreencastPipeline    *result,
  GError                     **error)
{
  GstElement *pipewire;
  GstElement *gtksink;
  gboolean linked;
  gboolean fd_transferred = FALSE;
  g_autofree gchar *node_id_str = NULL;

  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (result->pipeline == NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  node_id_str = g_strdup_printf ("%u", node_id);
  result->pipeline = gst_pipeline_new (
    mode == KASASA_SCREENCAST_PIPELINE_GPU ? "portal-gpu-pipeline"
                                          : "portal-cpu-pipeline");
  if (result->pipeline == NULL)
    goto ELEMENT_ERROR;

  pipewire = add_element (result->pipeline,
                          "pipewiresrc",
                          "pipewire_element");
  gtksink = gst_element_factory_make ("gtk4paintablesink", "sink");
  if (pipewire == NULL || gtksink == NULL)
    {
      if (gtksink != NULL)
        gst_object_unref (gtksink);
      goto ELEMENT_ERROR;
    }

  gst_object_ref_sink (gtksink);

  g_object_set (pipewire,
                "fd", fd,
                "path", node_id_str,
                "use-bufferpool", mode == KASASA_SCREENCAST_PIPELINE_GPU,
                NULL);
  fd_transferred = TRUE;
  g_object_set (gtksink, "sync", FALSE, NULL);
  g_object_get (gtksink, "paintable", &result->paintable, NULL);
  if (result->paintable == NULL)
    {
      gst_object_unref (gtksink);
      goto ELEMENT_ERROR;
    }

  if (!gst_bin_add (GST_BIN (result->pipeline), gtksink))
    {
      gst_object_unref (gtksink);
      goto ELEMENT_ERROR;
    }

  linked = mode == KASASA_SCREENCAST_PIPELINE_GPU
           ? build_gpu_pipeline (result->pipeline,
                                 pipewire,
                                 gtksink)
           : build_cpu_pipeline (result->pipeline, pipewire, gtksink);
  gst_object_unref (gtksink);
  if (!linked)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "The screencast pipeline could not be linked");
      kasasa_screencast_pipeline_clear (result);
      return FALSE;
    }

  return TRUE;

ELEMENT_ERROR:
  if (!fd_transferred && fd >= 0)
    close (fd);
  g_set_error_literal (error,
                       G_IO_ERROR,
                       G_IO_ERROR_NOT_SUPPORTED,
                       "Required GStreamer plugins are unavailable");
  kasasa_screencast_pipeline_clear (result);
  return FALSE;
}

void
kasasa_screencast_pipeline_clear (KasasaScreencastPipeline *pipeline)
{
  if (pipeline == NULL)
    return;

  g_clear_object (&pipeline->paintable);

  if (pipeline->pipeline != NULL)
    {
      gst_element_set_state (pipeline->pipeline, GST_STATE_NULL);
      gst_object_unref (pipeline->pipeline);
      pipeline->pipeline = NULL;
    }
}
