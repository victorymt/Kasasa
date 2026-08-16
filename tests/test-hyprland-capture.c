/* test-hyprland-capture.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-hyprland-capture.h"
#include "kasasa-hyprland-capture-private.h"

typedef struct
{
  GMainLoop *loop;
  GError *error;
  gchar *uri;
  guint callback_count;
} CaptureResult;

typedef struct
{
  GMutex mutex;
  GCond cond;
  gboolean started;
  guint stop_count;
  KasasaHyprlandStreamFrameFunc frame_cb;
  gpointer user_data;
} FakeBackend;

static FakeBackend fake_backend;

static gboolean
fake_available (void)
{
  return TRUE;
}

static gboolean
fake_handle_from_address (const gchar *address,
                          guint32     *handle,
                          GError     **error)
{
  (void) address;
  (void) error;
  *handle = 1;
  return TRUE;
}

static KasasaHyprlandStream *
fake_start (guint32                       handle,
            guint                         frame_rate,
            KasasaHyprlandStreamFrameFunc frame_cb,
            KasasaHyprlandStreamErrorFunc error_cb,
            gpointer                      user_data,
            GDestroyNotify                user_data_destroy,
            GError                      **error)
{
  (void) handle;
  (void) frame_rate;
  (void) error_cb;
  (void) user_data_destroy;
  (void) error;

  g_mutex_lock (&fake_backend.mutex);
  fake_backend.frame_cb = frame_cb;
  fake_backend.user_data = user_data;
  fake_backend.started = TRUE;
  g_cond_signal (&fake_backend.cond);
  g_mutex_unlock (&fake_backend.mutex);
  return (KasasaHyprlandStream *) &fake_backend;
}

static void
fake_stop (KasasaHyprlandStream *stream)
{
  g_assert_true (stream == (KasasaHyprlandStream *) &fake_backend);
  g_mutex_lock (&fake_backend.mutex);
  fake_backend.stop_count++;
  g_mutex_unlock (&fake_backend.mutex);
}

static const KasasaHyprlandCaptureBackendOps fake_backend_ops = {
  .available = fake_available,
  .handle_from_address = fake_handle_from_address,
  .start = fake_start,
  .stop = fake_stop,
};

static void
setup_fake_backend (void)
{
  g_mutex_init (&fake_backend.mutex);
  g_cond_init (&fake_backend.cond);
  fake_backend.started = FALSE;
  fake_backend.stop_count = 0;
  fake_backend.frame_cb = NULL;
  fake_backend.user_data = NULL;
  kasasa_hyprland_capture_test_set_backend (&fake_backend_ops);
}

static void
teardown_fake_backend (void)
{
  kasasa_hyprland_capture_test_reset ();
  g_cond_clear (&fake_backend.cond);
  g_mutex_clear (&fake_backend.mutex);
}

static void
wait_for_backend_start (void)
{
  gint64 deadline = g_get_monotonic_time () + G_TIME_SPAN_SECOND;

  g_mutex_lock (&fake_backend.mutex);
  while (!fake_backend.started)
    g_assert_true (g_cond_wait_until (&fake_backend.cond,
                                     &fake_backend.mutex,
                                     deadline));
  g_mutex_unlock (&fake_backend.mutex);
}

static guint
get_stop_count (void)
{
  guint stop_count;

  g_mutex_lock (&fake_backend.mutex);
  stop_count = fake_backend.stop_count;
  g_mutex_unlock (&fake_backend.mutex);
  return stop_count;
}

static void
capture_ready (GObject      *source_object,
               GAsyncResult *result,
               gpointer       user_data)
{
  CaptureResult *capture = user_data;

  (void) source_object;
  capture->callback_count++;
  capture->uri = kasasa_hyprland_capture_screenshot_finish (result,
                                                            &capture->error);
  g_main_loop_quit (capture->loop);
}

static void
test_cancel_before_capture_starts (void)
{
  KasasaWindowClient client = { 0 };
  CaptureResult capture = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  setup_fake_backend ();
  client.address = (gchar *) "0x1";
  g_cancellable_cancel (cancellable);
  capture.loop = g_main_loop_new (NULL, FALSE);

  kasasa_hyprland_capture_screenshot_async (&client,
                                            cancellable,
                                            capture_ready,
                                            &capture);
  g_main_loop_run (capture.loop);

  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (capture.callback_count, ==, 1);
  g_assert_false (fake_backend.started);
  g_assert_cmpuint (get_stop_count (), ==, 0);

  g_clear_error (&capture.error);
  g_main_loop_unref (capture.loop);
  teardown_fake_backend ();
}

static void
test_cancel_after_capture_starts (void)
{
  KasasaWindowClient client = { 0 };
  CaptureResult capture = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  setup_fake_backend ();
  client.address = (gchar *) "0x1";
  capture.loop = g_main_loop_new (NULL, FALSE);
  kasasa_hyprland_capture_screenshot_async (&client,
                                            cancellable,
                                            capture_ready,
                                            &capture);
  wait_for_backend_start ();
  g_cancellable_cancel (cancellable);
  g_main_loop_run (capture.loop);

  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (capture.callback_count, ==, 1);
  g_assert_cmpuint (get_stop_count (), ==, 1);

  g_clear_error (&capture.error);
  g_main_loop_unref (capture.loop);
  teardown_fake_backend ();
}

static void
test_capture_times_out (void)
{
  KasasaWindowClient client = { 0 };
  CaptureResult capture = { 0 };

  setup_fake_backend ();
  kasasa_hyprland_capture_test_set_timeout (20 * G_TIME_SPAN_MILLISECOND);
  client.address = (gchar *) "0x1";
  capture.loop = g_main_loop_new (NULL, FALSE);
  kasasa_hyprland_capture_screenshot_async (&client,
                                            NULL,
                                            capture_ready,
                                            &capture);
  g_main_loop_run (capture.loop);

  g_assert_null (capture.uri);
  g_assert_error (capture.error,
                  KASASA_WINDOW_QUERY_ERROR,
                  KASASA_WINDOW_QUERY_ERROR_FAILED);
  g_assert_nonnull (strstr (capture.error->message, "Timed out"));
  g_assert_cmpuint (capture.callback_count, ==, 1);
  g_assert_cmpuint (get_stop_count (), ==, 1);

  g_clear_error (&capture.error);
  g_main_loop_unref (capture.loop);
  teardown_fake_backend ();
}

static void
test_cancellation_wins_completed_frame_race (void)
{
  static const guint8 pixel[] = { 0xff, 0, 0, 0xff };
  KasasaWindowClient client = { 0 };
  CaptureResult capture = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  KasasaHyprlandStreamFrameFunc frame_cb;
  gpointer user_data;

  setup_fake_backend ();
  client.address = (gchar *) "0x1";
  capture.loop = g_main_loop_new (NULL, FALSE);
  kasasa_hyprland_capture_screenshot_async (&client,
                                            cancellable,
                                            capture_ready,
                                            &capture);
  wait_for_backend_start ();

  g_mutex_lock (&fake_backend.mutex);
  frame_cb = fake_backend.frame_cb;
  user_data = fake_backend.user_data;
  g_mutex_unlock (&fake_backend.mutex);
  frame_cb (user_data,
            pixel,
            1,
            1,
            4,
            KASASA_HYPRLAND_STREAM_FORMAT_RGBA,
            FALSE,
            0);
  g_cancellable_cancel (cancellable);
  g_main_loop_run (capture.loop);

  g_assert_null (capture.uri);
  g_assert_error (capture.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint (capture.callback_count, ==, 1);
  g_assert_cmpuint (get_stop_count (), ==, 1);

  g_clear_error (&capture.error);
  g_main_loop_unref (capture.loop);
  teardown_fake_backend ();
}

static void
test_frame_formats (void)
{
  static const guint8 bgra[] = { 3, 2, 1, 4 };
  static const guint8 rgba[] = { 1, 2, 3, 4 };
  static const struct
  {
    KasasaHyprlandStreamFormat format;
    const guint8 *input;
    guint8 alpha;
  } cases[] = {
    { KASASA_HYPRLAND_STREAM_FORMAT_BGRX, bgra, 0xff },
    { KASASA_HYPRLAND_STREAM_FORMAT_BGRA, bgra, 4 },
    { KASASA_HYPRLAND_STREAM_FORMAT_RGBX, rgba, 0xff },
    { KASASA_HYPRLAND_STREAM_FORMAT_RGBA, rgba, 4 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (GError) error = NULL;
      g_autofree guint8 *output = NULL;
      gint width = 0;
      gint height = 0;

      output = kasasa_hyprland_capture_test_copy_frame (cases[i].input,
                                                        1,
                                                        1,
                                                        4,
                                                        cases[i].format,
                                                        FALSE,
                                                        0,
                                                        &width,
                                                        &height,
                                                        &error);
      g_assert_no_error (error);
      g_assert_nonnull (output);
      g_assert_cmpint (width, ==, 1);
      g_assert_cmpint (height, ==, 1);
      g_assert_cmpint (output[0], ==, 1);
      g_assert_cmpint (output[1], ==, 2);
      g_assert_cmpint (output[2], ==, 3);
      g_assert_cmpint (output[3], ==, cases[i].alpha);
    }
}

static void
test_frame_transforms_and_y_invert (void)
{
  static const guint8 pixels[] = {
    1, 0, 0, 0xff, 2, 0, 0, 0xff, 3, 0, 0, 0xff,
    4, 0, 0, 0xff, 5, 0, 0, 0xff, 6, 0, 0, 0xff,
  };
  static const guint8 expected[2][8][6] = {
    {
      { 1, 2, 3, 4, 5, 6 }, { 3, 6, 2, 5, 1, 4 },
      { 6, 5, 4, 3, 2, 1 }, { 4, 1, 5, 2, 6, 3 },
      { 3, 2, 1, 6, 5, 4 }, { 1, 4, 2, 5, 3, 6 },
      { 4, 5, 6, 1, 2, 3 }, { 6, 3, 5, 2, 4, 1 },
    },
    {
      { 4, 5, 6, 1, 2, 3 }, { 6, 3, 5, 2, 4, 1 },
      { 3, 2, 1, 6, 5, 4 }, { 1, 4, 2, 5, 3, 6 },
      { 6, 5, 4, 3, 2, 1 }, { 4, 1, 5, 2, 6, 3 },
      { 1, 2, 3, 4, 5, 6 }, { 3, 6, 2, 5, 1, 4 },
    },
  };

  for (guint invert = 0; invert < 2; invert++)
    {
      for (guint transform = 0; transform < 8; transform++)
        {
          g_autoptr (GError) error = NULL;
          g_autofree guint8 *output = NULL;
          gint width = 0;
          gint height = 0;

          output = kasasa_hyprland_capture_test_copy_frame (
            pixels,
            3,
            2,
            12,
            KASASA_HYPRLAND_STREAM_FORMAT_RGBA,
            invert,
            transform,
            &width,
            &height,
            &error);
          g_assert_no_error (error);
          g_assert_nonnull (output);
          g_assert_cmpint (width * height, ==, 6);
          for (guint i = 0; i < 6; i++)
            g_assert_cmpint (output[i * 4], ==, expected[invert][transform][i]);
        }
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unit/hyprland-capture/cancel-before-start",
                   test_cancel_before_capture_starts);
  g_test_add_func ("/unit/hyprland-capture/cancel-after-start",
                   test_cancel_after_capture_starts);
  g_test_add_func ("/unit/hyprland-capture/timeout",
                   test_capture_times_out);
  g_test_add_func ("/unit/hyprland-capture/completion-cancel-race",
                   test_cancellation_wins_completed_frame_race);
  g_test_add_func ("/unit/hyprland-capture/frame-formats",
                   test_frame_formats);
  g_test_add_func ("/unit/hyprland-capture/frame-transforms",
                   test_frame_transforms_and_y_invert);
  return g_test_run ();
}
