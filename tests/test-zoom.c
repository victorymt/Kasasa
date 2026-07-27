/* test-zoom.c
 *
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

#include <glib.h>
#include <math.h>

#include "kasasa-zoom.h"

static void
test_scroll_axis_classification (void)
{
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (12.0, 0.8,
                                 KASASA_SCROLL_AXIS_UNDECIDED),
    ==,
    KASASA_SCROLL_AXIS_HORIZONTAL);
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (0.6, 8.0,
                                 KASASA_SCROLL_AXIS_UNDECIDED),
    ==,
    KASASA_SCROLL_AXIS_VERTICAL);
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (4.0, 4.0,
                                 KASASA_SCROLL_AXIS_UNDECIDED),
    ==,
    KASASA_SCROLL_AXIS_VERTICAL);
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (0.0, 0.0,
                                 KASASA_SCROLL_AXIS_UNDECIDED),
    ==,
    KASASA_SCROLL_AXIS_UNDECIDED);
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (NAN, 1.0,
                                 KASASA_SCROLL_AXIS_UNDECIDED),
    ==,
    KASASA_SCROLL_AXIS_UNDECIDED);
}

static void
test_scroll_axis_is_sticky (void)
{
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (0.5, 10.0,
                                 KASASA_SCROLL_AXIS_HORIZONTAL),
    ==,
    KASASA_SCROLL_AXIS_HORIZONTAL);
  g_assert_cmpint (
    kasasa_zoom_classify_scroll (10.0, 0.5,
                                 KASASA_SCROLL_AXIS_VERTICAL),
    ==,
    KASASA_SCROLL_AXIS_VERTICAL);
}

static void
test_logical_content_size (void)
{
  gdouble width;
  gdouble height;

  g_assert_true (kasasa_zoom_get_logical_content_size (1, 1, 2.0,
                                                        &width, &height));
  g_assert_cmpfloat (width, ==, 0.5);
  g_assert_cmpfloat (height, ==, 0.5);

  g_assert_true (kasasa_zoom_get_logical_content_size (G_MAXINT, G_MAXINT,
                                                        1.0,
                                                        &width, &height));
  g_assert_cmpfloat (width * height, >, G_MAXINT);
}

static void
test_invalid_logical_content_size (void)
{
  gdouble width;
  gdouble height;

  g_assert_false (kasasa_zoom_get_logical_content_size (0, 1, 1.0,
                                                         &width, &height));
  g_assert_false (kasasa_zoom_get_logical_content_size (1, -1, 1.0,
                                                         &width, &height));
  g_assert_false (kasasa_zoom_get_logical_content_size (1, 1, 0.0,
                                                         &width, &height));
  g_assert_false (kasasa_zoom_get_logical_content_size (1, 1, NAN,
                                                         &width, &height));
}

static void
test_wheel_delta (void)
{
  gdouble zoom;

  zoom = kasasa_zoom_apply_delta (1.0, 0.25, 4.0,
                                  1.0, KASASA_ZOOM_INPUT_WHEEL);
  g_assert_cmpfloat_with_epsilon (zoom, 1.0 / KASASA_ZOOM_STEP, 0.000001);

  zoom = kasasa_zoom_apply_delta (1.0, 0.25, 4.0,
                                  -1.0, KASASA_ZOOM_INPUT_WHEEL);
  g_assert_cmpfloat_with_epsilon (zoom, KASASA_ZOOM_STEP, 0.000001);

  zoom = kasasa_zoom_apply_delta (1.0, 0.25, 4.0,
                                  2.0, KASASA_ZOOM_INPUT_WHEEL);
  g_assert_cmpfloat_with_epsilon (zoom,
                                  1.0 / (KASASA_ZOOM_STEP * KASASA_ZOOM_STEP),
                                  0.000001);
}

static void
test_surface_delta (void)
{
  gdouble small;
  gdouble full_step;

  small = kasasa_zoom_apply_delta (1.0, 0.25, 4.0,
                                   5.0, KASASA_ZOOM_INPUT_SURFACE);
  full_step = kasasa_zoom_apply_delta (
    1.0, 0.25, 4.0,
    KASASA_ZOOM_SURFACE_PIXELS_PER_STEP,
    KASASA_ZOOM_INPUT_SURFACE);

  g_assert_cmpfloat (small, <, 1.0);
  g_assert_cmpfloat (small, >, full_step);
  g_assert_cmpfloat_with_epsilon (full_step,
                                  1.0 / KASASA_ZOOM_STEP,
                                  0.000001);
}

static void
test_dynamic_bounds (void)
{
  gdouble zoom;

  zoom = kasasa_zoom_apply_delta (0.6, 0.6, 2.0,
                                  1.0, KASASA_ZOOM_INPUT_WHEEL);
  g_assert_cmpfloat (zoom, ==, 0.6);

  zoom = kasasa_zoom_apply_delta (2.0, 0.5, 2.0,
                                  -1.0, KASASA_ZOOM_INPUT_WHEEL);
  g_assert_cmpfloat (zoom, ==, 2.0);
}

static void
test_zero_delta_clamps_stale_value (void)
{
  g_assert_cmpfloat (kasasa_zoom_apply_delta (0.4, 0.5, 2.0, 0.0,
                                              KASASA_ZOOM_INPUT_WHEEL),
                     ==,
                     0.5);
}

static void
test_nonfinite_delta_is_ignored (void)
{
  g_assert_cmpfloat (kasasa_zoom_apply_delta (1.0, 0.5, 2.0, NAN,
                                              KASASA_ZOOM_INPUT_SURFACE),
                     ==,
                     1.0);
  g_assert_cmpfloat (kasasa_zoom_apply_delta (1.0, 0.5, 2.0, INFINITY,
                                              KASASA_ZOOM_INPUT_WHEEL),
                     ==,
                     1.0);
}

static void
assert_continuous_shrink_reaches_lower_bound (KasasaZoomInput input,
                                              gdouble         delta)
{
  const gdouble lower = 0.25;
  gdouble zoom = 1.0;
  guint changes = 0;

  for (guint i = 0; i < 2048; i++)
    {
      gdouble previous = zoom;

      zoom = kasasa_zoom_apply_delta (zoom, lower, 4.0, delta, input);
      g_assert_true (isfinite (zoom));
      g_assert_cmpfloat (zoom, <=, previous);
      g_assert_cmpfloat (zoom, >=, lower);
      if (zoom < previous)
        changes++;
    }

  g_assert_cmpuint (changes, >, 0);
  g_assert_cmpfloat_with_epsilon (zoom, lower, 0.000001);

  for (guint i = 0; i < 64; i++)
    g_assert_cmpfloat (kasasa_zoom_apply_delta (zoom,
                                               lower,
                                               4.0,
                                               delta,
                                               input),
                       ==,
                       lower);
}

static void
test_continuous_shrink (void)
{
  assert_continuous_shrink_reaches_lower_bound (KASASA_ZOOM_INPUT_WHEEL,
                                                1.0);
  assert_continuous_shrink_reaches_lower_bound (KASASA_ZOOM_INPUT_SURFACE,
                                                1.0);
}

static void
test_follow_is_frame_rate_independent (void)
{
  gdouble one_frame;
  gdouble two_frames;

  one_frame = kasasa_zoom_follow_value (800.0, 400.0, 32.0);
  two_frames = kasasa_zoom_follow_value (800.0, 400.0, 16.0);
  two_frames = kasasa_zoom_follow_value (two_frames, 400.0, 16.0);

  g_assert_cmpfloat_with_epsilon (one_frame, two_frames, 0.000001);
  g_assert_cmpfloat (one_frame, <, 800.0);
  g_assert_cmpfloat (one_frame, >, 400.0);
}

static void
test_follow_retargets_without_reset (void)
{
  gdouble current;
  gdouble retargeted;

  current = kasasa_zoom_follow_value (800.0, 400.0, 16.0);
  retargeted = kasasa_zoom_follow_value (current, 1000.0, 16.0);

  g_assert_cmpfloat (retargeted, >, current);
  g_assert_cmpfloat (retargeted, <, 1000.0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/zoom/scroll-axis", test_scroll_axis_classification);
  g_test_add_func ("/zoom/scroll-axis-sticky", test_scroll_axis_is_sticky);
  g_test_add_func ("/zoom/logical-content-size", test_logical_content_size);
  g_test_add_func ("/zoom/logical-content-size-invalid",
                   test_invalid_logical_content_size);
  g_test_add_func ("/zoom/wheel-delta", test_wheel_delta);
  g_test_add_func ("/zoom/surface-delta", test_surface_delta);
  g_test_add_func ("/zoom/dynamic-bounds", test_dynamic_bounds);
  g_test_add_func ("/zoom/zero-delta", test_zero_delta_clamps_stale_value);
  g_test_add_func ("/zoom/nonfinite-delta", test_nonfinite_delta_is_ignored);
  g_test_add_func ("/zoom/continuous-shrink", test_continuous_shrink);
  g_test_add_func ("/zoom/follow-frame-rate", test_follow_is_frame_rate_independent);
  g_test_add_func ("/zoom/follow-retarget", test_follow_retargets_without_reset);

  return g_test_run ();
}
