/* test-image.c
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
#include <glib/gstdio.h>
#include <unistd.h>

#include "kasasa-image.h"

static const guint8 png_1x1[] = {
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
  0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c,
  0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
  0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00,
  0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
  0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
  0xae, 0x42, 0x60, 0x82
};

static gchar *
create_temp_file (const guint8 *contents,
                  gsize         length)
{
  g_autoptr (GError) error = NULL;
  gchar *path = NULL;
  gint fd;

  fd = g_file_open_tmp ("kasasa-image-test-XXXXXX.png", &path, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);

  g_assert_true (g_file_set_contents (path,
                                      (const gchar *) contents,
                                      length,
                                      &error));
  g_assert_no_error (error);

  return path;
}

static void
test_load_valid_png (void)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  g_autoptr (GError) error = NULL;

  path = create_temp_file (png_1x1, sizeof png_1x1);
  uri = g_filename_to_uri (path, NULL, &error);
  g_assert_no_error (error);

  g_assert_true (kasasa_image_load_uri (uri, &file, &texture, &error));
  g_assert_no_error (error);
  g_assert_nonnull (file);
  g_assert_nonnull (texture);
  g_assert_cmpint (gdk_texture_get_width (texture), ==, 1);
  g_assert_cmpint (gdk_texture_get_height (texture), ==, 1);

  g_assert_cmpint (g_remove (path), ==, 0);
}

static void
test_reject_invalid_image (void)
{
  static const guint8 invalid_image[] = "not an image";
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  g_autoptr (GError) error = NULL;

  path = create_temp_file (invalid_image, sizeof invalid_image - 1);
  uri = g_filename_to_uri (path, NULL, &error);
  g_assert_no_error (error);

  g_assert_false (kasasa_image_load_uri (uri, &file, &texture, &error));
  g_assert_nonnull (error);
  g_assert_null (file);
  g_assert_null (texture);

  g_assert_cmpint (g_remove (path), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/image/load-valid-png", test_load_valid_png);
  g_test_add_func ("/image/reject-invalid-image", test_reject_invalid_image);

  return g_test_run ();
}
