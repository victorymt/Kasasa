/* kasasa-frame-transform.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include <wayland-client-protocol.h>

static inline guint32
kasasa_frame_transform_normalize (guint32 transform)
{
  return transform <= WL_OUTPUT_TRANSFORM_FLIPPED_270
         ? transform
         : WL_OUTPUT_TRANSFORM_NORMAL;
}

static inline void
kasasa_frame_transform_dimensions (gint     width,
                                   gint     height,
                                   guint32  transform,
                                   gint    *output_width,
                                   gint    *output_height)
{
  if (transform == WL_OUTPUT_TRANSFORM_90
      || transform == WL_OUTPUT_TRANSFORM_270
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
      *output_width = height;
      *output_height = width;
    }
  else
    {
      *output_width = width;
      *output_height = height;
    }
}

static inline void
kasasa_frame_transform_source_position (guint32  transform,
                                        gint     width,
                                        gint     height,
                                        gint     output_x,
                                        gint     output_y,
                                        gint    *source_x,
                                        gint    *source_y)
{
  switch (transform)
    {
    case WL_OUTPUT_TRANSFORM_90:
      *source_x = width - 1 - output_y;
      *source_y = output_x;
      break;
    case WL_OUTPUT_TRANSFORM_180:
      *source_x = width - 1 - output_x;
      *source_y = height - 1 - output_y;
      break;
    case WL_OUTPUT_TRANSFORM_270:
      *source_x = output_y;
      *source_y = height - 1 - output_x;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      *source_x = width - 1 - output_x;
      *source_y = output_y;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      *source_x = output_y;
      *source_y = output_x;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      *source_x = output_x;
      *source_y = height - 1 - output_y;
      break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      *source_x = width - 1 - output_y;
      *source_y = height - 1 - output_x;
      break;
    case WL_OUTPUT_TRANSFORM_NORMAL:
    default:
      *source_x = output_x;
      *source_y = output_y;
      break;
    }
}
