/* kasasa-region-capture.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-region-capture.h"
#include "kasasa-region-capture-private.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <unistd.h>

#ifdef KASASA_HAVE_LAYER_SHELL
#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif

static void
cancel_subprocess (GCancellable *cancellable,
                   gpointer      user_data)
{
  g_subprocess_force_exit (G_SUBPROCESS (user_data));
}

static gboolean
run_subprocess (const gchar * const *argv,
                gboolean             capture_stdout,
                GCancellable        *cancellable,
                gchar              **stdout_text,
                GError             **error)
{
  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDERR_PIPE;
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autofree gchar *stderr_text = NULL;
  gulong cancel_id = 0;

  if (capture_stdout)
    flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;

  subprocess = g_subprocess_newv (argv, flags, error);
  if (subprocess == NULL)
    return FALSE;

  if (cancellable != NULL)
    cancel_id = g_cancellable_connect (cancellable,
                                       G_CALLBACK (cancel_subprocess),
                                       g_object_ref (subprocess),
                                       g_object_unref);

  if (!g_subprocess_communicate_utf8 (subprocess,
                                      NULL,
                                      cancellable,
                                      stdout_text,
                                      &stderr_text,
                                      error))
    {
      if (cancel_id != 0)
        g_cancellable_disconnect (cancellable, cancel_id);
      return FALSE;
    }

  if (cancel_id != 0)
    g_cancellable_disconnect (cancellable, cancel_id);

  if (!g_subprocess_get_successful (subprocess))
    {
      g_autofree gchar *detail = g_strdup (stderr_text);

      if (detail != NULL)
        g_strstrip (detail);
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "%s failed%s%s",
                   argv[0],
                   detail != NULL && *detail != '\0' ? ": " : "",
                   detail != NULL ? detail : "");
      return FALSE;
    }

  return TRUE;
}

static gboolean
geometry_is_valid (const gchar *geometry)
{
  g_autoptr (GRegex) regex = NULL;

  regex = g_regex_new ("^-?[0-9]+,-?[0-9]+ [1-9][0-9]*x[1-9][0-9]*$",
                       G_REGEX_DEFAULT,
                       G_REGEX_MATCH_DEFAULT,
                       NULL);
  return g_regex_match (regex, geometry, G_REGEX_MATCH_DEFAULT, NULL);
}

static gboolean
capture_was_cancelled (GCancellable *cancellable)
{
  return cancellable != NULL && g_cancellable_is_cancelled (cancellable);
}

typedef struct
{
  gint origin_x;
  gint origin_y;
  guint logical_width;
  guint logical_height;
} FrameGeometry;

static gboolean
map_frame_geometry (const FrameGeometry *geometry,
                    gint                 frame_width,
                    gint                 frame_height,
                    gint                 selection_x,
                    gint                 selection_y,
                    guint                selection_width,
                    guint                selection_height,
                    gint                *pixel_x,
                    gint                *pixel_y,
                    guint               *pixel_width,
                    guint               *pixel_height)
{
  gint64 logical_x0;
  gint64 logical_y0;
  gint64 logical_x1;
  gint64 logical_y1;
  gint64 pixel_x0;
  gint64 pixel_y0;
  gint64 pixel_x1;
  gint64 pixel_y1;

  if (geometry == NULL
      || geometry->logical_width == 0
      || geometry->logical_height == 0
      || frame_width <= 0
      || frame_height <= 0
      || selection_width == 0
      || selection_height == 0)
    return FALSE;

  logical_x0 = (gint64) selection_x - geometry->origin_x;
  logical_y0 = (gint64) selection_y - geometry->origin_y;
  logical_x1 = logical_x0 + selection_width;
  logical_y1 = logical_y0 + selection_height;
  if (logical_x0 < 0 || logical_y0 < 0
      || logical_x1 > geometry->logical_width
      || logical_y1 > geometry->logical_height)
    return FALSE;

  pixel_x0 = logical_x0 * frame_width / geometry->logical_width;
  pixel_y0 = logical_y0 * frame_height / geometry->logical_height;
  pixel_x1 = (logical_x1 * frame_width + geometry->logical_width - 1)
             / geometry->logical_width;
  pixel_y1 = (logical_y1 * frame_height + geometry->logical_height - 1)
             / geometry->logical_height;

  if (pixel_x0 < 0 || pixel_y0 < 0
      || pixel_x1 > frame_width || pixel_y1 > frame_height
      || pixel_x1 <= pixel_x0 || pixel_y1 <= pixel_y0)
    return FALSE;

  *pixel_x = (gint) pixel_x0;
  *pixel_y = (gint) pixel_y0;
  *pixel_width = (guint) (pixel_x1 - pixel_x0);
  *pixel_height = (guint) (pixel_y1 - pixel_y0);
  return TRUE;
}

#ifdef KASASA_ENABLE_TESTS
gboolean
kasasa_region_capture_test_map_geometry (gint   origin_x,
                                         gint   origin_y,
                                         guint  logical_width,
                                         guint  logical_height,
                                         gint   frame_width,
                                         gint   frame_height,
                                         gint   selection_x,
                                         gint   selection_y,
                                         guint  selection_width,
                                         guint  selection_height,
                                         gint  *pixel_x,
                                         gint  *pixel_y,
                                         guint *pixel_width,
                                         guint *pixel_height)
{
  const FrameGeometry geometry = {
    .origin_x = origin_x,
    .origin_y = origin_y,
    .logical_width = logical_width,
    .logical_height = logical_height,
  };

  return map_frame_geometry (&geometry,
                             frame_width,
                             frame_height,
                             selection_x,
                             selection_y,
                             selection_width,
                             selection_height,
                             pixel_x,
                             pixel_y,
                             pixel_width,
                             pixel_height);
}
#endif

#ifdef KASASA_HAVE_LAYER_SHELL
#define FRAME_OVERLAY_TIMEOUT_USEC (2 * G_TIME_SPAN_SECOND)

typedef struct
{
  gint ref_count;
  GMutex mutex;
  GCond cond;
  gboolean completed;
  guint pending_windows;
  gchar *frame_path;
  GPtrArray *windows;
  FrameGeometry geometry;
  GError *error;
} FrameOverlay;

typedef struct
{
  FrameOverlay *overlay;
  GdkFrameClock *frame_clock;
  gulong after_paint_id;
  gboolean painted;
} FrameOverlayPaintWait;

static FrameOverlay *
frame_overlay_ref (FrameOverlay *overlay)
{
  g_atomic_int_inc (&overlay->ref_count);
  return overlay;
}

static void
frame_overlay_unref (FrameOverlay *overlay)
{
  if (!g_atomic_int_dec_and_test (&overlay->ref_count))
    return;

  g_clear_error (&overlay->error);
  g_clear_pointer (&overlay->windows, g_ptr_array_unref);
  g_clear_pointer (&overlay->frame_path, g_free);
  g_cond_clear (&overlay->cond);
  g_mutex_clear (&overlay->mutex);
  g_free (overlay);
}

static FrameOverlay *
frame_overlay_new (const gchar *frame_path)
{
  FrameOverlay *overlay = g_new0 (FrameOverlay, 1);

  overlay->ref_count = 1;
  g_mutex_init (&overlay->mutex);
  g_cond_init (&overlay->cond);
  overlay->frame_path = g_strdup (frame_path);
  overlay->windows = g_ptr_array_new_with_free_func (g_object_unref);
  return overlay;
}

static void
frame_overlay_complete (FrameOverlay *overlay,
                        GError       *error)
{
  g_mutex_lock (&overlay->mutex);
  if (!overlay->completed)
    {
      overlay->completed = TRUE;
      overlay->error = error;
      g_cond_signal (&overlay->cond);
    }
  else
    g_clear_error (&error);
  g_mutex_unlock (&overlay->mutex);
}

static void
frame_overlay_paint_wait_free (gpointer user_data)
{
  FrameOverlayPaintWait *wait = user_data;

  if (wait->frame_clock != NULL && wait->after_paint_id != 0)
    g_signal_handler_disconnect (wait->frame_clock, wait->after_paint_id);
  g_clear_object (&wait->frame_clock);
  frame_overlay_unref (wait->overlay);
  g_free (wait);
}

static void
frame_overlay_after_paint_cb (GdkFrameClock *frame_clock,
                              gpointer       user_data)
{
  FrameOverlayPaintWait *wait = user_data;
  gulong handler_id;

  if (wait->painted)
    return;
  wait->painted = TRUE;

  g_assert_cmpuint (wait->overlay->pending_windows, >, 0);
  wait->overlay->pending_windows--;
  if (wait->overlay->pending_windows == 0)
    frame_overlay_complete (wait->overlay, NULL);

  handler_id = wait->after_paint_id;
  wait->after_paint_id = 0;
  g_signal_handler_disconnect (frame_clock, handler_id);
}

static void
frame_overlay_window_mapped_cb (GtkWidget *widget,
                                gpointer   user_data)
{
  FrameOverlayPaintWait *wait = user_data;
  GdkFrameClock *frame_clock;

  if (wait->frame_clock != NULL)
    return;

  frame_clock = gtk_widget_get_frame_clock (widget);
  if (frame_clock == NULL)
    return;

  wait->frame_clock = g_object_ref (frame_clock);
  wait->after_paint_id = g_signal_connect (frame_clock,
                                           "after-paint",
                                           G_CALLBACK (frame_overlay_after_paint_cb),
                                           wait);
  gdk_frame_clock_request_phase (frame_clock,
                                 GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);
  gtk_widget_queue_draw (widget);
}

static gboolean
frame_overlay_add_monitor_window (FrameOverlay *overlay,
                                  GdkMonitor   *monitor,
                                  GdkPixbuf    *frame)
{
  GdkRectangle geometry;
  FrameOverlayPaintWait *wait;
  GtkWidget *picture;
  GtkWindow *window;
  g_autoptr (GdkPixbuf) crop = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  gint frame_width = gdk_pixbuf_get_width (frame);
  gint frame_height = gdk_pixbuf_get_height (frame);
  gint x;
  gint y;
  guint width;
  guint height;

  gdk_monitor_get_geometry (monitor, &geometry);
  if (!map_frame_geometry (&overlay->geometry,
                           frame_width,
                           frame_height,
                           geometry.x,
                           geometry.y,
                           (guint) geometry.width,
                           (guint) geometry.height,
                           &x,
                           &y,
                           &width,
                           &height))
    return FALSE;

  crop = gdk_pixbuf_new_subpixbuf (frame,
                                   x,
                                   y,
                                   (gint) width,
                                   (gint) height);
  texture = gdk_texture_new_for_pixbuf (crop);
  picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_FILL);
  gtk_widget_set_hexpand (picture, TRUE);
  gtk_widget_set_vexpand (picture, TRUE);

  window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_decorated (window, FALSE);
  gtk_window_set_child (window, picture);
  gtk_layer_init_for_window (window);
  gtk_layer_set_namespace (window, "kasasa-frozen-frame");
  gtk_layer_set_layer (window, GTK_LAYER_SHELL_LAYER_OVERLAY);
  gtk_layer_set_monitor (window, monitor);
  gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
  gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
  gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
  gtk_layer_set_anchor (window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
  gtk_layer_set_exclusive_zone (window, -1);
  gtk_layer_set_keyboard_mode (window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  g_ptr_array_add (overlay->windows, g_object_ref_sink (window));
  overlay->pending_windows++;

  wait = g_new0 (FrameOverlayPaintWait, 1);
  wait->overlay = frame_overlay_ref (overlay);
  g_object_set_data_full (G_OBJECT (window),
                          "kasasa-frame-overlay-paint-wait",
                          wait,
                          frame_overlay_paint_wait_free);
  g_signal_connect (window,
                    "map",
                    G_CALLBACK (frame_overlay_window_mapped_cb),
                    wait);
  gtk_window_present (window);
  return TRUE;
}

static gboolean
frame_overlay_show_cb (gpointer user_data)
{
  FrameOverlay *overlay = user_data;
  g_autoptr (GdkPixbuf) frame = NULL;
  g_autoptr (GError) error = NULL;
  GdkDisplay *display;
  GListModel *monitors;
  guint n_monitors;
  gint min_x = G_MAXINT;
  gint min_y = G_MAXINT;
  gint max_x = G_MININT;
  gint max_y = G_MININT;

  if (!gtk_layer_is_supported ())
    {
      guint protocol_version = gtk_layer_get_protocol_version ();

      if (protocol_version > 0)
        g_set_error_literal (&error,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "GTK Layer Shell could not initialize; verify "
                             "that it is linked before libwayland-client");
      else
        g_set_error_literal (&error,
                             G_IO_ERROR,
                             G_IO_ERROR_NOT_SUPPORTED,
                             "The compositor does not support Layer Shell");
      frame_overlay_complete (overlay, g_steal_pointer (&error));
      goto out;
    }

  frame = gdk_pixbuf_new_from_file (overlay->frame_path, &error);
  if (frame == NULL)
    {
      frame_overlay_complete (overlay, g_steal_pointer (&error));
      goto out;
    }

  display = gdk_display_get_default ();
  monitors = display != NULL ? gdk_display_get_monitors (display) : NULL;
  n_monitors = monitors != NULL ? g_list_model_get_n_items (monitors) : 0;

  for (guint i = 0; i < n_monitors; i++)
    {
      g_autoptr (GdkMonitor) monitor = g_list_model_get_item (monitors, i);
      GdkRectangle geometry;

      gdk_monitor_get_geometry (monitor, &geometry);
      min_x = MIN (min_x, geometry.x);
      min_y = MIN (min_y, geometry.y);
      max_x = MAX (max_x, geometry.x + geometry.width);
      max_y = MAX (max_y, geometry.y + geometry.height);
    }

  if (n_monitors == 0 || max_x <= min_x || max_y <= min_y)
    {
      g_set_error_literal (&error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "No display is available for the frozen frame");
      frame_overlay_complete (overlay, g_steal_pointer (&error));
      goto out;
    }

  overlay->geometry.origin_x = min_x;
  overlay->geometry.origin_y = min_y;
  overlay->geometry.logical_width = (guint) (max_x - min_x);
  overlay->geometry.logical_height = (guint) (max_y - min_y);

  for (guint i = 0; i < n_monitors; i++)
    {
      g_autoptr (GdkMonitor) monitor = g_list_model_get_item (monitors, i);

      frame_overlay_add_monitor_window (overlay, monitor, frame);
    }

  if (overlay->pending_windows == 0)
    {
      g_set_error_literal (&error,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "The frozen frame did not cover any display");
      frame_overlay_complete (overlay, g_steal_pointer (&error));
    }

out:
  frame_overlay_unref (overlay);
  return G_SOURCE_REMOVE;
}

static gboolean
frame_overlay_hide_cb (gpointer user_data)
{
  FrameOverlay *overlay = user_data;

  if (overlay->windows != NULL)
    {
      for (guint i = 0; i < overlay->windows->len; i++)
        gtk_window_destroy (GTK_WINDOW (g_ptr_array_index (overlay->windows, i)));
      g_clear_pointer (&overlay->windows, g_ptr_array_unref);
    }

  frame_overlay_unref (overlay);
  return G_SOURCE_REMOVE;
}

static gboolean
frame_overlay_start (FrameOverlay  *overlay,
                     GCancellable *cancellable,
                     GError      **error)
{
  gint64 deadline = g_get_monotonic_time () + FRAME_OVERLAY_TIMEOUT_USEC;
  gboolean success = FALSE;

  g_main_context_invoke (NULL,
                         frame_overlay_show_cb,
                         frame_overlay_ref (overlay));
  g_mutex_lock (&overlay->mutex);
  while (!overlay->completed && !capture_was_cancelled (cancellable))
    {
      gint64 now = g_get_monotonic_time ();

      if (now >= deadline)
        break;
      g_cond_wait_until (&overlay->cond,
                         &overlay->mutex,
                         MIN (deadline, now + 100 * G_TIME_SPAN_MILLISECOND));
    }

  if (capture_was_cancelled (cancellable))
    g_set_error_literal (error,
                         G_IO_ERROR,
                         G_IO_ERROR_CANCELLED,
                         "Region capture cancelled");
  else if (!overlay->completed)
    g_set_error_literal (error,
                         G_IO_ERROR,
                         G_IO_ERROR_TIMED_OUT,
                         "Timed out while presenting the frozen frame");
  else if (overlay->error != NULL)
    g_propagate_error (error, g_error_copy (overlay->error));
  else
    success = TRUE;
  g_mutex_unlock (&overlay->mutex);
  return success;
}

static void
frame_overlay_stop (FrameOverlay *overlay)
{
  g_main_context_invoke (NULL,
                         frame_overlay_hide_cb,
                         frame_overlay_ref (overlay));
  frame_overlay_unref (overlay);
}
#endif

static gboolean
parse_geometry (const gchar *geometry,
                gint        *x,
                gint        *y,
                guint       *width,
                guint       *height)
{
  return geometry_is_valid (geometry)
         && sscanf (geometry, "%d,%d %ux%u", x, y, width, height) == 4;
}

static gboolean
crop_frame (const gchar *frame_path,
            const gchar *geometry,
            const gchar *output_path,
            const FrameGeometry *frame_geometry,
            GError             **error)
{
  g_autoptr (GdkPixbuf) frame = NULL;
  g_autoptr (GdkPixbuf) crop = NULL;
  gint x;
  gint y;
  guint width;
  guint height;
  gint frame_width;
  gint frame_height;
  FrameGeometry fallback_geometry;

  if (!parse_geometry (geometry, &x, &y, &width, &height))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "slurp returned invalid geometry: %s",
                   geometry);
      return FALSE;
    }

  frame = gdk_pixbuf_new_from_file (frame_path, error);
  if (frame == NULL)
    return FALSE;

  frame_width = gdk_pixbuf_get_width (frame);
  frame_height = gdk_pixbuf_get_height (frame);
  if (frame_geometry == NULL)
    {
      fallback_geometry.origin_x = 0;
      fallback_geometry.origin_y = 0;
      fallback_geometry.logical_width = (guint) frame_width;
      fallback_geometry.logical_height = (guint) frame_height;
      frame_geometry = &fallback_geometry;
    }

  if (!map_frame_geometry (frame_geometry,
                           frame_width,
                           frame_height,
                           x,
                           y,
                           width,
                           height,
                           &x,
                           &y,
                           &width,
                           &height))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_DATA,
                   "selection is outside the captured frame: %s",
                   geometry);
      return FALSE;
    }

  crop = gdk_pixbuf_new_subpixbuf (frame, x, y, (gint) width, (gint) height);
  if (!gdk_pixbuf_save (crop, output_path, "png", error, NULL))
    return FALSE;

  return TRUE;
}

static void
capture_region_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  const gchar *slurp_argv[] = { "slurp", NULL };
  const gchar *grim_argv[] = { "grim", NULL, NULL };
  g_autofree gchar *geometry = NULL;
  g_autofree gchar *frame_path = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr (GError) error = NULL;
  const FrameGeometry *frame_geometry = NULL;
  gint fd;
#ifdef KASASA_HAVE_LAYER_SHELL
  FrameOverlay *overlay = NULL;
#endif

  fd = g_file_open_tmp ("kasasa-region-frame-XXXXXX.png", &frame_path, &error);
  if (fd < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  close (fd);

  grim_argv[1] = frame_path;
  if (!run_subprocess (grim_argv, FALSE, cancellable, NULL, &error))
    {
      g_unlink (frame_path);
      if (capture_was_cancelled (cancellable))
        {
          g_clear_error (&error);
          g_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   "Region capture cancelled");
        }
      else
        g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

#ifdef KASASA_HAVE_LAYER_SHELL
  overlay = frame_overlay_new (frame_path);
  if (!frame_overlay_start (overlay, cancellable, &error))
    {
      frame_overlay_stop (overlay);
      g_unlink (frame_path);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  frame_geometry = &overlay->geometry;
#endif

  if (!run_subprocess (slurp_argv, TRUE, cancellable, &geometry, &error))
    {
#ifdef KASASA_HAVE_LAYER_SHELL
      frame_overlay_stop (overlay);
#endif
      g_unlink (frame_path);
      if (capture_was_cancelled (cancellable)
          || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED))
        {
          g_clear_error (&error);
          g_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   "Region selection cancelled");
        }
      else
        g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_strstrip (geometry);

  fd = g_file_open_tmp ("kasasa-region-XXXXXX.png", &path, &error);
  if (fd < 0)
    {
#ifdef KASASA_HAVE_LAYER_SHELL
      frame_overlay_stop (overlay);
#endif
      g_unlink (frame_path);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  close (fd);

  if (!crop_frame (frame_path,
                   geometry,
                   path,
                   frame_geometry,
                   &error))
    {
#ifdef KASASA_HAVE_LAYER_SHELL
      frame_overlay_stop (overlay);
#endif
      g_unlink (frame_path);
      g_unlink (path);
      if (capture_was_cancelled (cancellable))
        {
          g_clear_error (&error);
          g_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   "Region capture cancelled");
        }
      else
        g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

#ifdef KASASA_HAVE_LAYER_SHELL
  frame_overlay_stop (overlay);
#endif
  g_unlink (frame_path);
  uri = g_filename_to_uri (path, NULL, &error);
  if (uri == NULL)
    {
      g_unlink (path);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&uri), g_free);
}

gboolean
kasasa_region_capture_available (void)
{
  g_autofree gchar *slurp = g_find_program_in_path ("slurp");
  g_autofree gchar *grim = g_find_program_in_path ("grim");

  return slurp != NULL && grim != NULL;
}

void
kasasa_region_capture_screenshot_async (GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, kasasa_region_capture_screenshot_async);
  g_task_run_in_thread (task, capture_region_worker);
}

gchar *
kasasa_region_capture_screenshot_finish (GAsyncResult *result,
                                         GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result, kasasa_region_capture_screenshot_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
