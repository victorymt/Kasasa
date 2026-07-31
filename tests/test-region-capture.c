/* test-region-capture.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "kasasa-region-capture.h"

typedef struct
{
  GMainLoop *loop;
  gchar *uri;
  GError *error;
} CaptureResult;

static gchar *fake_bin_dir;
static gchar *old_path;

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
  g_autoptr (GFile) file = NULL;

  setup_fake_path ();
  write_program ("slurp", "#!/bin/sh\nprintf '10,20 40x30\\n'\n");
  write_program ("grim", "#!/bin/sh\nprintf captured > \"$3\"\n");

  g_assert_true (kasasa_region_capture_available ());
  capture = run_capture (NULL);
  g_assert_no_error (capture.error);
  g_assert_nonnull (capture.uri);
  file = g_file_new_for_uri (capture.uri);
  g_assert_true (g_file_query_exists (file, NULL));
  g_assert_true (g_file_delete (file, NULL, NULL));

  g_free (capture.uri);
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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/region-capture/success", test_capture_success);
  g_test_add_func ("/region-capture/selection-cancelled",
                   test_selection_cancelled);
  g_test_add_func ("/region-capture/grim-failure", test_grim_failure);
  g_test_add_func ("/region-capture/running-cancelled",
                   test_running_selector_cancelled);

  return g_test_run ();
}
