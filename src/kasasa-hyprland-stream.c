/* kasasa-hyprland-stream.c
 *
 * Continuous window capture via hyprland-toplevel-export-v1.
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

struct _KasasaHyprlandStream
{
  GThread *thread;
  GMutex lock;
  gboolean stop_requested;
  gboolean started;

  guint32 handle;

  KasasaHyprlandStreamFrameFunc frame_cb;
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
  gboolean buffer_info_ready;
  gboolean buffer_done;
  gboolean frame_failed;
  gboolean frame_ready;
  uint32_t frame_flags;
};

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
}

static void
stream_clear_frame (KasasaHyprlandStream *self)
{
  if (self->frame != NULL)
    {
      hyprland_toplevel_export_frame_v1_destroy (self->frame);
      self->frame = NULL;
    }
  stream_clear_buffer (self);
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
create_shm_buffer (KasasaHyprlandStream *self)
{
  struct wl_shm_pool *pool;
  size_t size;

  if (self->shm == NULL || self->buffer_width == 0 || self->buffer_height == 0
      || self->buffer_stride == 0)
    return FALSE;

  size = (size_t) self->buffer_stride * (size_t) self->buffer_height;
  if (size == 0)
    return FALSE;

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
dispatch_pending (KasasaHyprlandStream *self)
{
  if (wl_display_dispatch_pending (self->display) < 0)
    return FALSE;
  if (wl_display_flush (self->display) < 0 && errno != EAGAIN)
    return FALSE;
  return TRUE;
}

static gboolean
wait_events (KasasaHyprlandStream *self)
{
  struct pollfd pfd;
  int ret;

  while (!self->stop_requested)
    {
      while (wl_display_prepare_read (self->display) != 0)
        {
          if (wl_display_dispatch_pending (self->display) < 0)
            return FALSE;
        }

      if (wl_display_flush (self->display) < 0 && errno != EAGAIN)
        {
          wl_display_cancel_read (self->display);
          return FALSE;
        }

      pfd.fd = wl_display_get_fd (self->display);
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll (&pfd, 1, 100);
      if (ret < 0)
        {
          wl_display_cancel_read (self->display);
          if (errno == EINTR)
            continue;
          return FALSE;
        }
      if (ret == 0)
        {
          wl_display_cancel_read (self->display);
          return TRUE; /* timeout, caller re-checks flags */
        }

      if (wl_display_read_events (self->display) < 0)
        return FALSE;
      if (wl_display_dispatch_pending (self->display) < 0)
        return FALSE;
      return TRUE;
    }

  return FALSE;
}

static void
emit_frame (KasasaHyprlandStream *self)
{
  g_autofree guint8 *packed = NULL;
  const guint8 *src = self->buffer_data;
  gint width = (gint) self->buffer_width;
  gint height = (gint) self->buffer_height;
  gint stride = (gint) self->buffer_stride;
  gboolean y_invert =
    (self->frame_flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT) != 0;
  gboolean has_alpha = self->buffer_format == WL_SHM_FORMAT_ARGB8888
                       || self->buffer_format == WL_SHM_FORMAT_ABGR8888;
  gboolean swap_rb = self->buffer_format == WL_SHM_FORMAT_XBGR8888
                     || self->buffer_format == WL_SHM_FORMAT_ABGR8888;
  gint out_stride = width * 4;
  gint y;

  if (src == NULL || width <= 0 || height <= 0 || self->frame_cb == NULL)
    return;

  packed = g_malloc ((gsize) out_stride * (gsize) height);

  for (y = 0; y < height; y++)
    {
      gint src_y = y_invert ? (height - 1 - y) : y;
      const guint8 *src_row = src + (gsize) src_y * (gsize) stride;
      guint8 *dst_row = packed + (gsize) y * (gsize) out_stride;
      gint x;

      for (x = 0; x < width; x++)
        {
          const guint8 *p = src_row + x * 4;
          guint8 *d = dst_row + x * 4;

          if (swap_rb)
            {
              /* XBGR/ABGR in memory → BGRA/BGRx for GStreamer */
              d[0] = p[0]; /* B */
              d[1] = p[1]; /* G */
              d[2] = p[2]; /* R */
              d[3] = p[3];
            }
          else
            {
              /* XRGB/ARGB little-endian: bytes are B,G,R,X already */
              d[0] = p[0];
              d[1] = p[1];
              d[2] = p[2];
              d[3] = p[3];
            }
        }
    }

  self->frame_cb (self->user_data,
                  packed,
                  width,
                  height,
                  out_stride,
                  has_alpha);
}

static gboolean
capture_one_frame (KasasaHyprlandStream *self)
{
  stream_clear_frame (self);

  self->frame = hyprland_toplevel_export_manager_v1_capture_toplevel (
    self->export_manager, 0, self->handle);
  if (self->frame == NULL)
    return FALSE;

  hyprland_toplevel_export_frame_v1_add_listener (self->frame,
                                                  &frame_listener,
                                                  self);

  while (!self->stop_requested && !self->buffer_done && !self->frame_failed)
    {
      if (!dispatch_pending (self))
        return FALSE;
      if (self->buffer_done || self->frame_failed)
        break;
      if (!wait_events (self))
        return FALSE;
    }

  if (self->stop_requested || self->frame_failed || !self->buffer_info_ready)
    return !self->stop_requested ? FALSE : TRUE;

  if (!create_shm_buffer (self))
    return FALSE;

  /* ignore_damage=1 so we get a continuous stream even without damage events. */
  hyprland_toplevel_export_frame_v1_copy (self->frame, self->buffer, 1);

  while (!self->stop_requested && !self->frame_ready && !self->frame_failed)
    {
      if (!dispatch_pending (self))
        return FALSE;
      if (self->frame_ready || self->frame_failed)
        break;
      if (!wait_events (self))
        return FALSE;
    }

  if (self->stop_requested)
    return TRUE;
  if (self->frame_failed || !self->frame_ready)
    return FALSE;

  emit_frame (self);
  stream_clear_frame (self);
  return TRUE;
}

static gpointer
stream_thread_func (gpointer data)
{
  KasasaHyprlandStream *self = data;

  self->display = wl_display_connect (NULL);
  if (self->display == NULL)
    {
      g_warning ("Failed to connect to Wayland display for Hyprland stream");
      return NULL;
    }

  self->registry = wl_display_get_registry (self->display);
  wl_registry_add_listener (self->registry, &registry_listener, self);
  wl_display_roundtrip (self->display);

  if (self->shm == NULL || self->export_manager == NULL)
    {
      g_warning ("Hyprland toplevel export protocol is unavailable");
      goto out;
    }

  while (TRUE)
    {
      g_mutex_lock (&self->lock);
      if (self->stop_requested)
        {
          g_mutex_unlock (&self->lock);
          break;
        }
      g_mutex_unlock (&self->lock);

      if (!capture_one_frame (self))
        {
          if (!self->stop_requested)
            {
              g_debug ("Hyprland frame capture failed; retrying");
              g_usleep (50 * 1000);
            }
        }
      else
        {
          /* Cap roughly to 30 FPS when ignore_damage forces full frames. */
          g_usleep (33 * 1000);
        }
    }

out:
  stream_clear_frame (self);
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

  return NULL;
}

KasasaHyprlandStream *
kasasa_hyprland_stream_start (guint32                       handle,
                              KasasaHyprlandStreamFrameFunc  frame_cb,
                              gpointer                      user_data,
                              GDestroyNotify                user_data_destroy,
                              GError                      **error)
{
  KasasaHyprlandStream *self;

  g_return_val_if_fail (frame_cb != NULL, NULL);

  if (!kasasa_hyprland_stream_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland live capture requires Wayland and hyprctl"));
      return NULL;
    }

  self = g_new0 (KasasaHyprlandStream, 1);
  g_mutex_init (&self->lock);
  self->handle = handle;
  self->frame_cb = frame_cb;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;
  self->buffer_fd = -1;

  self->thread = g_thread_new ("kasasa-hypr-stream", stream_thread_func, self);
  if (self->thread == NULL)
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Failed to start Hyprland capture thread"));
      g_mutex_clear (&self->lock);
      g_free (self);
      return NULL;
    }

  self->started = TRUE;
  return self;
}

void
kasasa_hyprland_stream_stop (KasasaHyprlandStream *self)
{
  if (self == NULL)
    return;

  g_mutex_lock (&self->lock);
  self->stop_requested = TRUE;
  g_mutex_unlock (&self->lock);

  if (self->thread != NULL)
    {
      g_thread_join (self->thread);
      self->thread = NULL;
    }

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  g_mutex_clear (&self->lock);
  g_free (self);
}
