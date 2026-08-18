/* kasasa-window-layout.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-window-layout.h"

#include <math.h>

#include "kasasa-zoom.h"

#define MIN_OCCUPY_SCREEN 0.1

gboolean
kasasa_window_layout_compute (const KasasaWindowLayoutInput *input,
                              KasasaWindowLayoutOutput      *output)
{
  gdouble image_width;
  gdouble image_height;
  gdouble image_area;
  gdouble monitor_area;
  gdouble max_width;
  gdouble max_height;
  gdouble occupy_area_factor;
  gdouble size_scale;
  gdouble target_scale;
  gdouble min_zoom;
  gdouble max_zoom;
  gdouble zoom_lower;
  gdouble zoom_upper;

  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (output != NULL, FALSE);

  if (input->content_width <= 0 || input->content_height <= 0
      || !isfinite (input->monitor_width)
      || !isfinite (input->monitor_height)
      || input->monitor_width <= 0.0 || input->monitor_height <= 0.0
      || !isfinite (input->content_scale) || input->content_scale <= 0.0
      || !isfinite (input->zoom_factor)
      || input->occupy_screen <= 0)
    return FALSE;

  if (!kasasa_zoom_get_logical_content_size (input->content_width,
                                             input->content_height,
                                             input->content_scale,
                                             &image_width,
                                             &image_height))
    return FALSE;

  monitor_area = input->monitor_width * input->monitor_height;
  image_area = image_width * image_height;
  occupy_area_factor = input->occupy_screen / 100.0;

  if (!isfinite (monitor_area) || !isfinite (image_area)
      || image_area <= 0.0 || !isfinite (occupy_area_factor)
      || occupy_area_factor <= 0.0)
    return FALSE;

  size_scale = sqrt (monitor_area / image_area * occupy_area_factor);
  if (!isfinite (size_scale))
    return FALSE;

  size_scale = MAX (size_scale, MIN_OCCUPY_SCREEN);
  target_scale = MIN (1.0, size_scale);
  output->width = image_width * target_scale;
  output->height = image_height * target_scale;

  /* Keep the image inside the current monitor, leaving room for the header. */
  max_width = MAX (1.0, input->monitor_width);
  max_height = MAX (1.0, input->monitor_height - 35.0);
  if (output->width > max_width)
    {
      output->width = max_width;
      output->height = image_height * output->width / image_width;
    }
  if (output->height > max_height)
    {
      output->height = max_height;
      output->width = image_width * output->height / image_height;
    }

  min_zoom = MAX (KASASA_WINDOW_LAYOUT_MIN_WIDTH / output->width,
                  KASASA_WINDOW_LAYOUT_MIN_HEIGHT / output->height);
  max_zoom = MIN (max_width / output->width,
                  max_height / output->height);
  zoom_lower = MAX (KASASA_WINDOW_LAYOUT_ZOOM_MIN, min_zoom);
  zoom_upper = MIN (KASASA_WINDOW_LAYOUT_ZOOM_MAX, max_zoom);
  if (zoom_lower > zoom_upper)
    zoom_lower = zoom_upper;

  output->zoom_min = zoom_lower;
  output->zoom_max = zoom_upper;
  output->zoom_factor = CLAMP (input->zoom_factor, zoom_lower, zoom_upper);
  output->width *= output->zoom_factor;
  output->height *= output->zoom_factor;

  if (output->width > max_width)
    {
      output->height *= max_width / output->width;
      output->width = max_width;
    }
  if (output->height > max_height)
    {
      output->width *= max_height / output->height;
      output->height = max_height;
    }

  if (!isfinite (output->width) || !isfinite (output->height)
      || output->width <= 0.0 || output->height <= 0.0)
    return FALSE;

  output->width = MAX (KASASA_WINDOW_LAYOUT_MIN_WIDTH, round (output->width));
  output->height = MAX (KASASA_WINDOW_LAYOUT_MIN_HEIGHT, round (output->height));

  return TRUE;
}
