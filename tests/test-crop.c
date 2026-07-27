/* test-crop.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>

#include "kasasa-crop.h"

#define BYTES_PER_PIXEL 4

static void
set_pixel (guint8 *data,
           gsize   stride,
           gint    x,
           gint    y,
           guint8  blue,
           guint8  green,
           guint8  red)
{
  guint8 *pixel = data + (gsize) y * stride
                  + (gsize) x * BYTES_PER_PIXEL;

  pixel[0] = blue;
  pixel[1] = green;
  pixel[2] = red;
}

static void
assert_crop (const KasasaCrop *crop,
             gint              top,
             gint              right,
             gint              bottom,
             gint              left,
             gint              width,
             gint              height)
{
  g_assert_cmpint (crop->top, ==, top);
  g_assert_cmpint (crop->right, ==, right);
  g_assert_cmpint (crop->bottom, ==, bottom);
  g_assert_cmpint (crop->left, ==, left);
  g_assert_cmpint (crop->width, ==, width);
  g_assert_cmpint (crop->height, ==, height);
}

static void
test_empty_frame (void)
{
  guint8 data[3 * 16] = { 0 };
  KasasaCrop crop;

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            4,
                                            3,
                                            16,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_EMPTY);
}

static void
test_content_bounds (void)
{
  guint8 data[4 * 20] = { 0 };
  KasasaCrop crop;

  set_pixel (data, 20, 1, 1, 1, 0, 0);
  set_pixel (data, 20, 3, 2, 0, 1, 0);

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            5,
                                            4,
                                            20,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_FOUND);
  assert_crop (&crop, 1, 1, 1, 1, 3, 2);
}

static void
test_edge_pixels (void)
{
  guint8 data[3 * 16] = { 0 };
  KasasaCrop crop;

  set_pixel (data, 16, 0, 0, 1, 1, 1);
  set_pixel (data, 16, 3, 2, 1, 1, 1);

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            4,
                                            3,
                                            16,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_FOUND);
  assert_crop (&crop, 0, 0, 0, 0, 4, 3);
}

static void
test_padded_stride (void)
{
  guint8 data[3 * 16] = { 0 };
  KasasaCrop crop;

  for (gint y = 0; y < 3; y++)
    for (gint i = 12; i < 16; i++)
      data[y * 16 + i] = 255;

  set_pixel (data, 16, 1, 1, 1, 0, 0);

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            3,
                                            3,
                                            16,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_FOUND);
  assert_crop (&crop, 1, 1, 1, 1, 1, 1);
}

static void
test_padding_is_not_content (void)
{
  guint8 data[3 * 16] = { 0 };
  KasasaCrop crop;

  for (gint y = 0; y < 3; y++)
    for (gint i = 12; i < 16; i++)
      data[y * 16 + i] = 255;

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            3,
                                            3,
                                            16,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_EMPTY);
}

static void
test_rejects_short_buffer (void)
{
  guint8 data[31] = { 0 };
  KasasaCrop crop;

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            4,
                                            2,
                                            16,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_INVALID);
}

static void
test_rejects_short_stride (void)
{
  guint8 data[32] = { 0 };
  KasasaCrop crop;

  g_assert_cmpint (kasasa_crop_find_rgb32 (data,
                                            sizeof data,
                                            4,
                                            2,
                                            15,
                                            &crop),
                   ==,
                   KASASA_CROP_RESULT_INVALID);
}

static void
test_crop_aspect_ratio_matches_portal_size (void)
{
  KasasaCrop crop = {
    .width = 1920,
    .height = 1080,
  };

  g_assert_true (kasasa_crop_matches_aspect_ratio (&crop,
                                                   1280,
                                                   720,
                                                   0.02));

  crop.width = 300;
  crop.height = 200;
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop,
                                                    1280,
                                                    720,
                                                    0.02));
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop,
                                                    0,
                                                    720,
                                                    0.02));
}

static void
test_aspect_ratio_tolerance_boundary (void)
{
  KasasaCrop crop = {
    .width = 101,
    .height = 100,
  };
  const gdouble difference = 0.01;

  g_assert_true (kasasa_crop_matches_aspect_ratio (&crop,
                                                   100,
                                                   100,
                                                   difference + 0.0000001));
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop,
                                                    100,
                                                    100,
                                                    difference - 0.0000001));
}

static void
test_aspect_ratio_rejects_invalid_input (void)
{
  KasasaCrop crop = {
    .width = 100,
    .height = 100,
  };

  g_assert_false (kasasa_crop_matches_aspect_ratio (NULL, 100, 100, 0.02));
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop, 100, 100, -0.01));

  crop.width = 0;
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop, 100, 100, 0.02));
  crop.width = 100;
  crop.height = 0;
  g_assert_false (kasasa_crop_matches_aspect_ratio (&crop, 100, 100, 0.02));
}

static void
test_aspect_ratio_large_dimensions (void)
{
  KasasaCrop crop = {
    .width = G_MAXINT,
    .height = G_MAXINT - 1,
  };

  g_assert_true (kasasa_crop_matches_aspect_ratio (&crop,
                                                   G_MAXINT,
                                                   G_MAXINT - 1,
                                                   0.0));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/crop/empty-frame", test_empty_frame);
  g_test_add_func ("/crop/content-bounds", test_content_bounds);
  g_test_add_func ("/crop/edge-pixels", test_edge_pixels);
  g_test_add_func ("/crop/padded-stride", test_padded_stride);
  g_test_add_func ("/crop/padding-is-not-content", test_padding_is_not_content);
  g_test_add_func ("/crop/rejects-short-buffer", test_rejects_short_buffer);
  g_test_add_func ("/crop/rejects-short-stride", test_rejects_short_stride);
  g_test_add_func ("/crop/portal-aspect-ratio",
                   test_crop_aspect_ratio_matches_portal_size);
  g_test_add_func ("/crop/aspect-ratio-tolerance-boundary",
                   test_aspect_ratio_tolerance_boundary);
  g_test_add_func ("/crop/aspect-ratio-invalid-input",
                   test_aspect_ratio_rejects_invalid_input);
  g_test_add_func ("/crop/aspect-ratio-large-dimensions",
                   test_aspect_ratio_large_dimensions);

  return g_test_run ();
}
