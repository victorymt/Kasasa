/* kasasa-hyprland-capture.c
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

#include "config.h"

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "kasasa-hyprland-capture.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-window-query.h"

typedef struct
{
  GMutex mutex;
  GCond cond;
  gboolean completed;
  guint8 *pixels;
  gint width;
  gint height;
  GError *error;
} CaptureFrame;

typedef struct
{
  guint8 *pixels;
  gint width;
  gint height;
} CapturedImage;

static void
set_capture_error_literal (GError      **error,
                           const gchar  *message)
{
  g_set_error_literal (error,
                       KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_FAILED,
                       message);
}

static guint8 *
copy_frame_rgba (const guint8                *data,
                 gint                         width,
                 gint                         height,
                 gint                         stride,
                 KasasaHyprlandStreamFormat   format,
                 gboolean                     y_invert,
                 guint32                      transform,
                 gint                        *output_width,
                 gint                        *output_height,
                 GError                     **error)
{
  gsize source_row_size;
  gsize output_row_size;
  gsize output_size;
  guint8 *pixels;
  gint transformed_width;
  gint transformed_height;
  gint x;
  gint y;

  if (data == NULL || width <= 0 || height <= 0 || stride <= 0
      || (gsize) width > G_MAXSIZE / 4)
    {
      set_capture_error_literal (
        error,
        _("The compositor provided an invalid window frame"));
      return NULL;
    }

  source_row_size = (gsize) width * 4;
  if ((gsize) stride < source_row_size)
    {
      set_capture_error_literal (
        error,
        _("The compositor provided an invalid window frame"));
      return NULL;
    }

  if (transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
    transform = WL_OUTPUT_TRANSFORM_NORMAL;
  if (transform == WL_OUTPUT_TRANSFORM_90
      || transform == WL_OUTPUT_TRANSFORM_270
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
      || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
      transformed_width = height;
      transformed_height = width;
    }
  else
    {
      transformed_width = width;
      transformed_height = height;
    }

  output_row_size = (gsize) transformed_width * 4;
  if ((gsize) transformed_height > G_MAXSIZE / output_row_size)
    {
      set_capture_error_literal (error, _("The window frame is too large"));
      return NULL;
    }
  output_size = output_row_size * (gsize) transformed_height;
  pixels = g_try_malloc (output_size);
  if (pixels == NULL)
    {
      set_capture_error_literal (error,
                                 _("Couldn't allocate a window screenshot"));
      return NULL;
    }

  for (y = 0; y < transformed_height; y++)
    {
      for (x = 0; x < transformed_width; x++)
        {
          const guint8 *source;
          guint8 *destination;
          gint source_x;
          gint source_y;

          switch (transform)
            {
            case WL_OUTPUT_TRANSFORM_90:
              source_x = width - 1 - y;
              source_y = x;
              break;
            case WL_OUTPUT_TRANSFORM_180:
              source_x = width - 1 - x;
              source_y = height - 1 - y;
              break;
            case WL_OUTPUT_TRANSFORM_270:
              source_x = y;
              source_y = height - 1 - x;
              break;
            case WL_OUTPUT_TRANSFORM_FLIPPED:
              source_x = width - 1 - x;
              source_y = y;
              break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_90:
              source_x = y;
              source_y = x;
              break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_180:
              source_x = x;
              source_y = height - 1 - y;
              break;
            case WL_OUTPUT_TRANSFORM_FLIPPED_270:
              source_x = width - 1 - y;
              source_y = height - 1 - x;
              break;
            case WL_OUTPUT_TRANSFORM_NORMAL:
            default:
              source_x = x;
              source_y = y;
              break;
            }

          if (y_invert)
            source_y = height - 1 - source_y;

          source = data + (gsize) source_y * (gsize) stride
                        + (gsize) source_x * 4;
          destination = pixels + (gsize) y * output_row_size
                               + (gsize) x * 4;

          switch (format)
            {
            case KASASA_HYPRLAND_STREAM_FORMAT_BGRX:
              destination[0] = source[2];
              destination[1] = source[1];
              destination[2] = source[0];
              destination[3] = 0xff;
              break;
            case KASASA_HYPRLAND_STREAM_FORMAT_BGRA:
              destination[0] = source[2];
              destination[1] = source[1];
              destination[2] = source[0];
              destination[3] = source[3];
              break;
            case KASASA_HYPRLAND_STREAM_FORMAT_RGBX:
              destination[0] = source[0];
              destination[1] = source[1];
              destination[2] = source[2];
              destination[3] = 0xff;
              break;
            case KASASA_HYPRLAND_STREAM_FORMAT_RGBA:
              memcpy (destination, source, 4);
              break;
            default:
              g_free (pixels);
              set_capture_error_literal (
                error,
                _("The compositor provided an unsupported window frame format"));
              return NULL;
            }
        }
    }

  *output_width = transformed_width;
  *output_height = transformed_height;
  return pixels;
}

static void
complete_capture (CaptureFrame *capture,
                  guint8       *pixels,
                  gint          width,
                  gint          height,
                  GError       *error)
{
  g_mutex_lock (&capture->mutex);
  if (!capture->completed)
    {
      capture->pixels = pixels;
      capture->width = width;
      capture->height = height;
      capture->error = error;
      capture->completed = TRUE;
      g_cond_signal (&capture->cond);
      pixels = NULL;
      error = NULL;
    }
  g_mutex_unlock (&capture->mutex);

  g_free (pixels);
  g_clear_error (&error);
}

static void
on_capture_frame (gpointer                     user_data,
                  const guint8                *data,
                  gint                         width,
                  gint                         height,
                  gint                         stride,
                  KasasaHyprlandStreamFormat   format,
                  gboolean                     y_invert,
                  guint32                      transform)
{
  CaptureFrame *capture = user_data;
  g_autoptr (GError) error = NULL;
  guint8 *pixels;
  gint output_width = 0;
  gint output_height = 0;

  pixels = copy_frame_rgba (data,
                            width,
                            height,
                            stride,
                            format,
                            y_invert,
                            transform,
                            &output_width,
                            &output_height,
                            &error);
  complete_capture (capture,
                    pixels,
                    output_width,
                    output_height,
                    g_steal_pointer (&error));
}

static void
on_capture_error (gpointer      user_data,
                  const GError *error)
{
  CaptureFrame *capture = user_data;
  GError *copy;

  if (error != NULL)
    copy = g_error_copy (error);
  else
    copy = g_error_new_literal (KASASA_WINDOW_QUERY_ERROR,
                                KASASA_WINDOW_QUERY_ERROR_FAILED,
                                _("Couldn't capture the window"));
  complete_capture (capture, NULL, 0, 0, copy);
}

gboolean
kasasa_hyprland_capture_available (void)
{
  return kasasa_hyprland_stream_available ();
}

static void
captured_image_free (CapturedImage *image)
{
  g_free (image->pixels);
  g_free (image);
}

static CapturedImage *
capture_window_frame (const KasasaWindowClient *client,
                      GError                  **error)
{
  CaptureFrame capture = { 0 };
  KasasaHyprlandStream *stream = NULL;
  CapturedImage *image = NULL;
  guint32 handle = 0;

  if (!kasasa_hyprland_capture_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland window capture requires Wayland and hyprctl"));
      return NULL;
    }

  if (!kasasa_hyprland_stream_handle_from_address (client->address,
                                                   &handle,
                                                   error))
    return NULL;

  g_mutex_init (&capture.mutex);
  g_cond_init (&capture.cond);
  stream = kasasa_hyprland_stream_start (handle,
                                         120,
                                         on_capture_frame,
                                         on_capture_error,
                                         &capture,
                                         NULL,
                                         error);
  if (stream == NULL)
    goto out;

  g_mutex_lock (&capture.mutex);
  while (!capture.completed)
    g_cond_wait (&capture.cond, &capture.mutex);
  g_mutex_unlock (&capture.mutex);

  kasasa_hyprland_stream_stop (stream);
  stream = NULL;

  if (capture.error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&capture.error));
      goto out;
    }

  image = g_new0 (CapturedImage, 1);
  image->pixels = g_steal_pointer (&capture.pixels);
  image->width = capture.width;
  image->height = capture.height;

out:
  if (stream != NULL)
    kasasa_hyprland_stream_stop (stream);
  g_free (capture.pixels);
  g_clear_error (&capture.error);
  g_cond_clear (&capture.cond);
  g_mutex_clear (&capture.mutex);
  return image;
}

static gchar *
save_captured_image (CapturedImage *image,
                     GError       **error)
{
  g_autoptr (GBytes) bytes = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  g_autofree gchar *path = NULL;
  gchar *uri = NULL;
  gsize stride;
  gsize size;
  gint fd;

  g_return_val_if_fail (image != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  stride = (gsize) image->width * 4;
  size = stride * (gsize) image->height;
  bytes = g_bytes_new_take (g_steal_pointer (&image->pixels), size);
  texture = gdk_memory_texture_new (image->width,
                                    image->height,
                                    GDK_MEMORY_R8G8B8A8_PREMULTIPLIED,
                                    bytes,
                                    stride);

  fd = g_file_open_tmp ("kasasa-capture-XXXXXX.png", &path, error);
  if (fd < 0)
    goto out;
  close (fd);

  if (!gdk_texture_save_to_png (texture, path))
    {
      g_unlink (path);
      set_capture_error_literal (error, _("Couldn't save the window screenshot"));
      goto out;
    }

  uri = g_filename_to_uri (path, NULL, error);
  if (uri == NULL)
    g_unlink (path);

out:
  return uri;
}

gchar *
kasasa_hyprland_capture_screenshot (const KasasaWindowClient *client,
                                    GError                  **error)
{
  g_autoptr (GError) local_error = NULL;
  CapturedImage *image;
  gchar *uri;

  g_return_val_if_fail (client != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  image = capture_window_frame (client, &local_error);
  if (image == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return NULL;
    }

  uri = save_captured_image (image, error);
  captured_image_free (image);
  return uri;
}

static void
capture_screenshot_thread (GTask        *task,
                           gpointer      source_object,
                           gpointer      task_data,
                           GCancellable *cancellable)
{
  const KasasaWindowClient *client = task_data;
  g_autoptr (GError) error = NULL;
  CapturedImage *image;

  if (g_task_return_error_if_cancelled (task))
    return;

  image = capture_window_frame (client, &error);
  if (image == NULL)
    {
      if (error == NULL)
        set_capture_error_literal (&error, _("Couldn't capture the window"));
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (g_task_return_error_if_cancelled (task))
    {
      captured_image_free (image);
      return;
    }

  g_task_return_pointer (task, image, (GDestroyNotify) captured_image_free);
}

void
kasasa_hyprland_capture_screenshot_async (const KasasaWindowClient *client,
                                          GCancellable             *cancellable,
                                          GAsyncReadyCallback       callback,
                                          gpointer                  user_data)
{
  GTask *task;

  g_return_if_fail (client != NULL);

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, kasasa_hyprland_capture_screenshot_async);
  g_task_set_task_data (task,
                        kasasa_window_client_copy (client),
                        (GDestroyNotify) kasasa_window_client_free);
  g_task_set_return_on_cancel (task, TRUE);
  g_task_run_in_thread (task, capture_screenshot_thread);
  g_object_unref (task);
}

gchar *
kasasa_hyprland_capture_screenshot_finish (GAsyncResult *result,
                                           GError      **error)
{
  CapturedImage *image;
  gchar *uri;

  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result,
                          kasasa_hyprland_capture_screenshot_async),
                        NULL);

  image = g_task_propagate_pointer (G_TASK (result), error);
  if (image == NULL)
    return NULL;

  uri = save_captured_image (image, error);
  captured_image_free (image);
  return uri;
}
