/* kasasa-zoom.c
 *
 * Copyright 2024-2026 Kelvin Novais
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

#include "kasasa-zoom.h"

#include <math.h>

KasasaScrollAxis
kasasa_zoom_classify_scroll (gdouble          dx,
                             gdouble          dy,
                             KasasaScrollAxis current_axis)
{
  if (current_axis != KASASA_SCROLL_AXIS_UNDECIDED)
    return current_axis;

  if (!isfinite (dx) || !isfinite (dy) || (dx == 0.0 && dy == 0.0))
    return KASASA_SCROLL_AXIS_UNDECIDED;

  return ABS (dx) > ABS (dy)
         ? KASASA_SCROLL_AXIS_HORIZONTAL
         : KASASA_SCROLL_AXIS_VERTICAL;
}

gboolean
kasasa_zoom_get_logical_content_size (gint     content_width,
                                      gint     content_height,
                                      gdouble  scale,
                                      gdouble *logical_width,
                                      gdouble *logical_height)
{
  g_return_val_if_fail (logical_width != NULL, FALSE);
  g_return_val_if_fail (logical_height != NULL, FALSE);

  if (content_width <= 0 || content_height <= 0
      || !isfinite (scale) || scale <= 0.0)
    return FALSE;

  *logical_width = content_width / scale;
  *logical_height = content_height / scale;

  return isfinite (*logical_width) && isfinite (*logical_height)
         && *logical_width > 0.0 && *logical_height > 0.0;
}

gdouble
kasasa_zoom_apply_delta (gdouble         current,
                         gdouble         lower,
                         gdouble         upper,
                         gdouble         delta,
                         KasasaZoomInput input)
{
  gdouble steps;
  gdouble next;

  g_return_val_if_fail (current > 0.0, current);
  g_return_val_if_fail (lower > 0.0, current);
  g_return_val_if_fail (upper >= lower, current);

  if (!isfinite (delta) || delta == 0.0)
    return CLAMP (current, lower, upper);

  steps = input == KASASA_ZOOM_INPUT_SURFACE
          ? delta / KASASA_ZOOM_SURFACE_PIXELS_PER_STEP
          : delta;
  next = current * pow (KASASA_ZOOM_STEP, -steps);

  return CLAMP (next, lower, upper);
}

gdouble
kasasa_zoom_follow_value (gdouble current,
                          gdouble target,
                          gdouble elapsed_ms)
{
  gdouble progress;

  g_return_val_if_fail (isfinite (current), current);
  g_return_val_if_fail (isfinite (target), current);

  if (!isfinite (elapsed_ms) || elapsed_ms <= 0.0 || current == target)
    return current;

  /* Exponential following is independent of frame rate. Updating target while
   * scrolling changes direction continuously instead of restarting an easing. */
  progress = -expm1 (-elapsed_ms / KASASA_ZOOM_FOLLOW_TAU_MS);

  return current + (target - current) * CLAMP (progress, 0.0, 1.0);
}
