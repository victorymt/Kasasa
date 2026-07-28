/* kasasa-screencast-pipeline.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum
{
  KASASA_SCREENCAST_PIPELINE_CPU,
  KASASA_SCREENCAST_PIPELINE_GPU,
} KasasaScreencastPipelineMode;

typedef struct
{
  GstElement *pipeline;
  GdkPaintable *paintable;
} KasasaScreencastPipeline;

gboolean kasasa_screencast_pipeline_gpu_available (void);

gboolean kasasa_screencast_pipeline_build_portal (
  gint                         fd,
  guint                        node_id,
  KasasaScreencastPipelineMode mode,
  KasasaScreencastPipeline    *result,
  GError                     **error);

void kasasa_screencast_pipeline_clear (KasasaScreencastPipeline *pipeline);

G_END_DECLS
