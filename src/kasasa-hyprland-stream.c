/* kasasa-hyprland-stream.c
 *
 * Continuous window capture via hyprland-toplevel-export-v1.
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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#include <glib/gi18n.h>
#include <wayland-client.h>

#include "hyprland-toplevel-export-v1-client.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-window-query.h"

#define FRAME_STAGE_TIMEOUT_USEC          (G_TIME_SPAN_SECOND)
#define EVENT_POLL_INTERVAL_MSEC          100
#define MAX_CONSECUTIVE_FRAME_FAILURES    3

struct _KasasaHyprlandStream
{
  GThread *thread;
  gint stop_requested;

  guint32 handle;

  KasasaHyprlandStreamFrameFunc frame_cb;
  KasasaHyprlandStreamErrorFunc error_cb;
  gpointer user_data;
  GDestroyNotify user_data_destroy;

  /* Wayland objects owned by the worker thread */
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct hyprland_toplevel_export_manager_v1 *export_manager;

  /* Per-frame state */
  struct hyprland_toplevel_export_frame_v1 *frame;
  struct wl_buffer *buffer;
  void *buffer_data;
  size_t buffer_size;
  int buffer_fd;
  uint32_t buffer_format;
  uint32_t buffer_width;
  uint32_t buffer_height;
  uint32_t buffer_stride;
  uint32_t allocated_buffer_format;
  uint32_t allocated_buffer_width;
  uint32_t allocated_buffer_height;
  uint32_t allocated_buffer_stride;
  gboolean buffer_info_ready;
  gboolean buffer_done;
  gboolean frame_failed;
  gboolean frame_ready;
  uint32_t frame_flags;
};

static gboolean
stream_stop_requested (KasasaHyprlandStream *self)
{
  return g_atomic_int_get (&self->stop_requested) != 0;
}

static gboolean
set_stream_error_literal (GError      **error,
                          const gchar  *message)
{
  g_set_error_literal (error,
                       KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_FAILED,
                       message);
  return FALSE;
}

static gboolean
set_stream_errno_error (GError      **error,
                        const gchar  *message)
{
  int saved_errno = errno;

  g_set_error (error,
               KASASA_WINDOW_QUERY_ERROR,
               KASASA_WINDOW_QUERY_ERROR_FAILED,
               "%s: %s",
               message,
               g_strerror (saved_errno));
  return FALSE;
}

gboolean
kasasa_hyprland_stream_handle_from_address (const gchar *address,
                                            guint32     *handle,
                                            GError     **error)
{
  gchar *end = NULL;
  guint64 value;

  g_return_val_if_fail (handle != NULL, FALSE);

  if (address == NULL || *address == '\0')
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Window address is empty"));
      return FALSE;
    }

  errno = 0;
  value = g_ascii_strtoull (address, &end, 0);
  if (errno != 0 || end == address || (end != NULL && *end != '\0'))
    {
      g_set_error (error,
                   KASASA_WINDOW_QUERY_ERROR,
                   KASASA_WINDOW_QUERY_ERROR_FAILED,
                   _("Invalid window address “%s”"),
                   address);
      return FALSE;
    }

  /* Protocol handle is the lower 32 bits of the hyprctl address. */
  *handle = (guint32) (value & 0xffffffffu);
  return TRUE;
}

gboolean
kasasa_hyprland_stream_available (void)
{
  const gchar *display = g_getenv ("WAYLAND_DISPLAY");
  return display != NULL && *display != '\0'
         && kasasa_window_query_backend_available ();
}

static void
stream_clear_buffer (KasasaHyprlandStream *self)
{
  if (self->buffer != NULL)
    {
      wl_buffer_destroy (self->buffer);
      self->buffer = NULL;
    }
  if (self->buffer_data != NULL && self->buffer_data != MAP_FAILED)
    {
      munmap (self->buffer_data, self->buffer_size);
      self->buffer_data = NULL;
    }
  if (self->buffer_fd >= 0)
    {
      close (self->buffer_fd);
      self->buffer_fd = -1;
    }
  self->buffer_size = 0;
  self->allocated_buffer_format = 0;
  self->allocated_buffer_width = 0;
  self->allocated_buffer_height = 0;
  self->allocated_buffer_stride = 0;
}

static void
stream_clear_frame (KasasaHyprlandStream *self)
{
  if (self->frame != NULL)
    {
      hyprland_toplevel_export_frame_v1_destroy (self->frame);
      self->frame = NULL;
    }
  self->buffer_info_ready = FALSE;
  self->buffer_done = FALSE;
  self->frame_failed = FALSE;
  self->frame_ready = FALSE;
  self->frame_flags = 0;
  self->buffer_format = 0;
  self->buffer_width = 0;
  self->buffer_height = 0;
  self->buffer_stride = 0;
}

static int
create_shm_file (size_t size)
{
  int fd = -1;

#ifdef __linux__
  fd = (int) syscall (SYS_memfd_create, "kasasa-hypr-stream", MFD_CLOEXEC);
#endif
  if (fd < 0)
    {
      g_autofree gchar *name = g_strdup_printf ("/kasasa-hypr-stream-%u-%u",
                                                (guint) getpid (),
                                                g_random_int ());
      fd = shm_open (name, O_RDWR | O_CREAT | O_EXCL, 0600);
      if (fd >= 0)
        shm_unlink (name);
    }

  if (fd < 0)
    return -1;

  if (ftruncate (fd, (off_t) size) < 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

static gboolean
ensure_shm_buffer (KasasaHyprlandStream *self)
{
  struct wl_shm_pool *pool;
  size_t size;

  if (self->shm == NULL || self->buffer_width == 0 || self->buffer_height == 0
      || self->buffer_stride == 0)
    return FALSE;

  if (self->buffer_width > G_MAXINT32
      || self->buffer_height > G_MAXINT32
      || self->buffer_stride > G_MAXINT32
      || (size_t) self->buffer_height > G_MAXSIZE / self->buffer_stride)
    return FALSE;

  size = (size_t) self->buffer_stride * (size_t) self->buffer_height;
  if (size == 0 || size > G_MAXINT32)
    return FALSE;

  if (self->buffer != NULL
      && self->allocated_buffer_format == self->buffer_format
      && self->allocated_buffer_width == self->buffer_width
      && self->allocated_buffer_height == self->buffer_height
      && self->allocated_buffer_stride == self->buffer_stride)
    return TRUE;

  stream_clear_buffer (self);

  self->buffer_fd = create_shm_file (size);
  if (self->buffer_fd < 0)
    return FALSE;

  self->buffer_data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                            self->buffer_fd, 0);
  if (self->buffer_data == MAP_FAILED)
    {
      self->buffer_data = NULL;
      close (self->buffer_fd);
      self->buffer_fd = -1;
      return FALSE;
    }
  self->buffer_size = size;
  memset (self->buffer_data, 0, size);

  pool = wl_shm_create_pool (self->shm, self->buffer_fd, (int32_t) size);
  if (pool == NULL)
    {
      stream_clear_buffer (self);
      return FALSE;
    }

  self->buffer = wl_shm_pool_create_buffer (pool,
                                            0,
                                            (int32_t) self->buffer_width,
                                            (int32_t) self->buffer_height,
                                            (int32_t) self->buffer_stride,
                                            self->buffer_format);
  wl_shm_pool_destroy (pool);

  if (self->buffer == NULL)
    {
      stream_clear_buffer (self);
      return FALSE;
    }

  self->allocated_buffer_format = self->buffer_format;
  self->allocated_buffer_width = self->buffer_width;
  self->allocated_buffer_height = self->buffer_height;
  self->allocated_buffer_stride = self->buffer_stride;

  return TRUE;
}

static void
frame_handle_buffer (void                                   *data,
                     struct hyprland_toplevel_export_frame_v1 *frame,
                     uint32_t                                format,
                     uint32_t                                width,
                     uint32_t                                height,
                     uint32_t                                stride)
{
  KasasaHyprlandStream *self = data;

  (void) frame;

  /* Prefer wl_shm formats we can convert easily. */
  if (format != WL_SHM_FORMAT_XRGB8888
      && format != WL_SHM_FORMAT_ARGB8888
      && format != WL_SHM_FORMAT_XBGR8888
      && format != WL_SHM_FORMAT_ABGR8888)
    {
      g_debug ("Ignoring unsupported shm format 0x%x", format);
      return;
    }

  self->buffer_format = format;
  self->buffer_width = width;
  self->buffer_height = height;
  self->buffer_stride = stride;
  self->buffer_info_ready = TRUE;
}

static void
frame_handle_damage (void                                   *data,
                     struct hyprland_toplevel_export_frame_v1 *frame,
                     uint32_t                                x,
                     uint32_t                                y,
                     uint32_t                                width,
                     uint32_t                                height)
{
  (void) data;
  (void) frame;
  (void) x;
  (void) y;
  (void) width;
  (void) height;
}

static void
frame_handle_flags (void                                   *data,
                    struct hyprland_toplevel_export_frame_v1 *frame,
                    uint32_t                                flags)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->frame_flags = flags;
}

static void
frame_handle_ready (void                                   *data,
                    struct hyprland_toplevel_export_frame_v1 *frame,
                    uint32_t                                tv_sec_hi,
                    uint32_t                                tv_sec_lo,
                    uint32_t                                tv_nsec)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  (void) tv_sec_hi;
  (void) tv_sec_lo;
  (void) tv_nsec;
  self->frame_ready = TRUE;
}

static void
frame_handle_failed (void                                   *data,
                     struct hyprland_toplevel_export_frame_v1 *frame)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->frame_failed = TRUE;
}

static void
frame_handle_linux_dmabuf (void                                   *data,
                           struct hyprland_toplevel_export_frame_v1 *frame,
                           uint32_t                                format,
                           uint32_t                                width,
                           uint32_t                                height)
{
  (void) data;
  (void) frame;
  (void) format;
  (void) width;
  (void) height;
  /* Prefer wl_shm path for simple CPU delivery into GStreamer. */
}

static void
frame_handle_buffer_done (void                                   *data,
                          struct hyprland_toplevel_export_frame_v1 *frame)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->buffer_done = TRUE;
}

static const struct hyprland_toplevel_export_frame_v1_listener frame_listener = {
  .buffer = frame_handle_buffer,
  .damage = frame_handle_damage,
  .flags = frame_handle_flags,
  .ready = frame_handle_ready,
  .failed = frame_handle_failed,
  .linux_dmabuf = frame_handle_linux_dmabuf,
  .buffer_done = frame_handle_buffer_done,
};

static void
registry_handle_global (void               *data,
                        struct wl_registry *registry,
                        uint32_t            name,
                        const char         *interface,
                        uint32_t            version)
{
  KasasaHyprlandStream *self = data;

  if (g_strcmp0 (interface, wl_shm_interface.name) == 0)
    {
      self->shm = wl_registry_bind (registry, name, &wl_shm_interface, 1);
    }
  else if (g_strcmp0 (interface,
                      hyprland_toplevel_export_manager_v1_interface.name) == 0)
    {
      uint32_t bind_version = MIN (version, 2);
      self->export_manager =
        wl_registry_bind (registry,
                          name,
                          &hyprland_toplevel_export_manager_v1_interface,
                          bind_version);
    }
}

static void
registry_handle_global_remove (void               *data,
                               struct wl_registry *registry,
                               uint32_t            name)
{
  (void) data;
  (void) registry;
  (void) name;
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};

static gboolean
dispatch_pending (KasasaHyprlandStream  *self,
                  GError               **error)
{
  if (wl_display_dispatch_pending (self->display) < 0)
    return set_stream_errno_error (error, _("Wayland event dispatch failed"));
  if (wl_display_flush (self->display) < 0 && errno != EAGAIN)
    return set_stream_errno_error (error, _("Wayland request flush failed"));
  return TRUE;
}

static gboolean
wait_events (KasasaHyprlandStream  *self,
             gint64                 deadline,
             const gchar           *timeout_message,
             GError               **error)
{
  struct pollfd pfd;
  int ret;

  while (!stream_stop_requested (self))
    {
      gint64 remaining;
      gint timeout_msec;

      while (wl_display_prepare_read (self->display) != 0)
        {
          if (wl_display_dispatch_pending (self->display) < 0)
            return set_stream_errno_error (error,
                                           _("Wayland event dispatch failed"));
        }

      if (wl_display_flush (self->display) < 0 && errno != EAGAIN)
        {
          wl_display_cancel_read (self->display);
          return set_stream_errno_error (error,
                                         _("Wayland request flush failed"));
        }

      remaining = deadline - g_get_monotonic_time ();
      if (remaining <= 0)
        {
          wl_display_cancel_read (self->display);
          return set_stream_error_literal (error, timeout_message);
        }

      timeout_msec = (gint) MIN ((remaining + 999) / 1000,
                                 EVENT_POLL_INTERVAL_MSEC);
      pfd.fd = wl_display_get_fd (self->display);
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll (&pfd, 1, timeout_msec);
      if (ret < 0)
        {
          wl_display_cancel_read (self->display);
          if (errno == EINTR)
            continue;
          return set_stream_errno_error (error,
                                         _("Waiting for Wayland events failed"));
        }
      if (ret == 0)
        {
          wl_display_cancel_read (self->display);
          if (g_get_monotonic_time () >= deadline)
            return set_stream_error_literal (error, timeout_message);
          return TRUE;
        }
      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
          wl_display_cancel_read (self->display);
          return set_stream_error_literal (error,
                                           _("The Wayland connection was closed"));
        }

      if (wl_display_read_events (self->display) < 0)
        return set_stream_errno_error (error,
                                       _("Reading Wayland events failed"));
      if (wl_display_dispatch_pending (self->display) < 0)
        return set_stream_errno_error (error,
                                       _("Wayland event dispatch failed"));
      return TRUE;
    }

  return FALSE;
}

typedef struct
{
  gboolean done;
} RoundtripState;

static void
roundtrip_done (void               *data,
                struct wl_callback *callback,
                uint32_t            callback_data)
{
  RoundtripState *state = data;

  (void) callback;
  (void) callback_data;
  state->done = TRUE;
}

static const struct wl_callback_listener roundtrip_listener = {
  .done = roundtrip_done,
};

static gboolean
roundtrip_with_timeout (KasasaHyprlandStream  *self,
                        GError               **error)
{
  struct wl_callback *callback;
  RoundtripState state = { FALSE };
  gint64 deadline = g_get_monotonic_time () + FRAME_STAGE_TIMEOUT_USEC;

  callback = wl_display_sync (self->display);
  if (callback == NULL)
    return set_stream_error_literal (error,
                                     _("Couldn't synchronize with Wayland"));

  wl_callback_add_listener (callback, &roundtrip_listener, &state);
  while (!stream_stop_requested (self) && !state.done)
    {
      if (!dispatch_pending (self, error)
          || (!state.done
              && !wait_events (self,
                               deadline,
                               _("Timed out waiting for the Wayland compositor"),
                               error)))
        {
          wl_callback_destroy (callback);
          return FALSE;
        }
    }

  wl_callback_destroy (callback);
  return state.done;
}

static void
emit_frame (KasasaHyprlandStream *self)
{
  gint width = (gint) self->buffer_width;
  gint height = (gint) self->buffer_height;
  gint stride = (gint) self->buffer_stride;
  gboolean y_invert =
    (self->frame_flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT) != 0;
  KasasaHyprlandStreamFormat format;

  if (self->buffer_data == NULL || width <= 0 || height <= 0
      || stride <= 0 || self->frame_cb == NULL)
    return;

  switch (self->buffer_format)
    {
    case WL_SHM_FORMAT_XRGB8888:
      format = KASASA_HYPRLAND_STREAM_FORMAT_BGRX;
      break;
    case WL_SHM_FORMAT_ARGB8888:
      format = KASASA_HYPRLAND_STREAM_FORMAT_BGRA;
      break;
    case WL_SHM_FORMAT_XBGR8888:
      format = KASASA_HYPRLAND_STREAM_FORMAT_RGBX;
      break;
    case WL_SHM_FORMAT_ABGR8888:
      format = KASASA_HYPRLAND_STREAM_FORMAT_RGBA;
      break;
    default:
      return;
    }

  self->frame_cb (self->user_data,
                  self->buffer_data,
                  width,
                  height,
                  stride,
                  format,
                  y_invert);
}

static gboolean
capture_one_frame (KasasaHyprlandStream  *self,
                   gboolean              *retryable,
                   GError               **error)
{
  gint64 deadline;

  if (retryable != NULL)
    *retryable = FALSE;

  stream_clear_frame (self);

  self->frame = hyprland_toplevel_export_manager_v1_capture_toplevel (
    self->export_manager, 0, self->handle);
  if (self->frame == NULL)
    return set_stream_error_literal (error,
                                     _("Couldn't request a window frame"));

  hyprland_toplevel_export_frame_v1_add_listener (self->frame,
                                                  &frame_listener,
                                                  self);

  deadline = g_get_monotonic_time () + FRAME_STAGE_TIMEOUT_USEC;
  while (!stream_stop_requested (self)
         && !self->buffer_done
         && !self->frame_failed)
    {
      if (!dispatch_pending (self, error))
        return FALSE;
      if (self->buffer_done || self->frame_failed)
        break;
      if (!wait_events (self,
                        deadline,
                        _("Timed out waiting for a window frame"),
                        error))
        return FALSE;
    }

  if (stream_stop_requested (self))
    return FALSE;
  if (self->frame_failed)
    {
      if (retryable != NULL)
        *retryable = TRUE;
      return set_stream_error_literal (error,
                                       _("The selected window is no longer available"));
    }
  if (!self->buffer_info_ready)
    return set_stream_error_literal (error,
                                     _("The compositor provided no supported frame format"));

  if (!ensure_shm_buffer (self))
    return set_stream_error_literal (error,
                                     _("Couldn't allocate a window frame buffer"));

  /* ignore_damage=1 so we get a continuous stream even without damage events. */
  hyprland_toplevel_export_frame_v1_copy (self->frame, self->buffer, 1);

  deadline = g_get_monotonic_time () + FRAME_STAGE_TIMEOUT_USEC;
  while (!stream_stop_requested (self)
         && !self->frame_ready
         && !self->frame_failed)
    {
      if (!dispatch_pending (self, error))
        return FALSE;
      if (self->frame_ready || self->frame_failed)
        break;
      if (!wait_events (self,
                        deadline,
                        _("Timed out waiting for a window frame"),
                        error))
        return FALSE;
    }

  if (stream_stop_requested (self))
    return FALSE;
  if (self->frame_failed)
    {
      if (retryable != NULL)
        *retryable = TRUE;
      return set_stream_error_literal (error,
                                       _("The selected window is no longer available"));
    }
  if (!self->frame_ready)
    return set_stream_error_literal (error,
                                     _("The compositor did not complete the window frame"));

  emit_frame (self);
  stream_clear_frame (self);
  return TRUE;
}

static gpointer
stream_thread_func (gpointer data)
{
  KasasaHyprlandStream *self = data;
  g_autoptr (GError) stream_error = NULL;
  gboolean retryable;
  guint consecutive_failures = 0;

  self->display = wl_display_connect (NULL);
  if (self->display == NULL)
    {
      set_stream_errno_error (&stream_error,
                              _("Failed to connect to the Wayland display"));
      goto out;
    }

  self->registry = wl_display_get_registry (self->display);
  if (self->registry == NULL)
    {
      set_stream_error_literal (&stream_error,
                                _("Couldn't access the Wayland registry"));
      goto out;
    }
  wl_registry_add_listener (self->registry, &registry_listener, self);
  if (!roundtrip_with_timeout (self, &stream_error))
    goto out;

  if (self->shm == NULL || self->export_manager == NULL)
    {
      set_stream_error_literal (&stream_error,
                                _("Hyprland's window capture protocol is unavailable"));
      goto out;
    }

  if (!capture_one_frame (self, NULL, &stream_error))
    goto out;

  while (!stream_stop_requested (self))
    {
      g_clear_error (&stream_error);
      if (!capture_one_frame (self, &retryable, &stream_error))
        {
          if (stream_stop_requested (self))
            break;

          if (!retryable)
            break;

          consecutive_failures++;
          if (consecutive_failures >= MAX_CONSECUTIVE_FRAME_FAILURES)
            break;

          g_debug ("Hyprland frame capture failed (%u/%u): %s",
                   consecutive_failures,
                   MAX_CONSECUTIVE_FRAME_FAILURES,
                   stream_error != NULL ? stream_error->message : "unknown error");
          g_usleep (50 * 1000);
        }
      else
        {
          consecutive_failures = 0;
          /* Cap roughly to 30 FPS when ignore_damage forces full frames. */
          g_usleep (33 * 1000);
        }
    }

out:
  stream_clear_frame (self);
  stream_clear_buffer (self);
  if (self->export_manager != NULL)
    {
      hyprland_toplevel_export_manager_v1_destroy (self->export_manager);
      self->export_manager = NULL;
    }
  if (self->shm != NULL)
    {
      wl_shm_destroy (self->shm);
      self->shm = NULL;
    }
  if (self->registry != NULL)
    {
      wl_registry_destroy (self->registry);
      self->registry = NULL;
    }
  if (self->display != NULL)
    {
      wl_display_disconnect (self->display);
      self->display = NULL;
    }

  if (!stream_stop_requested (self) && self->error_cb != NULL)
    {
      if (stream_error == NULL)
        set_stream_error_literal (&stream_error,
                                  _("Couldn't start Hyprland window capture"));
      self->error_cb (self->user_data, stream_error);
    }

  return NULL;
}

KasasaHyprlandStream *
kasasa_hyprland_stream_start (guint32                       handle,
                              KasasaHyprlandStreamFrameFunc  frame_cb,
                              KasasaHyprlandStreamErrorFunc  error_cb,
                              gpointer                      user_data,
                              GDestroyNotify                user_data_destroy,
                              GError                      **error)
{
  KasasaHyprlandStream *self;
  g_autoptr (GError) thread_error = NULL;

  g_return_val_if_fail (frame_cb != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (!kasasa_hyprland_stream_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland live capture requires Wayland and hyprctl"));
      return NULL;
    }

  self = g_new0 (KasasaHyprlandStream, 1);
  self->handle = handle;
  self->frame_cb = frame_cb;
  self->error_cb = error_cb;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;
  self->buffer_fd = -1;

  self->thread = g_thread_try_new ("kasasa-hypr-stream",
                                   stream_thread_func,
                                   self,
                                   &thread_error);
  if (self->thread == NULL)
    {
      g_set_error (error,
                   KASASA_WINDOW_QUERY_ERROR,
                   KASASA_WINDOW_QUERY_ERROR_FAILED,
                   _("Failed to start Hyprland capture thread: %s"),
                   thread_error != NULL ? thread_error->message : _("unknown error"));
      kasasa_hyprland_stream_stop (self);
      return NULL;
    }

  return self;
}

void
kasasa_hyprland_stream_stop (KasasaHyprlandStream *self)
{
  if (self == NULL)
    return;

  g_atomic_int_set (&self->stop_requested, TRUE);

  if (self->thread != NULL)
    {
      g_thread_join (self->thread);
      self->thread = NULL;
    }

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  g_free (self);
}
