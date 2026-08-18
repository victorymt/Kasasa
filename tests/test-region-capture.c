/* test-region-capture.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>

#include "kasasa-region-capture.h"
#include "kasasa-region-capture-private.h"

typedef struct
{
  GMainLoop *loop;
  gchar *uri;
  GError *error;
} CaptureResult;

static gchar *fake_bin_dir;
static gchar *old_path;

static const guint8 png_1x1[] = {
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
  0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c,
  0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
  0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00,
  0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
  0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
  0xae, 0x42, 0x60, 0x82,
};

static void
capture_done (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  CaptureResult *capture = user_data;

  capture->uri = kasasa_region_capture_screenshot_finish (result,
                                                          &capture->error);
  g_main_loop_quit (capture->loop);
}

static void
write_program (const gchar *name,
               const gchar *contents)
{
  g_autofree gchar *path = g_build_filename (fake_bin_dir, name, NULL);
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, contents, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_chmod (path, 0700), ==, 0);
}

static void
setup_fake_path (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *path = NULL;

  fake_bin_dir = g_dir_make_tmp ("kasasa-region-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (fake_bin_dir);

  old_path = g_strdup (g_getenv ("PATH"));
  path = g_strconcat (fake_bin_dir, G_SEARCHPATH_SEPARATOR_S,
                      old_path != NULL ? old_path : "", NULL);
  g_setenv ("PATH", path, TRUE);
}

static void
teardown_fake_path (void)
{
  g_autofree gchar *slurp = g_build_filename (fake_bin_dir, "slurp", NULL);
  g_autofree gchar *grim = g_build_filename (fake_bin_dir, "grim", NULL);

  if (old_path != NULL)
    g_setenv ("PATH", old_path, TRUE);
  else
    g_unsetenv ("PATH");

  g_remove (slurp);
  g_remove (grim);
  g_rmdir (fake_bin_dir);
  g_clear_pointer (&fake_bin_dir, g_free);
  g_clear_pointer (&old_path, g_free);
}

static CaptureResult
run_capture (GCancellable *cancellable)
{
  CaptureResult capture = { 0 };

  capture.loop = g_main_loop_new (NULL, FALSE);
  kasasa_region_capture_screenshot_async (cancellable,
                                          capture_done,
                                          &capture);
  g_main_loop_run (capture.loop);
  g_main_loop_unref (capture.loop);
  capture.loop = NULL;
  return capture;
}

static void
test_capture_success (void)
{
  CaptureResult capture;
  g_autofree gchar *frame_path = NULL;
  g_autofree gchar *ready_path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autoptr (GError) error = NULL;
  gint fd;

  setup_fake_path ();
  fd = g_file_open_tmp ("kasasa-region-frame-test-XXXXXX.png", &frame_path,
                        &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  close (fd);
  ready_path = g_strdup_printf ("%s.ready", frame_path);
  g_assert_true (g_file_set_contents (frame_path,
                                      (const gchar *) png_1x1,
                                      sizeof png_1x1,
                                      &error));
  g_assert_no_error (error);
  g_setenv ("KASASA_REGION_FRAME", frame_path, TRUE);
  g_setenv ("KASASA_REGION_FRAME_READY", ready_path, TRUE);
  write_program ("slurp",
                 "#!/bin/sh\n"
                 "test -f \"$KASASA_REGION_FRAME_READY\" || exit 3\n"
                 "printf '0,0 1x1\\n'\n");
  write_program ("grim",
                 "#!/bin/sh\n"
                 "cp \"$KASASA_REGION_FRAME\" \"$1\"\n"
                 "touch \"$KASASA_REGION_FRAME_READY\"\n");

  g_assert_true (kasasa_region_capture_available ());
  capture = run_capture (NULL);
  g_assert_no_error (capture.error);
  g_assert_nonnull (capture.uri);
  file = g_file_new_for_uri (capture.uri);
  g_assert_true (g_file_query_exists (file, NULL));
  pixbuf = gdk_pixbuf_new_from_file (g_file_get_path (file), &error);
  g_assert_no_error (error);
  g_assert_nonnull (pixbuf);
  g_assert_cmpint (gdk_pixbuf_get_width (pixbuf), ==, 1);
  g_assert_cmpint (gdk_pixbuf_get_height (pixbuf), ==, 1);
  g_assert_true (g_file_delete (file, NULL, NULL));

  g_free (capture.uri);
  g_unsetenv ("KASASA_REGION_FRAME");
  g_unsetenv ("KASASA_REGION_FRAME_READY");
  g_remove (frame_path);
  g_remove (ready_path);
  teardown_fake_path ();
}

static void
test_selection_cancelled (void)
{
  CaptureResult capture;

  setup_fake_path ();
  write_program ("slurp", "#!/bin/sh\nexit 1\n");
  write_program ("grim", "#!/bin/sh\nexit 0\n");

  capture = run_capture (NULL);
  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  g_clear_error (&capture.error);
  teardown_fake_path ();
}

static void
test_selection_failure (void)
{
  CaptureResult capture;

  setup_fake_path ();
  write_program ("slurp", "#!/bin/sh\nexit 2\n");
  write_program ("grim", "#!/bin/sh\nexit 0\n");

  capture = run_capture (NULL);
  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_FAILED);

  g_clear_error (&capture.error);
  teardown_fake_path ();
}

static void
test_grim_failure (void)
{
  CaptureResult capture;

  setup_fake_path ();
  write_program ("slurp", "#!/bin/sh\nprintf '0,0 20x20\\n'\n");
  write_program ("grim", "#!/bin/sh\nexit 2\n");

  capture = run_capture (NULL);
  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_FAILED);

  g_clear_error (&capture.error);
  teardown_fake_path ();
}

static gboolean
cancel_capture (gpointer user_data)
{
  g_cancellable_cancel (G_CANCELLABLE (user_data));
  return G_SOURCE_REMOVE;
}

static void
test_running_selector_cancelled (void)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  CaptureResult capture;

  setup_fake_path ();
  write_program ("slurp", "#!/bin/sh\nwhile :; do :; done\n");
  write_program ("grim", "#!/bin/sh\nexit 0\n");

  g_timeout_add (25, cancel_capture, cancellable);
  capture = run_capture (cancellable);
  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

  g_clear_error (&capture.error);
  teardown_fake_path ();
}

static void
assert_geometry_mapping (gint   origin_x,
                         gint   origin_y,
                         guint  logical_width,
                         guint  logical_height,
                         gint   frame_width,
                         gint   frame_height,
                         gint   selection_x,
                         gint   selection_y,
                         guint  selection_width,
                         guint  selection_height,
                         gint   expected_x,
                         gint   expected_y,
                         guint  expected_width,
                         guint  expected_height)
{
  gint pixel_x = -1;
  gint pixel_y = -1;
  guint pixel_width = 0;
  guint pixel_height = 0;

  g_assert_true (kasasa_region_capture_test_map_geometry (
    origin_x,
    origin_y,
    logical_width,
    logical_height,
    frame_width,
    frame_height,
    selection_x,
    selection_y,
    selection_width,
    selection_height,
    &pixel_x,
    &pixel_y,
    &pixel_width,
    &pixel_height));
  g_assert_cmpint (pixel_x, ==, expected_x);
  g_assert_cmpint (pixel_y, ==, expected_y);
  g_assert_cmpuint (pixel_width, ==, expected_width);
  g_assert_cmpuint (pixel_height, ==, expected_height);
}

static void
test_geometry_fractional_scale (void)
{
  assert_geometry_mapping (0, 0,
                           1920, 1200,
                           2560, 1600,
                           600, 300, 600, 300,
                           800, 400, 800, 400);
}

static void
test_geometry_fractional_rounding (void)
{
  assert_geometry_mapping (0, 0,
                           3, 3,
                           4, 4,
                           1, 1, 1, 1,
                           1, 1, 2, 2);
}

static void
test_geometry_negative_origin (void)
{
  assert_geometry_mapping (-1920, 0,
                           1920, 1200,
                           2560, 1600,
                           -1320, 300, 600, 300,
                           800, 400, 800, 400);
}

static void
test_geometry_out_of_bounds (void)
{
  gint pixel_x = -1;
  gint pixel_y = -1;
  guint pixel_width = 0;
  guint pixel_height = 0;

  g_assert_false (kasasa_region_capture_test_map_geometry (
    -1920, 0,
    1920, 1200,
    2560, 1600,
    -1921, 300, 600, 300,
    &pixel_x, &pixel_y, &pixel_width, &pixel_height));
  g_assert_false (kasasa_region_capture_test_map_geometry (
    -1920, 0,
    1920, 1200,
    2560, 1600,
    -600, 1000, 601, 201,
    &pixel_x, &pixel_y, &pixel_width, &pixel_height));
}

static void
test_geometry_identity (void)
{
  assert_geometry_mapping (0, 0,
                           2560, 1600,
                           2560, 1600,
                           600, 300, 600, 300,
                           600, 300, 600, 300);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/region-capture/success", test_capture_success);
  g_test_add_func ("/region-capture/selection-cancelled",
                   test_selection_cancelled);
  g_test_add_func ("/region-capture/selection-failure",
                   test_selection_failure);
  g_test_add_func ("/region-capture/grim-failure", test_grim_failure);
  g_test_add_func ("/region-capture/running-cancelled",
                   test_running_selector_cancelled);
  g_test_add_func ("/region-capture/geometry/fractional-scale",
                   test_geometry_fractional_scale);
  g_test_add_func ("/region-capture/geometry/fractional-rounding",
                   test_geometry_fractional_rounding);
  g_test_add_func ("/region-capture/geometry/negative-origin",
                   test_geometry_negative_origin);
  g_test_add_func ("/region-capture/geometry/out-of-bounds",
                   test_geometry_out_of_bounds);
  g_test_add_func ("/region-capture/geometry/identity",
                   test_geometry_identity);

  return g_test_run ();
}
