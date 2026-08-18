/* test-window-layout.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>

#include "kasasa-window-layout.h"

static KasasaWindowLayoutInput
default_input (void)
{
  return (KasasaWindowLayoutInput) {
    .content_width = 800,
    .content_height = 600,
    .monitor_width = 1920.0,
    .monitor_height = 1080.0,
    .content_scale = 1.0,
    .occupy_screen = 50,
    .zoom_factor = 1.0,
  };
}

static void
test_preserves_content_size_when_it_fits (void)
{
  KasasaWindowLayoutInput input = default_input ();
  KasasaWindowLayoutOutput output = { 0 };

  g_assert_true (kasasa_window_layout_compute (&input, &output));
  g_assert_cmpfloat (output.width, ==, 800.0);
  g_assert_cmpfloat (output.height, ==, 600.0);
  g_assert_cmpfloat (output.zoom_factor, ==, 1.0);
}

static void
test_scales_large_content_to_target_area (void)
{
  KasasaWindowLayoutInput input = default_input ();
  KasasaWindowLayoutOutput output = { 0 };

  input.content_width = 8000;
  input.content_height = 4000;

  g_assert_true (kasasa_window_layout_compute (&input, &output));
  g_assert_cmpfloat_with_epsilon (output.width, 1440.0, 1.0);
  g_assert_cmpfloat_with_epsilon (output.height, 720.0, 1.0);
}

static void
test_uses_content_scale (void)
{
  KasasaWindowLayoutInput input = default_input ();
  KasasaWindowLayoutOutput output = { 0 };

  input.content_width = 2000;
  input.content_height = 1000;
  input.content_scale = 2.0;

  g_assert_true (kasasa_window_layout_compute (&input, &output));
  g_assert_cmpfloat (output.width, ==, 1000.0);
  g_assert_cmpfloat (output.height, ==, 500.0);
}

static void
test_clamps_zoom_to_visible_range (void)
{
  KasasaWindowLayoutInput input = default_input ();
  KasasaWindowLayoutOutput output = { 0 };

  input.content_width = 100;
  input.content_height = 50;
  input.zoom_factor = KASASA_WINDOW_LAYOUT_ZOOM_MIN;

  g_assert_true (kasasa_window_layout_compute (&input, &output));
  g_assert_cmpfloat (output.zoom_factor, ==, 2.2);
  g_assert_cmpfloat (output.width, ==, 220.0);
  g_assert_cmpfloat (output.height, ==, KASASA_WINDOW_LAYOUT_MIN_HEIGHT);
}

static void
test_rejects_invalid_input (void)
{
  KasasaWindowLayoutInput input = default_input ();
  KasasaWindowLayoutOutput output = { 0 };

  input.content_width = 0;
  g_assert_false (kasasa_window_layout_compute (&input, &output));

  input = default_input ();
  input.occupy_screen = 0;
  g_assert_false (kasasa_window_layout_compute (&input, &output));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/window-layout/preserves-fitting-content",
                   test_preserves_content_size_when_it_fits);
  g_test_add_func ("/window-layout/scales-large-content",
                   test_scales_large_content_to_target_area);
  g_test_add_func ("/window-layout/content-scale", test_uses_content_scale);
  g_test_add_func ("/window-layout/zoom-range",
                   test_clamps_zoom_to_visible_range);
  g_test_add_func ("/window-layout/invalid-input", test_rejects_invalid_input);

  return g_test_run ();
}
