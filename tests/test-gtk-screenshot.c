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

#include "kasasa-content.h"
#include "kasasa-screencast.h"
#include "kasasa-screencast-pipeline.h"
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
test_screencast_rejects_invalid_connection (void)
{
  g_autoptr (KasasaScreencast) screencast = kasasa_screencast_new ();
  g_autoptr (GError) error = NULL;
  gint fd;

  g_object_ref_sink (screencast);
  fd = open ("/dev/null", O_RDONLY);
  g_assert_cmpint (fd, >=, 0);

  g_assert_false (kasasa_screencast_show (screencast,
                                          NULL,
                                          fd,
                                          1,
                                          0,
                                          0,
                                          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (fcntl (fd, F_GETFD), ==, -1);
  g_assert_cmpint (errno, ==, EBADF);
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
test_screencast_pipeline_preference (void)
{
  KasasaScreencastPipelineMode expected_gpu_mode;

  expected_gpu_mode = kasasa_screencast_pipeline_gpu_available ()
                      ? KASASA_SCREENCAST_PIPELINE_GPU
                      : KASASA_SCREENCAST_PIPELINE_CPU;

  g_assert_cmpint (kasasa_screencast_pipeline_select_mode ("cpu"),
                   ==,
                   KASASA_SCREENCAST_PIPELINE_CPU);
  g_assert_cmpint (kasasa_screencast_pipeline_select_mode ("gpu"),
                   ==,
                   expected_gpu_mode);
  g_assert_cmpint (kasasa_screencast_pipeline_select_mode ("invalid"),
                   ==,
                   KASASA_SCREENCAST_PIPELINE_CPU);
}

static void
count_cpu_fallback (KasasaScreencast *screencast,
                    guint             *fallback_count)
{
  (*fallback_count)++;
}

static void
test_screencast_cpu_fallback_signal (void)
{
  g_autoptr (KasasaScreencast) screencast = kasasa_screencast_new ();
  guint fallback_count = 0;

  g_object_ref_sink (screencast);
  g_signal_connect (screencast,
                    "cpu-fallback",
                    G_CALLBACK (count_cpu_fallback),
                    &fallback_count);
  g_signal_emit_by_name (screencast, "cpu-fallback");

  g_assert_cmpuint (fallback_count, ==, 1);
}

static void
test_cpu_screencast_pipeline (void)
{
  KasasaScreencastPipeline pipeline = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GstElement) pipewire = NULL;
  g_autoptr (GstElement) display_queue = NULL;
  g_autoptr (GstElement) display_convert = NULL;
  g_autoptr (GstElement) sink = NULL;
  g_autoptr (GstElement) videocrop = NULL;
  g_autoptr (GstElement) analysis_queue = NULL;
  g_autoptr (GstElement) analysis_sink = NULL;
  g_autoptr (GstElement) gldownload = NULL;
  g_autoptr (GstElement) glupload = NULL;
  gboolean use_bufferpool = TRUE;
  gboolean enable_last_sample = TRUE;
  gboolean sync = FALSE;
  guint64 throttle_time = 0;
  gint min_buffers = 0;
  gint leaky = 0;
  guint max_size_buffers = 0;

  g_assert_true (kasasa_screencast_pipeline_build_portal (
                   -1,
                   1,
                   KASASA_SCREENCAST_PIPELINE_CPU,
                   24,
                   &pipeline,
                   &error));
  g_assert_no_error (error);
  g_assert_nonnull (pipeline.pipeline);
  g_assert_nonnull (pipeline.paintable);

  pipewire = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                  "pipewire_element");
  display_queue = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                       "display_queue");
  display_convert = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                         "display_convert");
  sink = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "sink");
  videocrop = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "videocrop");
  analysis_queue = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                        "analysis_queue");
  analysis_sink = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                       "analysis_sink");
  gldownload = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "gldownload");
  glupload = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "glupload");
  g_assert_nonnull (pipewire);
  g_assert_nonnull (display_queue);
  g_assert_nonnull (display_convert);
  g_assert_nonnull (sink);
  g_assert_null (videocrop);
  g_assert_null (analysis_queue);
  g_assert_null (analysis_sink);
  g_assert_null (gldownload);
  g_assert_null (glupload);

  g_object_get (pipewire,
                "use-bufferpool", &use_bufferpool,
                "min-buffers", &min_buffers,
                NULL);
  g_object_get (sink,
                "sync", &sync,
                "throttle-time", &throttle_time,
                "enable-last-sample", &enable_last_sample,
                NULL);
  g_object_get (display_queue,
                "leaky", &leaky,
                "max-size-buffers", &max_size_buffers,
                NULL);
  g_assert_false (use_bufferpool);
  g_assert_cmpint (min_buffers, ==, 1);
  g_assert_true (sync);
  g_assert_cmpuint (throttle_time, ==, GST_SECOND / 24);
  g_assert_false (enable_last_sample);
  g_assert_cmpint (leaky, ==, 2);
  g_assert_cmpuint (max_size_buffers, ==, 1);

  kasasa_screencast_pipeline_clear (&pipeline);
}

static void
test_portal_pipeline_closes_untransferred_fd (void)
{
  KasasaScreencastPipeline pipeline = { 0 };
  GstRegistry *registry = gst_registry_get ();
  GstPluginFeature *pipewire_feature;
  g_autoptr (GError) error = NULL;
  gboolean built;
  gboolean fd_was_closed;
  gint fd;

  pipewire_feature = gst_registry_lookup_feature (registry, "pipewiresrc");
  if (pipewire_feature == NULL)
    {
      g_test_skip ("pipewiresrc is unavailable");
      return;
    }

  gst_registry_remove_feature (registry, pipewire_feature);
  fd = open ("/dev/null", O_RDONLY);
  g_assert_cmpint (fd, >=, 0);
  built = kasasa_screencast_pipeline_build_portal (
    fd,
    1,
    KASASA_SCREENCAST_PIPELINE_CPU,
    30,
    &pipeline,
    &error);
  fd_was_closed = fcntl (fd, F_GETFD) == -1 && errno == EBADF;
  g_assert_true (gst_registry_add_feature (registry, pipewire_feature));
  gst_object_unref (pipewire_feature);
  kasasa_screencast_pipeline_clear (&pipeline);

  g_assert_false (built);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_true (fd_was_closed);
}

static void
test_gpu_screencast_pipeline (void)
{
  KasasaScreencastPipeline pipeline = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GstElement) pipewire = NULL;
  g_autoptr (GstElement) glupload = NULL;
  g_autoptr (GstElement) glcolorconvert = NULL;
  g_autoptr (GstElement) gl_filter = NULL;
  g_autoptr (GstElement) display_queue = NULL;
  g_autoptr (GstElement) sink = NULL;
  g_autoptr (GstElement) gpu_crop = NULL;
  g_autoptr (GstElement) analysis_queue = NULL;
  g_autoptr (GstElement) analysis_sink = NULL;
  g_autoptr (GstElement) gldownload = NULL;
  g_autoptr (GstElement) glsinkbin = NULL;
  g_autoptr (GstElement) videocrop = NULL;
  g_autoptr (GstPad) queue_sink_pad = NULL;
  g_autoptr (GstPad) queue_upstream_pad = NULL;
  g_autoptr (GstElement) queue_upstream = NULL;
  g_autoptr (GstPad) upload_sink_pad = NULL;
  g_autoptr (GstPad) upload_upstream_pad = NULL;
  g_autoptr (GstElement) upload_upstream = NULL;
  g_autoptr (GstCaps) caps = NULL;
  const GstCapsFeatures *features;
  gboolean use_bufferpool = FALSE;
  gboolean enable_last_sample = TRUE;
  gboolean sync = FALSE;
  guint64 throttle_time = 0;
  gint min_buffers = 0;
  gint leaky = 0;
  guint max_size_buffers = 0;

  if (!kasasa_screencast_pipeline_gpu_available ())
    {
      g_test_skip ("Required GStreamer GL elements are unavailable");
      return;
    }

  g_assert_true (kasasa_screencast_pipeline_build_portal (
                   -1,
                   1,
                   KASASA_SCREENCAST_PIPELINE_GPU,
                   60,
                   &pipeline,
                   &error));
  g_assert_no_error (error);
  g_assert_nonnull (pipeline.pipeline);
  g_assert_nonnull (pipeline.paintable);

  pipewire = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                  "pipewire_element");
  glupload = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "glupload");
  glcolorconvert = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                        "glcolorconvert");
  gl_filter = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "gl_filter");
  display_queue = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                       "display_queue");
  sink = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "sink");
  gpu_crop = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "gpu_crop");
  analysis_queue = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                        "analysis_queue");
  analysis_sink = gst_bin_get_by_name (GST_BIN (pipeline.pipeline),
                                       "analysis_sink");
  gldownload = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "gldownload");
  glsinkbin = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "glsinkbin");
  videocrop = gst_bin_get_by_name (GST_BIN (pipeline.pipeline), "videocrop");
  g_assert_nonnull (pipewire);
  g_assert_nonnull (glupload);
  g_assert_nonnull (glcolorconvert);
  g_assert_nonnull (gl_filter);
  g_assert_nonnull (display_queue);
  g_assert_nonnull (sink);
  g_assert_null (glsinkbin);
  g_assert_null (gpu_crop);
  g_assert_null (analysis_queue);
  g_assert_null (analysis_sink);
  g_assert_null (gldownload);
  g_assert_null (videocrop);

  g_object_get (pipewire,
                "use-bufferpool", &use_bufferpool,
                "min-buffers", &min_buffers,
                NULL);
  g_object_get (gl_filter, "caps", &caps, NULL);
  g_object_get (sink,
                "sync", &sync,
                "throttle-time", &throttle_time,
                "enable-last-sample", &enable_last_sample,
                NULL);
  g_object_get (display_queue,
                "leaky", &leaky,
                "max-size-buffers", &max_size_buffers,
                NULL);
  g_assert_true (use_bufferpool);
  g_assert_cmpint (min_buffers, ==, 8);
  g_assert_true (sync);
  g_assert_cmpuint (throttle_time, ==, GST_SECOND / 60);
  g_assert_false (enable_last_sample);
  g_assert_cmpint (leaky, ==, 2);
  g_assert_cmpuint (max_size_buffers, ==, 1);
  g_assert_nonnull (caps);
  features = gst_caps_get_features (caps, 0);
  g_assert_true (gst_caps_features_contains (features, "memory:GLMemory"));

  queue_sink_pad = gst_element_get_static_pad (display_queue, "sink");
  queue_upstream_pad = gst_pad_get_peer (queue_sink_pad);
  queue_upstream = gst_pad_get_parent_element (queue_upstream_pad);
  upload_sink_pad = gst_element_get_static_pad (glupload, "sink");
  upload_upstream_pad = gst_pad_get_peer (upload_sink_pad);
  upload_upstream = gst_pad_get_parent_element (upload_upstream_pad);
  g_assert_true (queue_upstream == pipewire);
  g_assert_true (upload_upstream == display_queue);

  kasasa_screencast_pipeline_clear (&pipeline);
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
  g_test_add_func ("/gtk/content/fit-rounding-gaps",
                   test_content_fit_ignores_rounding_gaps);
  g_test_add_func ("/gtk/screencast/invalid-connection",
                   test_screencast_rejects_invalid_connection);
  g_test_add_func ("/gtk/screencast/hyprland-output-unavailable",
                   test_hyprland_output_rejects_unavailable_backend);
  g_test_add_func ("/gtk/screencast/pipeline-preference",
                   test_screencast_pipeline_preference);
  g_test_add_func ("/gtk/screencast/cpu-fallback-signal",
                   test_screencast_cpu_fallback_signal);
  g_test_add_func ("/gtk/screencast/cpu-pipeline",
                   test_cpu_screencast_pipeline);
  g_test_add_func ("/gtk/screencast/early-failure-closes-fd",
                   test_portal_pipeline_closes_untransferred_fd);
  g_test_add_func ("/gtk/screencast/gpu-pipeline",
                   test_gpu_screencast_pipeline);
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
