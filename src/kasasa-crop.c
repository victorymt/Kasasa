/* kasasa-crop.c
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

#include "kasasa-crop.h"

#define BYTES_PER_PIXEL 4

gboolean
kasasa_crop_matches_aspect_ratio (const KasasaCrop *crop,
                                  gint              expected_width,
                                  gint              expected_height,
                                  gdouble           tolerance)
{
  gdouble actual_ratio;
  gdouble expected_ratio;

  if (crop == NULL || crop->width <= 0 || crop->height <= 0
      || expected_width <= 0 || expected_height <= 0 || tolerance < 0.0)
    return FALSE;

  actual_ratio = (gdouble) crop->width / (gdouble) crop->height;
  expected_ratio = (gdouble) expected_width / (gdouble) expected_height;

  return ABS (actual_ratio - expected_ratio) / expected_ratio <= tolerance;
}

KasasaCropResult
kasasa_crop_find_rgb32 (const guint8 *data,
                        gsize         size,
                        gint          width,
                        gint          height,
                        gsize         stride,
                        KasasaCrop   *crop)
{
  gsize row_size;
  gsize required_size;
  gint top;
  gint bottom = -1;
  gint left;
  gint right = -1;

  if (data == NULL || crop == NULL || width <= 0 || height <= 0)
    return KASASA_CROP_RESULT_INVALID;

  if ((gsize) width > G_MAXSIZE / BYTES_PER_PIXEL)
    return KASASA_CROP_RESULT_INVALID;

  row_size = (gsize) width * BYTES_PER_PIXEL;
  if (stride < row_size)
    return KASASA_CROP_RESULT_INVALID;

  if (height > 1
      && stride > (G_MAXSIZE - row_size) / ((gsize) height - 1))
    return KASASA_CROP_RESULT_INVALID;

  required_size = ((gsize) height - 1) * stride + row_size;
  if (size < required_size)
    return KASASA_CROP_RESULT_INVALID;

  top = height;
  left = width;

  for (gint y = 0; y < height; y++)
    {
      const guint8 *row = data + (gsize) y * stride;

      for (gint x = 0; x < width; x++)
        {
          const guint8 *pixel = row + (gsize) x * BYTES_PER_PIXEL;

          if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)
            {
              top = MIN (top, y);
              bottom = MAX (bottom, y);
              left = MIN (left, x);
              right = MAX (right, x);
            }
        }
    }

  if (bottom < 0 || right < 0)
    return KASASA_CROP_RESULT_EMPTY;

  crop->top = top;
  crop->right = width - 1 - right;
  crop->bottom = height - 1 - bottom;
  crop->left = left;
  crop->width = right - left + 1;
  crop->height = bottom - top + 1;

  return KASASA_CROP_RESULT_FOUND;
}
