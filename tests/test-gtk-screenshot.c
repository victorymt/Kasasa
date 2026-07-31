/* test-gtk-screenshot.c
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

#include <adwaita.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "kasasa-content.h"
#include "kasasa-dmabuf-paintable.h"
#include "kasasa-screencast.h"
#include "kasasa-window-query.h"
#include "kasasa-screenshot.h"
#include "kasasa-window.h"

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

static gboolean trash_active;

/* The component test isolates KasasaScreenshot from its application window. */
KasasaWindow *
kasasa_window_get_window_reference (GtkWidget *widget)
{
  return NULL;
}

gboolean
kasasa_window_get_trash_button_active (KasasaWindow *window)
{
  return trash_active;
}

static gchar *
create_temp_file (const guint8 *contents,
                  gsize         length)
{
  g_autoptr (GError) error = NULL;
  gchar *path = NULL;
  gint fd;

  fd = g_file_open_tmp ("kasasa-gtk-test-XXXXXX.png", &path, &error);
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
test_screenshot_layout (void)
{
  g_autoptr (KasasaScreenshot) screenshot = kasasa_screenshot_new ();
  GtkWidget *child = adw_bin_get_child (ADW_BIN (screenshot));
  gint minimum;
  gint natural;

  g_object_ref_sink (screenshot);

  g_assert_true (GTK_IS_PICTURE (child));
  g_assert_cmpint (gtk_picture_get_content_fit (GTK_PICTURE (child)),
                   ==,
                   GTK_CONTENT_FIT_CONTAIN);
  g_assert_true (gtk_picture_get_can_shrink (GTK_PICTURE (child)));
  g_assert_true (gtk_widget_get_hexpand (child));
  g_assert_true (gtk_widget_get_vexpand (child));
  g_assert_null (gtk_widget_get_layout_manager (GTK_WIDGET (screenshot)));

  gtk_widget_measure (GTK_WIDGET (screenshot),
                      GTK_ORIENTATION_HORIZONTAL,
                      -1,
                      &minimum,
                      &natural,
                      NULL,
                      NULL);
  g_assert_cmpint (minimum, ==, 0);
  g_assert_cmpint (natural, ==, 0);
}

static void
test_screencast_layout (void)
{
  g_autoptr (KasasaScreencast) screencast = kasasa_screencast_new ();
  GtkWidget *stack = adw_bin_get_child (ADW_BIN (screencast));
  GtkWidget *child;
  GtkPicture *picture = NULL;

  g_object_ref_sink (screencast);
  g_assert_true (GTK_IS_STACK (stack));

  for (child = gtk_widget_get_first_child (stack);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (GTK_IS_PICTURE (child))
        {
          picture = GTK_PICTURE (child);
          break;
        }
    }

  g_assert_nonnull (picture);
  g_assert_cmpint (gtk_picture_get_content_fit (picture),
                   ==,
                   GTK_CONTENT_FIT_CONTAIN);
}

static void
test_dmabuf_paintable_transform_dimensions (void)
{
  static const guint8 pixels[3 * 2 * 4] = { 0 };
  g_autoptr (GBytes) bytes = g_bytes_new_static (pixels, sizeof pixels);
  g_autoptr (GdkTexture) texture = NULL;
  g_autoptr (KasasaDmabufPaintable) paintable =
    kasasa_dmabuf_paintable_new ();
  GtkSnapshot *snapshot;
  GskRenderNode *node;

  texture = gdk_memory_texture_new (3,
                                    2,
                                    GDK_MEMORY_R8G8B8A8_PREMULTIPLIED,
                                    bytes,
                                    3 * 4);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   0);

  kasasa_dmabuf_paintable_set_texture (paintable,
                                       texture,
                                       WL_OUTPUT_TRANSFORM_NORMAL,
                                       FALSE);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   3);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   2);

  kasasa_dmabuf_paintable_set_texture (paintable,
                                       texture,
                                       WL_OUTPUT_TRANSFORM_90,
                                       FALSE);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   2);
  g_assert_cmpint (gdk_paintable_get_intrinsic_height (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   3);

  snapshot = gtk_snapshot_new ();
  gdk_paintable_snapshot (GDK_PAINTABLE (paintable),
                          snapshot,
                          20,
                          30);
  node = gtk_snapshot_free_to_node (snapshot);
  g_assert_nonnull (node);
  gsk_render_node_unref (node);

  kasasa_dmabuf_paintable_set_texture (paintable,
                                       NULL,
                                       WL_OUTPUT_TRANSFORM_NORMAL,
                                       FALSE);
  g_assert_cmpint (gdk_paintable_get_intrinsic_width (
                     GDK_PAINTABLE (paintable)),
                   ==,
                   0);
}

static void
test_content_fit_ignores_rounding_gaps (void)
{
  g_assert_false (kasasa_content_should_fill_allocation (0, 1200,
                                                         600, 900));
  g_assert_true (kasasa_content_should_fill_allocation (800, 1200,
                                                        600, 900));
  g_assert_true (kasasa_content_should_fill_allocation (800, 1200,
                                                        600, 901));
  g_assert_true (kasasa_content_should_fill_allocation (100, 1000,
                                                        76, 751));
  g_assert_false (kasasa_content_should_fill_allocation (100, 1000,
                                                         75, 75));
  g_assert_false (kasasa_content_should_fill_allocation (800, 1200,
                                                         600, 902));
}

static void
test_hyprland_output_rejects_unavailable_backend (void)
{
  g_autoptr (KasasaScreencast) screencast = kasasa_screencast_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *old_wayland_display = NULL;

  g_object_ref_sink (screencast);
  old_wayland_display = g_strdup (g_getenv ("WAYLAND_DISPLAY"));
  g_unsetenv ("WAYLAND_DISPLAY");

  g_assert_false (kasasa_screencast_show_hyprland_output (screencast,
                                                          "DP-1",
                                                          1920,
                                                          1080,
                                                          &error));

  if (old_wayland_display != NULL)
    g_setenv ("WAYLAND_DISPLAY", old_wayland_display, TRUE);
  g_assert_error (error,
                  KASASA_WINDOW_QUERY_ERROR,
                  KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE);
}

static void
count_fallback (KasasaScreencast *screencast,
                guint             *fallback_count)
{
  (*fallback_count)++;
}

static void
test_screencast_dmabuf_fallback_signal (void)
{
  g_autoptr (KasasaScreencast) screencast = kasasa_screencast_new ();
  guint fallback_count = 0;

  g_object_ref_sink (screencast);
  g_signal_connect (screencast,
                    "dmabuf-fallback",
                    G_CALLBACK (count_fallback),
                    &fallback_count);

  kasasa_screencast_test_push_shm_fallback_frame (screencast);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpuint (fallback_count, ==, 1);

  kasasa_screencast_test_push_shm_fallback_frame (screencast);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpuint (fallback_count, ==, 1);
}



static void
test_failed_replacement_preserves_screenshot (void)
{
  static const guint8 invalid_image[] = "not an image";
  g_autoptr (KasasaScreenshot) screenshot = kasasa_screenshot_new ();
  g_autofree gchar *valid_path = NULL;
  g_autofree gchar *valid_uri = NULL;
  g_autofree gchar *invalid_path = NULL;
  g_autofree gchar *invalid_uri = NULL;
  g_autoptr (GFile) original_file = NULL;
  g_autoptr (GError) error = NULL;
  gint height;
  gint width;

  g_object_ref_sink (screenshot);

  valid_path = create_temp_file (png_1x1, sizeof png_1x1);
  valid_uri = g_filename_to_uri (valid_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (kasasa_screenshot_load_screenshot (screenshot,
                                                    valid_uri,
                                                    &error));
  g_assert_no_error (error);

  original_file = g_object_ref (kasasa_screenshot_get_file (screenshot));
  kasasa_content_get_dimensions (KASASA_CONTENT (screenshot), &height, &width);
  g_assert_cmpint (height, ==, 1);
  g_assert_cmpint (width, ==, 1);

  invalid_path = create_temp_file (invalid_image, sizeof invalid_image - 1);
  invalid_uri = g_filename_to_uri (invalid_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_false (kasasa_screenshot_load_screenshot (screenshot,
                                                     invalid_uri,
                                                     &error));
  g_assert_nonnull (error);
  g_assert_true (g_file_equal (original_file,
                               kasasa_screenshot_get_file (screenshot)));
  g_assert_cmpint (g_remove (valid_path), ==, 0);
  g_assert_cmpint (g_remove (invalid_path), ==, 0);
}

static void
test_finish_trashes_exact_file (void)
{
  g_autoptr (KasasaScreenshot) screenshot = kasasa_screenshot_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *root = NULL;
  g_autofree gchar *selected_dir = NULL;
  g_autofree gchar *other_dir = NULL;
  g_autofree gchar *selected_path = NULL;
  g_autofree gchar *other_path = NULL;
  g_autofree gchar *selected_uri = NULL;

  g_object_ref_sink (screenshot);

  g_assert_cmpint (g_mkdir_with_parents (g_get_user_data_dir (), 0700), ==, 0);
  root = g_build_filename (g_get_user_data_dir (),
                           "kasasa-trash-test-XXXXXX",
                           NULL);
  g_assert_nonnull (g_mkdtemp (root));
  selected_dir = g_build_filename (root, "selected", NULL);
  other_dir = g_build_filename (root, "other", NULL);
  g_assert_cmpint (g_mkdir (selected_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir (other_dir, 0700), ==, 0);

  selected_path = g_build_filename (selected_dir, "same-name.png", NULL);
  other_path = g_build_filename (other_dir, "same-name.png", NULL);
  g_assert_true (g_file_set_contents (selected_path,
                                      (const gchar *) png_1x1,
                                      sizeof png_1x1,
                                      &error));
  g_assert_no_error (error);
  g_assert_true (g_file_set_contents (other_path,
                                      (const gchar *) png_1x1,
                                      sizeof png_1x1,
                                      &error));
  g_assert_no_error (error);

  selected_uri = g_filename_to_uri (selected_path, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (kasasa_screenshot_load_screenshot (screenshot,
                                                    selected_uri,
                                                    &error));
  g_assert_no_error (error);

  trash_active = TRUE;
  kasasa_content_finish (KASASA_CONTENT (screenshot));
  trash_active = FALSE;

  g_assert_false (g_file_test (selected_path, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (other_path, G_FILE_TEST_IS_REGULAR));

  g_assert_cmpint (g_remove (other_path), ==, 0);
  g_assert_cmpint (g_rmdir (selected_dir), ==, 0);
  g_assert_cmpint (g_rmdir (other_dir), ==, 0);
  g_assert_cmpint (g_rmdir (root), ==, 0);
}

static void
test_same_file_replacement_does_not_trash (void)
{
  g_autoptr (KasasaScreenshot) screenshot = kasasa_screenshot_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;

  g_object_ref_sink (screenshot);
  path = create_temp_file (png_1x1, sizeof png_1x1);
  uri = g_filename_to_uri (path, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (kasasa_screenshot_load_screenshot (screenshot, uri, &error));
  g_assert_no_error (error);

  trash_active = TRUE;
  g_assert_true (kasasa_screenshot_load_screenshot (screenshot, uri, &error));
  trash_active = FALSE;

  g_assert_no_error (error);
  g_assert_true (g_file_test (path, G_FILE_TEST_IS_REGULAR));
  g_assert_cmpint (g_remove (path), ==, 0);
}

static void
test_finish_handles_missing_file (void)
{
  g_autoptr (KasasaScreenshot) screenshot = kasasa_screenshot_new ();
  g_autoptr (GError) error = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;

  g_object_ref_sink (screenshot);
  path = create_temp_file (png_1x1, sizeof png_1x1);
  uri = g_filename_to_uri (path, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (kasasa_screenshot_load_screenshot (screenshot, uri, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_remove (path), ==, 0);

  trash_active = TRUE;
  g_test_expect_message (NULL,
                         G_LOG_LEVEL_WARNING,
                         "Error while deleting screenshot:*");
  kasasa_content_finish (KASASA_CONTENT (screenshot));
  g_test_assert_expected_messages ();
  trash_active = FALSE;
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  gst_init (&argc, &argv);
  if (!gtk_init_check ())
    {
      if (g_getenv ("KASASA_REQUIRE_DISPLAY") != NULL)
        g_error ("A display is required for GTK component tests");

      g_test_message ("No display available; skipping GTK component tests");
      return 77;
    }

  adw_init ();

  g_test_add_func ("/gtk/screenshot/layout", test_screenshot_layout);
  g_test_add_func ("/gtk/screencast/layout", test_screencast_layout);
  g_test_add_func ("/gtk/screencast/dmabuf-transform-dimensions",
                   test_dmabuf_paintable_transform_dimensions);
  g_test_add_func ("/gtk/content/fit-rounding-gaps",
                   test_content_fit_ignores_rounding_gaps);
  g_test_add_func ("/gtk/screencast/hyprland-output-unavailable",
                   test_hyprland_output_rejects_unavailable_backend);
  g_test_add_func ("/gtk/screencast/dmabuf-fallback-signal",
                   test_screencast_dmabuf_fallback_signal);
  g_test_add_func ("/gtk/screenshot/failed-replacement",
                   test_failed_replacement_preserves_screenshot);
  g_test_add_func ("/gtk/screenshot/trashes-exact-file",
                   test_finish_trashes_exact_file);
  g_test_add_func ("/gtk/screenshot/same-file-replacement",
                   test_same_file_replacement_does_not_trash);
  g_test_add_func ("/gtk/screenshot/missing-file",
                   test_finish_handles_missing_file);

  return g_test_run ();
}
