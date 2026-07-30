/* kasasa-hyprland-stream.c
 *
 * Continuous native capture of Hyprland windows and outputs.
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

#include <drm_fourcc.h>
#include <gbm.h>
#include <glib/gi18n.h>
#include <wayland-client.h>
#include <xf86drm.h>

#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "hyprland-toplevel-export-v1-client.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-window-query.h"
#include "linux-dmabuf-v1-client.h"

#define FRAME_STAGE_TIMEOUT_USEC          (G_TIME_SPAN_SECOND)
#define EVENT_POLL_INTERVAL_MSEC          100
#define MAX_CONSECUTIVE_FRAME_FAILURES    3
#define DMABUF_POOL_SIZE                  3

typedef enum
{
  KASASA_HYPRLAND_STREAM_SOURCE_WINDOW,
  KASASA_HYPRLAND_STREAM_SOURCE_OUTPUT,
} KasasaHyprlandStreamSource;

typedef struct
{
  uint32_t global_name;
  struct wl_output *output;
  gchar *name;
} KasasaWaylandOutput;

typedef struct
{
  uint32_t format;
  uint32_t padding;
  uint64_t modifier;
} KasasaDmabufFormat;

typedef struct
{
  struct gbm_bo *bo;
  struct wl_buffer *buffer;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t offset;
  uint64_t modifier;
} KasasaDmabufBuffer;

typedef struct
{
  grefcount ref_count;
  GMutex mutex;
  GCond available;
  gboolean stopped;
  gboolean busy[DMABUF_POOL_SIZE];
} KasasaDmabufLeasePool;

typedef struct
{
  KasasaDmabufLeasePool *pool;
  guint slot;
  gint fd;
} KasasaDmabufLease;

typedef enum
{
  ENSURE_DMABUF_FAILED,
  ENSURE_DMABUF_READY,
  ENSURE_DMABUF_BUSY,
} EnsureDmabufResult;

struct _KasasaHyprlandStream
{
  GThread *thread;
  gint stop_requested;

  KasasaHyprlandStreamSource source;
  guint32 handle;
  gchar *output_name;
  guint frame_rate;

  KasasaHyprlandStreamFrameFunc frame_cb;
  KasasaHyprlandStreamDmabufFrameFunc dmabuf_frame_cb;
  KasasaHyprlandStreamErrorFunc error_cb;
  gpointer user_data;
  GDestroyNotify user_data_destroy;

  /* Wayland objects owned by the worker thread */
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct zwp_linux_dmabuf_v1 *linux_dmabuf;
  struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback;
  struct hyprland_toplevel_export_manager_v1 *export_manager;
  struct ext_output_image_capture_source_manager_v1 *output_source_manager;
  struct ext_image_copy_capture_manager_v1 *capture_manager;
  struct ext_image_capture_source_v1 *capture_source;
  struct ext_image_copy_capture_session_v1 *capture_session;
  GPtrArray *outputs;

  /* DMA-BUF allocator state owned by the worker thread */
  GArray *dmabuf_format_table;
  GArray *dmabuf_formats;
  GArray *dmabuf_tranche_formats;
  dev_t dmabuf_main_device;
  dev_t dmabuf_tranche_device;
  gboolean dmabuf_main_device_valid;
  gboolean dmabuf_tranche_device_valid;
  gboolean dmabuf_feedback_done;
  gint dmabuf_disabled;
  int drm_fd;
  struct gbm_device *gbm_device;
  struct gbm_bo *gbm_bo;
  KasasaDmabufBuffer dmabuf_buffers[DMABUF_POOL_SIZE];
  KasasaDmabufLeasePool *dmabuf_lease_pool;
  gint dmabuf_slot;

  /* Per-frame state */
  struct hyprland_toplevel_export_frame_v1 *frame;
  struct ext_image_copy_capture_frame_v1 *output_frame;
  struct wl_buffer *buffer;
  void *buffer_data;
  size_t buffer_size;
  int buffer_fd;
  uint32_t buffer_format;
  uint32_t buffer_width;
  uint32_t buffer_height;
  uint32_t buffer_stride;
  uint32_t buffer_offset;
  uint64_t buffer_modifier;
  uint32_t allocated_buffer_format;
  uint32_t allocated_buffer_width;
  uint32_t allocated_buffer_height;
  uint32_t allocated_buffer_stride;
  gboolean buffer_format_valid;
  gboolean buffer_info_ready;
  gboolean buffer_is_dmabuf;
  gboolean dmabuf_info_ready;
  uint32_t dmabuf_format;
  uint32_t dmabuf_width;
  uint32_t dmabuf_height;
  gboolean buffer_done;
  gboolean frame_failed;
  gboolean frame_ready;
  gboolean constraints_done;
  gboolean capture_stopped;
  gboolean has_emitted_frame;
  uint32_t failure_reason;
  uint32_t frame_flags;
  uint32_t frame_transform;
};

static void
wayland_output_free (KasasaWaylandOutput *output)
{
  if (output == NULL)
    return;

  if (output->output != NULL)
    wl_output_destroy (output->output);
  g_free (output->name);
  g_free (output);
}

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

static KasasaDmabufLeasePool *
dmabuf_lease_pool_new (void)
{
  KasasaDmabufLeasePool *pool = g_new0 (KasasaDmabufLeasePool, 1);

  g_ref_count_init (&pool->ref_count);
  g_mutex_init (&pool->mutex);
  g_cond_init (&pool->available);
  return pool;
}

static KasasaDmabufLeasePool *
dmabuf_lease_pool_ref (KasasaDmabufLeasePool *pool)
{
  g_ref_count_inc (&pool->ref_count);
  return pool;
}

static void
dmabuf_lease_pool_unref (KasasaDmabufLeasePool *pool)
{
  if (pool == NULL || !g_ref_count_dec (&pool->ref_count))
    return;

  g_cond_clear (&pool->available);
  g_mutex_clear (&pool->mutex);
  g_free (pool);
}

static void
dmabuf_lease_pool_stop (KasasaDmabufLeasePool *pool)
{
  if (pool == NULL)
    return;

  g_mutex_lock (&pool->mutex);
  pool->stopped = TRUE;
  g_cond_broadcast (&pool->available);
  g_mutex_unlock (&pool->mutex);
}

static gint
dmabuf_lease_pool_acquire (KasasaDmabufLeasePool *pool,
                           guint                   frame_rate)
{
  gint64 deadline;
  guint i;

  if (pool == NULL)
    return -1;

  deadline = g_get_monotonic_time ()
             + MAX (G_TIME_SPAN_MILLISECOND,
                    G_TIME_SPAN_SECOND / MAX (frame_rate, 1U));
  g_mutex_lock (&pool->mutex);
  while (!pool->stopped)
    {
      for (i = 0; i < DMABUF_POOL_SIZE; i++)
        {
          if (!pool->busy[i])
            {
              pool->busy[i] = TRUE;
              g_mutex_unlock (&pool->mutex);
              return (gint) i;
            }
        }

      if (!g_cond_wait_until (&pool->available, &pool->mutex, deadline))
        break;
    }
  g_mutex_unlock (&pool->mutex);
  return -1;
}

static void
dmabuf_lease_pool_release_slot (KasasaDmabufLeasePool *pool,
                                guint                   slot)
{
  if (pool == NULL || slot >= DMABUF_POOL_SIZE)
    return;

  g_mutex_lock (&pool->mutex);
  if (!pool->stopped)
    {
      pool->busy[slot] = FALSE;
      g_cond_signal (&pool->available);
    }
  g_mutex_unlock (&pool->mutex);
}

static void
dmabuf_lease_release (gpointer data)
{
  KasasaDmabufLease *lease = data;

  if (lease == NULL)
    return;

  if (lease->fd >= 0)
    close (lease->fd);
  dmabuf_lease_pool_release_slot (lease->pool, lease->slot);
  dmabuf_lease_pool_unref (lease->pool);
  g_free (lease);
}

static void
dmabuf_buffer_clear (KasasaDmabufBuffer *buffer)
{
  if (buffer->buffer != NULL)
    wl_buffer_destroy (buffer->buffer);
  if (buffer->bo != NULL)
    gbm_bo_destroy (buffer->bo);
  memset (buffer, 0, sizeof (*buffer));
  buffer->modifier = DRM_FORMAT_MOD_INVALID;
}

static void
stream_clear_dmabuf_buffers (KasasaHyprlandStream *self)
{
  guint i;

  for (i = 0; i < DMABUF_POOL_SIZE; i++)
    dmabuf_buffer_clear (&self->dmabuf_buffers[i]);
}

static void
stream_clear_buffer (KasasaHyprlandStream *self)
{
  if (self->buffer_is_dmabuf)
    {
      self->buffer = NULL;
      self->gbm_bo = NULL;
      if (self->dmabuf_slot >= 0)
        dmabuf_lease_pool_release_slot (self->dmabuf_lease_pool,
                                        (guint) self->dmabuf_slot);
      self->dmabuf_slot = -1;
    }
  else if (self->buffer != NULL)
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
  self->buffer_offset = 0;
  self->buffer_modifier = DRM_FORMAT_MOD_INVALID;
  self->buffer_is_dmabuf = FALSE;
}

static void
stream_clear_frame (KasasaHyprlandStream *self)
{
  if (self->frame != NULL)
    {
      hyprland_toplevel_export_frame_v1_destroy (self->frame);
      self->frame = NULL;
    }
  if (self->output_frame != NULL)
    {
      ext_image_copy_capture_frame_v1_destroy (self->output_frame);
      self->output_frame = NULL;
    }
  if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_WINDOW)
    {
      self->buffer_info_ready = FALSE;
      self->buffer_done = FALSE;
      self->buffer_format = 0;
      self->buffer_format_valid = FALSE;
      self->buffer_width = 0;
      self->buffer_height = 0;
      self->buffer_stride = 0;
      self->dmabuf_info_ready = FALSE;
      self->dmabuf_format = 0;
      self->dmabuf_width = 0;
      self->dmabuf_height = 0;
    }
  self->frame_failed = FALSE;
  self->frame_ready = FALSE;
  self->failure_reason = 0;
  self->frame_flags = 0;
  self->frame_transform = WL_OUTPUT_TRANSFORM_NORMAL;
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

static gboolean
drm_device_ids_equal (dev_t first,
                      dev_t second)
{
  drmDevicePtr first_device = NULL;
  drmDevicePtr second_device = NULL;
  gboolean equal = FALSE;

  if (first == second)
    return TRUE;

  if (drmGetDeviceFromDevId (first, 0, &first_device) == 0
      && drmGetDeviceFromDevId (second, 0, &second_device) == 0)
    equal = drmDevicesEqual (first_device, second_device) == 1;

  if (first_device != NULL)
    drmFreeDevice (&first_device);
  if (second_device != NULL)
    drmFreeDevice (&second_device);
  return equal;
}

static gboolean
setup_dmabuf_allocator (KasasaHyprlandStream *self)
{
  drmDevicePtr device = NULL;
  const gchar *node = NULL;

  if (self->gbm_device != NULL)
    return TRUE;
  if (!self->dmabuf_main_device_valid)
    return FALSE;
  if (drmGetDeviceFromDevId (self->dmabuf_main_device, 0, &device) != 0)
    return FALSE;

  if ((device->available_nodes & (1 << DRM_NODE_RENDER)) != 0)
    node = device->nodes[DRM_NODE_RENDER];
  else if ((device->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0)
    node = device->nodes[DRM_NODE_PRIMARY];

  if (node != NULL)
    self->drm_fd = open (node, O_RDWR | O_CLOEXEC);
  if (self->drm_fd >= 0)
    self->gbm_device = gbm_create_device (self->drm_fd);

  if (self->gbm_device != NULL)
    g_info ("Hyprland DMA-BUF allocator uses %s (%s)",
            node,
            gbm_device_get_backend_name (self->gbm_device));
  else if (self->drm_fd >= 0)
    {
      close (self->drm_fd);
      self->drm_fd = -1;
    }

  drmFreeDevice (&device);
  return self->gbm_device != NULL;
}

static gboolean
dmabuf_modifier_exists (GArray   *modifiers,
                        uint64_t  modifier)
{
  guint i;

  for (i = 0; i < modifiers->len; i++)
    {
      if (g_array_index (modifiers, uint64_t, i) == modifier)
        return TRUE;
    }
  return FALSE;
}

static void
stream_use_dmabuf_buffer (KasasaHyprlandStream *self,
                          guint                   slot_index)
{
  KasasaDmabufBuffer *slot = &self->dmabuf_buffers[slot_index];

  self->dmabuf_slot = (gint) slot_index;
  self->gbm_bo = slot->bo;
  self->buffer = slot->buffer;
  self->buffer_is_dmabuf = TRUE;
  self->buffer_format = slot->format;
  self->buffer_width = slot->width;
  self->buffer_height = slot->height;
  self->buffer_stride = slot->stride;
  self->buffer_offset = slot->offset;
  self->buffer_modifier = slot->modifier;
}

static EnsureDmabufResult
ensure_dmabuf_buffer (KasasaHyprlandStream *self)
{
  struct zwp_linux_buffer_params_v1 *params;
  KasasaDmabufBuffer *slot;
  g_autoptr (GArray) modifiers = NULL;
  gboolean implicit_supported = FALSE;
  gboolean used_explicit_modifiers = FALSE;
  uint64_t wl_modifier;
  uint64_t actual_modifier;
  uint32_t stride;
  uint32_t offset;
  int fd;
  gint slot_index;
  guint i;

  if (self->dmabuf_frame_cb == NULL
      || g_atomic_int_get (&self->dmabuf_disabled)
      || self->linux_dmabuf == NULL || !self->dmabuf_info_ready
      || self->gbm_device == NULL || self->dmabuf_formats == NULL)
    return ENSURE_DMABUF_FAILED;

  modifiers = g_array_new (FALSE, FALSE, sizeof (uint64_t));
  for (i = 0; i < self->dmabuf_formats->len; i++)
    {
      KasasaDmabufFormat entry =
        g_array_index (self->dmabuf_formats, KasasaDmabufFormat, i);

      if (entry.format != self->dmabuf_format)
        continue;
      if (entry.modifier == DRM_FORMAT_MOD_INVALID)
        implicit_supported = TRUE;
      else if (!dmabuf_modifier_exists (modifiers, entry.modifier))
        g_array_append_val (modifiers, entry.modifier);
    }

  stream_clear_buffer (self);
  slot_index = dmabuf_lease_pool_acquire (self->dmabuf_lease_pool,
                                           self->frame_rate);
  if (slot_index < 0)
    return ENSURE_DMABUF_BUSY;

  slot = &self->dmabuf_buffers[slot_index];
  if (slot->bo != NULL
      && slot->buffer != NULL
      && slot->format == self->dmabuf_format
      && slot->width == self->dmabuf_width
      && slot->height == self->dmabuf_height)
    {
      stream_use_dmabuf_buffer (self, (guint) slot_index);
      return ENSURE_DMABUF_READY;
    }

  dmabuf_buffer_clear (slot);
  if (modifiers->len > 0)
    {
      slot->bo = gbm_bo_create_with_modifiers2 (
        self->gbm_device,
        self->dmabuf_width,
        self->dmabuf_height,
        self->dmabuf_format,
        (const uint64_t *) modifiers->data,
        modifiers->len,
        GBM_BO_USE_RENDERING);
      used_explicit_modifiers = slot->bo != NULL;
    }
  if (slot->bo == NULL && implicit_supported)
    slot->bo = gbm_bo_create (self->gbm_device,
                              self->dmabuf_width,
                              self->dmabuf_height,
                              self->dmabuf_format,
                              GBM_BO_USE_RENDERING);
  if (slot->bo == NULL || gbm_bo_get_plane_count (slot->bo) != 1)
    goto fail;

  actual_modifier = gbm_bo_get_modifier (slot->bo);
  wl_modifier = used_explicit_modifiers
                ? actual_modifier
                : DRM_FORMAT_MOD_INVALID;
  stride = gbm_bo_get_stride_for_plane (slot->bo, 0);
  offset = gbm_bo_get_offset (slot->bo, 0);
  fd = gbm_bo_get_fd_for_plane (slot->bo, 0);
  if (fd < 0 || stride == 0)
    {
      if (fd >= 0)
        close (fd);
      goto fail;
    }

  params = zwp_linux_dmabuf_v1_create_params (self->linux_dmabuf);
  if (params == NULL)
    {
      close (fd);
      goto fail;
    }
  zwp_linux_buffer_params_v1_add (params,
                                  fd,
                                  0,
                                  offset,
                                  stride,
                                  (uint32_t) (wl_modifier >> 32),
                                  (uint32_t) wl_modifier);
  close (fd);
  slot->buffer = zwp_linux_buffer_params_v1_create_immed (
    params,
    (int32_t) self->dmabuf_width,
    (int32_t) self->dmabuf_height,
    self->dmabuf_format,
    0);
  zwp_linux_buffer_params_v1_destroy (params);
  if (slot->buffer == NULL)
    goto fail;

  slot->format = self->dmabuf_format;
  slot->width = self->dmabuf_width;
  slot->height = self->dmabuf_height;
  slot->stride = stride;
  slot->offset = offset;
  slot->modifier = actual_modifier;
  g_debug ("Allocated Hyprland DMA-BUF pool slot %d (%ux%u, modifier 0x%016" G_GINT64_MODIFIER "x)",
           slot_index,
           slot->width,
           slot->height,
           slot->modifier);
  stream_use_dmabuf_buffer (self, (guint) slot_index);
  return ENSURE_DMABUF_READY;

fail:
  dmabuf_buffer_clear (slot);
  dmabuf_lease_pool_release_slot (self->dmabuf_lease_pool,
                                  (guint) slot_index);
  return ENSURE_DMABUF_FAILED;
}

static gboolean
stream_format_supported (uint32_t format)
{
  return format == WL_SHM_FORMAT_XRGB8888
         || format == WL_SHM_FORMAT_ARGB8888
         || format == WL_SHM_FORMAT_XBGR8888
         || format == WL_SHM_FORMAT_ABGR8888;
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
  if (!stream_format_supported (format))
    {
      g_debug ("Ignoring unsupported shm format 0x%x", format);
      return;
    }

  self->buffer_format = format;
  self->buffer_width = width;
  self->buffer_height = height;
  self->buffer_stride = stride;
  self->buffer_format_valid = TRUE;
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
  KasasaHyprlandStream *self = data;

  (void) frame;

  if (self->dmabuf_frame_cb == NULL
      || g_atomic_int_get (&self->dmabuf_disabled)
      || width == 0 || height == 0)
    return;

  self->dmabuf_format = format;
  self->dmabuf_width = width;
  self->dmabuf_height = height;
  self->dmabuf_info_ready = TRUE;
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
output_constraints_begin (KasasaHyprlandStream *self)
{
  if (!self->constraints_done)
    return;

  self->constraints_done = FALSE;
  self->buffer_info_ready = FALSE;
  self->buffer_format = 0;
  self->buffer_format_valid = FALSE;
  self->buffer_width = 0;
  self->buffer_height = 0;
  self->buffer_stride = 0;
}

static void
output_session_handle_buffer_size (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session,
  uint32_t                                  width,
  uint32_t                                  height)
{
  KasasaHyprlandStream *self = data;

  (void) session;
  output_constraints_begin (self);
  self->buffer_width = width;
  self->buffer_height = height;
  self->buffer_stride = width <= G_MAXUINT32 / 4 ? width * 4 : 0;
}

static void
output_session_handle_shm_format (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session,
  uint32_t                                  format)
{
  KasasaHyprlandStream *self = data;

  (void) session;
  output_constraints_begin (self);
  if (!self->buffer_format_valid && stream_format_supported (format))
    {
      self->buffer_format = format;
      self->buffer_format_valid = TRUE;
    }
}

static void
output_session_handle_dmabuf_device (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session,
  struct wl_array                          *device)
{
  (void) data;
  (void) session;
  (void) device;
}

static void
output_session_handle_dmabuf_format (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session,
  uint32_t                                  format,
  struct wl_array                          *modifiers)
{
  (void) data;
  (void) session;
  (void) format;
  (void) modifiers;
}

static void
output_session_handle_done (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session)
{
  KasasaHyprlandStream *self = data;

  (void) session;
  self->constraints_done = TRUE;
  self->buffer_info_ready = self->buffer_format_valid
                            && self->buffer_width > 0
                            && self->buffer_height > 0
                            && self->buffer_stride > 0;
}

static void
output_session_handle_stopped (
  void                                     *data,
  struct ext_image_copy_capture_session_v1 *session)
{
  KasasaHyprlandStream *self = data;

  (void) session;
  self->capture_stopped = TRUE;
}

static const struct ext_image_copy_capture_session_v1_listener
output_session_listener = {
  .buffer_size = output_session_handle_buffer_size,
  .shm_format = output_session_handle_shm_format,
  .dmabuf_device = output_session_handle_dmabuf_device,
  .dmabuf_format = output_session_handle_dmabuf_format,
  .done = output_session_handle_done,
  .stopped = output_session_handle_stopped,
};

static void
output_frame_handle_transform (
  void                                   *data,
  struct ext_image_copy_capture_frame_v1 *frame,
  uint32_t                                transform)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->frame_transform = transform <= WL_OUTPUT_TRANSFORM_FLIPPED_270
                          ? transform
                          : WL_OUTPUT_TRANSFORM_NORMAL;
}

static void
output_frame_handle_damage (
  void                                   *data,
  struct ext_image_copy_capture_frame_v1 *frame,
  int32_t                                 x,
  int32_t                                 y,
  int32_t                                 width,
  int32_t                                 height)
{
  (void) data;
  (void) frame;
  (void) x;
  (void) y;
  (void) width;
  (void) height;
}

static void
output_frame_handle_presentation_time (
  void                                   *data,
  struct ext_image_copy_capture_frame_v1 *frame,
  uint32_t                                tv_sec_hi,
  uint32_t                                tv_sec_lo,
  uint32_t                                tv_nsec)
{
  (void) data;
  (void) frame;
  (void) tv_sec_hi;
  (void) tv_sec_lo;
  (void) tv_nsec;
}

static void
output_frame_handle_ready (
  void                                   *data,
  struct ext_image_copy_capture_frame_v1 *frame)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->frame_ready = TRUE;
}

static void
output_frame_handle_failed (
  void                                   *data,
  struct ext_image_copy_capture_frame_v1 *frame,
  uint32_t                                reason)
{
  KasasaHyprlandStream *self = data;

  (void) frame;
  self->failure_reason = reason;
  self->frame_failed = TRUE;
}

static const struct ext_image_copy_capture_frame_v1_listener
output_frame_listener = {
  .transform = output_frame_handle_transform,
  .damage = output_frame_handle_damage,
  .presentation_time = output_frame_handle_presentation_time,
  .ready = output_frame_handle_ready,
  .failed = output_frame_handle_failed,
};

static void
wayland_output_handle_geometry (void             *data,
                                struct wl_output *output,
                                int32_t           x,
                                int32_t           y,
                                int32_t           physical_width,
                                int32_t           physical_height,
                                int32_t           subpixel,
                                const char       *make,
                                const char       *model,
                                int32_t           transform)
{
  (void) data;
  (void) output;
  (void) x;
  (void) y;
  (void) physical_width;
  (void) physical_height;
  (void) subpixel;
  (void) make;
  (void) model;
  (void) transform;
}

static void
wayland_output_handle_mode (void             *data,
                            struct wl_output *output,
                            uint32_t          flags,
                            int32_t           width,
                            int32_t           height,
                            int32_t           refresh)
{
  (void) data;
  (void) output;
  (void) flags;
  (void) width;
  (void) height;
  (void) refresh;
}

static void
wayland_output_handle_done (void             *data,
                            struct wl_output *output)
{
  (void) data;
  (void) output;
}

static void
wayland_output_handle_scale (void             *data,
                             struct wl_output *output,
                             int32_t           factor)
{
  (void) data;
  (void) output;
  (void) factor;
}

static void
wayland_output_handle_name (void             *data,
                            struct wl_output *output,
                            const char       *name)
{
  KasasaWaylandOutput *record = data;

  (void) output;
  g_free (record->name);
  record->name = g_strdup (name);
}

static void
wayland_output_handle_description (void             *data,
                                   struct wl_output *output,
                                   const char       *description)
{
  (void) data;
  (void) output;
  (void) description;
}

static const struct wl_output_listener output_listener = {
  .geometry = wayland_output_handle_geometry,
  .mode = wayland_output_handle_mode,
  .done = wayland_output_handle_done,
  .scale = wayland_output_handle_scale,
  .name = wayland_output_handle_name,
  .description = wayland_output_handle_description,
};

static gboolean
device_from_array (struct wl_array *array,
                   dev_t           *device)
{
  if (array == NULL || device == NULL || array->size != sizeof (*device))
    return FALSE;

  memcpy (device, array->data, sizeof (*device));
  return TRUE;
}

static void
dmabuf_feedback_handle_done (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback)
{
  KasasaHyprlandStream *self = data;

  (void) feedback;
  self->dmabuf_feedback_done = TRUE;
}

static void
dmabuf_feedback_handle_format_table (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback,
  int32_t                                fd,
  uint32_t                               size)
{
  KasasaHyprlandStream *self = data;
  void *mapping;

  (void) feedback;
  g_array_set_size (self->dmabuf_format_table, 0);
  g_array_set_size (self->dmabuf_formats, 0);
  g_array_set_size (self->dmabuf_tranche_formats, 0);

  if (fd < 0 || size == 0 || size % sizeof (KasasaDmabufFormat) != 0)
    {
      if (fd >= 0)
        close (fd);
      return;
    }

  mapping = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close (fd);
  if (mapping == MAP_FAILED)
    return;

  g_array_append_vals (self->dmabuf_format_table,
                       mapping,
                       size / sizeof (KasasaDmabufFormat));
  munmap (mapping, size);
}

static void
dmabuf_feedback_handle_main_device (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback,
  struct wl_array                       *device)
{
  KasasaHyprlandStream *self = data;

  (void) feedback;
  self->dmabuf_main_device_valid =
    device_from_array (device, &self->dmabuf_main_device);
}

static void
dmabuf_feedback_handle_tranche_done (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback)
{
  KasasaHyprlandStream *self = data;

  (void) feedback;
  if (self->dmabuf_main_device_valid
      && self->dmabuf_tranche_device_valid
      && drm_device_ids_equal (self->dmabuf_main_device,
                               self->dmabuf_tranche_device))
    g_array_append_vals (self->dmabuf_formats,
                         self->dmabuf_tranche_formats->data,
                         self->dmabuf_tranche_formats->len);

  self->dmabuf_tranche_device_valid = FALSE;
  g_array_set_size (self->dmabuf_tranche_formats, 0);
}

static void
dmabuf_feedback_handle_tranche_target_device (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback,
  struct wl_array                       *device)
{
  KasasaHyprlandStream *self = data;

  (void) feedback;
  self->dmabuf_tranche_device_valid =
    device_from_array (device, &self->dmabuf_tranche_device);
}

static void
dmabuf_feedback_handle_tranche_formats (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback,
  struct wl_array                       *indices)
{
  KasasaHyprlandStream *self = data;
  uint16_t *index;

  (void) feedback;
  if (indices == NULL || indices->size % sizeof (*index) != 0)
    return;

  wl_array_for_each (index, indices)
    {
      KasasaDmabufFormat entry;

      if (*index >= self->dmabuf_format_table->len)
        continue;
      entry = g_array_index (self->dmabuf_format_table,
                             KasasaDmabufFormat,
                             *index);
      g_array_append_val (self->dmabuf_tranche_formats, entry);
    }
}

static void
dmabuf_feedback_handle_tranche_flags (
  void                                  *data,
  struct zwp_linux_dmabuf_feedback_v1  *feedback,
  uint32_t                               flags)
{
  (void) data;
  (void) feedback;
  (void) flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener
dmabuf_feedback_listener = {
  .done = dmabuf_feedback_handle_done,
  .format_table = dmabuf_feedback_handle_format_table,
  .main_device = dmabuf_feedback_handle_main_device,
  .tranche_done = dmabuf_feedback_handle_tranche_done,
  .tranche_target_device = dmabuf_feedback_handle_tranche_target_device,
  .tranche_formats = dmabuf_feedback_handle_tranche_formats,
  .tranche_flags = dmabuf_feedback_handle_tranche_flags,
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
  else if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_WINDOW
           && self->dmabuf_frame_cb != NULL
           && version >= 4
           && g_strcmp0 (interface,
                         zwp_linux_dmabuf_v1_interface.name) == 0)
    {
      self->linux_dmabuf =
        wl_registry_bind (registry,
                          name,
                          &zwp_linux_dmabuf_v1_interface,
                          MIN (version, 5));
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
  else if (g_strcmp0 (interface,
                      ext_output_image_capture_source_manager_v1_interface.name) == 0)
    {
      self->output_source_manager =
        wl_registry_bind (registry,
                          name,
                          &ext_output_image_capture_source_manager_v1_interface,
                          1);
    }
  else if (g_strcmp0 (interface,
                      ext_image_copy_capture_manager_v1_interface.name) == 0)
    {
      self->capture_manager =
        wl_registry_bind (registry,
                          name,
                          &ext_image_copy_capture_manager_v1_interface,
                          1);
    }
  else if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_OUTPUT
           && g_strcmp0 (interface, wl_output_interface.name) == 0)
    {
      KasasaWaylandOutput *output = g_new0 (KasasaWaylandOutput, 1);

      output->global_name = name;
      output->output = wl_registry_bind (registry,
                                         name,
                                         &wl_output_interface,
                                         MIN (version, 4));
      wl_output_add_listener (output->output, &output_listener, output);
      g_ptr_array_add (self->outputs, output);
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

  if (self->buffer_is_dmabuf)
    {
      KasasaDmabufLease *lease;
      int fd;
      guint slot;
      uint32_t offset = self->buffer_offset;
      uint32_t fourcc = self->buffer_format;
      uint64_t modifier = self->buffer_modifier;

      if (self->gbm_bo == NULL || self->dmabuf_frame_cb == NULL
          || self->dmabuf_slot < 0
          || width <= 0 || height <= 0 || stride <= 0)
        return;

      slot = (guint) self->dmabuf_slot;
      fd = gbm_bo_get_fd_for_plane (self->gbm_bo, 0);
      if (fd < 0)
        {
          g_atomic_int_set (&self->dmabuf_disabled, TRUE);
          stream_clear_buffer (self);
          return;
        }

      lease = g_new0 (KasasaDmabufLease, 1);
      lease->pool = dmabuf_lease_pool_ref (self->dmabuf_lease_pool);
      lease->slot = slot;
      lease->fd = fd;

      /* The lease now owns the pool slot. Keep the reusable wl_buffer/BO in
       * dmabuf_buffers, but detach them from the per-frame state. */
      self->buffer = NULL;
      self->gbm_bo = NULL;
      self->dmabuf_slot = -1;
      self->buffer_is_dmabuf = FALSE;
      self->buffer_offset = 0;
      self->buffer_modifier = DRM_FORMAT_MOD_INVALID;

      self->dmabuf_frame_cb (self->user_data,
                             fd,
                             width,
                             height,
                             stride,
                             offset,
                             fourcc,
                             modifier,
                             y_invert,
                             self->frame_transform,
                             dmabuf_lease_release,
                             lease);
      return;
    }

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
                  y_invert,
                  self->frame_transform);
}

static gboolean
capture_one_window_frame (KasasaHyprlandStream  *self,
                          gboolean              *retryable,
                          GError               **error)
{
  EnsureDmabufResult dmabuf_result = ENSURE_DMABUF_FAILED;
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
  if (!self->buffer_info_ready && !self->dmabuf_info_ready)
    return set_stream_error_literal (error,
                                     _("The compositor provided no supported frame format"));

  if (self->dmabuf_info_ready
      && !g_atomic_int_get (&self->dmabuf_disabled))
    {
      dmabuf_result = ensure_dmabuf_buffer (self);
      if (dmabuf_result == ENSURE_DMABUF_BUSY)
        {
          /* GTK still owns all three capture buffers. Drop this capture
           * request instead of allocating an unbounded fourth buffer. */
          stream_clear_frame (self);
          return TRUE;
        }
      if (dmabuf_result == ENSURE_DMABUF_FAILED)
        {
          g_atomic_int_set (&self->dmabuf_disabled, TRUE);
          g_info ("DMA-BUF allocation failed; falling back to wl_shm");
        }
    }

  if (!self->buffer_is_dmabuf && !ensure_shm_buffer (self))
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
      if (self->buffer_is_dmabuf)
        {
          g_atomic_int_set (&self->dmabuf_disabled, TRUE);
          g_info ("DMA-BUF capture was rejected; falling back to wl_shm");
          stream_clear_buffer (self);
        }
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

static KasasaWaylandOutput *
find_wayland_output (KasasaHyprlandStream *self)
{
  guint i;

  for (i = 0; i < self->outputs->len; i++)
    {
      KasasaWaylandOutput *output = g_ptr_array_index (self->outputs, i);

      if (g_strcmp0 (output->name, self->output_name) == 0)
        return output;
    }

  return NULL;
}

static gboolean
setup_output_capture (KasasaHyprlandStream  *self,
                      GError               **error)
{
  KasasaWaylandOutput *output = find_wayland_output (self);

  if (output == NULL)
    {
      g_set_error (error,
                   KASASA_WINDOW_QUERY_ERROR,
                   KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
                   _("Wayland output “%s” is unavailable"),
                   self->output_name);
      return FALSE;
    }

  self->capture_source =
    ext_output_image_capture_source_manager_v1_create_source (
      self->output_source_manager,
      output->output);
  if (self->capture_source == NULL)
    return set_stream_error_literal (error,
                                     _("Couldn't create an output capture source"));

  self->capture_session =
    ext_image_copy_capture_manager_v1_create_session (self->capture_manager,
                                                       self->capture_source,
                                                       EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS);
  if (self->capture_session == NULL)
    return set_stream_error_literal (error,
                                     _("Couldn't create an output capture session"));

  ext_image_copy_capture_session_v1_add_listener (self->capture_session,
                                                   &output_session_listener,
                                                   self);
  if (!roundtrip_with_timeout (self, error))
    return FALSE;

  if (self->capture_stopped)
    return set_stream_error_literal (error,
                                     _("The selected monitor is no longer available"));
  if (!self->constraints_done || !self->buffer_info_ready)
    return set_stream_error_literal (error,
                                     _("The compositor provided no supported monitor frame format"));

  return TRUE;
}

static gboolean
wait_for_output_constraints (KasasaHyprlandStream  *self,
                             GError               **error)
{
  gint64 deadline = g_get_monotonic_time () + FRAME_STAGE_TIMEOUT_USEC;

  while (!stream_stop_requested (self)
         && !self->constraints_done
         && !self->capture_stopped)
    {
      if (!dispatch_pending (self, error))
        return FALSE;
      if (self->constraints_done || self->capture_stopped)
        break;
      if (!wait_events (self,
                        deadline,
                        _("Timed out waiting for monitor capture constraints"),
                        error))
        return FALSE;
    }

  if (stream_stop_requested (self))
    return FALSE;
  if (self->capture_stopped)
    return set_stream_error_literal (error,
                                     _("The selected monitor is no longer available"));
  if (!self->constraints_done)
    return set_stream_error_literal (error,
                                     _("Monitor capture buffer constraints are unavailable"));

  return TRUE;
}

static gboolean
capture_one_output_frame (KasasaHyprlandStream  *self,
                          gboolean              *retryable,
                          GError               **error)
{
  gint64 deadline;

  if (retryable != NULL)
    *retryable = FALSE;

  stream_clear_frame (self);
  if (self->capture_stopped)
    return set_stream_error_literal (error,
                                     _("The selected monitor is no longer available"));
  if (!self->constraints_done
      && !wait_for_output_constraints (self, error))
    return FALSE;
  if (!self->buffer_info_ready)
    return set_stream_error_literal (error,
                                     _("Monitor capture buffer constraints are unavailable"));
  if (!ensure_shm_buffer (self))
    return set_stream_error_literal (error,
                                     _("Couldn't allocate a monitor frame buffer"));

  self->output_frame =
    ext_image_copy_capture_session_v1_create_frame (self->capture_session);
  if (self->output_frame == NULL)
    return set_stream_error_literal (error,
                                     _("Couldn't request a monitor frame"));

  ext_image_copy_capture_frame_v1_add_listener (self->output_frame,
                                                 &output_frame_listener,
                                                 self);
  ext_image_copy_capture_frame_v1_attach_buffer (self->output_frame,
                                                  self->buffer);
  ext_image_copy_capture_frame_v1_damage_buffer (
    self->output_frame,
    0,
    0,
    (int32_t) self->buffer_width,
    (int32_t) self->buffer_height);
  ext_image_copy_capture_frame_v1_capture (self->output_frame);

  /* The first frame must arrive immediately. Later frames are damage-driven
   * and may legitimately wait while the output is static. */
  deadline = self->has_emitted_frame
             ? G_MAXINT64
             : g_get_monotonic_time () + FRAME_STAGE_TIMEOUT_USEC;
  while (!stream_stop_requested (self)
         && !self->frame_ready
         && !self->frame_failed
         && !self->capture_stopped)
    {
      if (!dispatch_pending (self, error))
        return FALSE;
      if (self->frame_ready || self->frame_failed || self->capture_stopped)
        break;
      if (!wait_events (self,
                        deadline,
                        _("Timed out waiting for a monitor frame"),
                        error))
        return FALSE;
    }

  if (stream_stop_requested (self))
    return FALSE;
  if (self->capture_stopped
      || self->failure_reason
         == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)
    return set_stream_error_literal (error,
                                     _("The selected monitor is no longer available"));
  if (self->frame_failed)
    {
      if (retryable != NULL)
        *retryable = TRUE;
      if (self->failure_reason
          == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS)
        return set_stream_error_literal (error,
                                         _("Monitor capture constraints changed"));
      return set_stream_error_literal (error,
                                       _("The compositor failed to capture the monitor"));
    }
  if (!self->frame_ready)
    return set_stream_error_literal (error,
                                     _("The compositor did not complete the monitor frame"));

  emit_frame (self);
  self->has_emitted_frame = TRUE;
  stream_clear_frame (self);
  return TRUE;
}

static gboolean
capture_one_frame (KasasaHyprlandStream  *self,
                   gboolean              *retryable,
                   GError               **error)
{
  if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_OUTPUT)
    return capture_one_output_frame (self, retryable, error);

  return capture_one_window_frame (self, retryable, error);
}

static void
wait_for_frame_interval (KasasaHyprlandStream *self,
                         gint64                 frame_started)
{
  const gint64 interval = G_USEC_PER_SEC / (gint64) self->frame_rate;
  const gint64 deadline = frame_started + interval;

  while (!stream_stop_requested (self))
    {
      gint64 remaining = deadline - g_get_monotonic_time ();

      if (remaining <= 0)
        return;

      g_usleep ((gulong) MIN (remaining, 10 * G_TIME_SPAN_MILLISECOND));
    }
}

static gpointer
stream_thread_func (gpointer data)
{
  KasasaHyprlandStream *self = data;
  g_autoptr (GError) stream_error = NULL;
  gboolean retryable;
  guint consecutive_failures = 0;
  gint64 frame_started;

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

  if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_WINDOW
      && self->dmabuf_frame_cb != NULL
      && self->linux_dmabuf != NULL)
    {
      self->dmabuf_feedback =
        zwp_linux_dmabuf_v1_get_default_feedback (self->linux_dmabuf);
      if (self->dmabuf_feedback != NULL)
        {
          zwp_linux_dmabuf_feedback_v1_add_listener (
            self->dmabuf_feedback,
            &dmabuf_feedback_listener,
            self);
          if (!roundtrip_with_timeout (self, &stream_error))
            goto out;
        }

      if (!self->dmabuf_feedback_done
          || self->dmabuf_formats->len == 0
          || !setup_dmabuf_allocator (self))
        {
          g_atomic_int_set (&self->dmabuf_disabled, TRUE);
          g_info ("Hyprland DMA-BUF capture is unavailable; using wl_shm");
        }
    }

  if (self->source == KASASA_HYPRLAND_STREAM_SOURCE_WINDOW)
    {
      if (self->shm == NULL || self->export_manager == NULL)
        {
          set_stream_error_literal (&stream_error,
                                    _("Hyprland's window capture protocol is unavailable"));
          goto out;
        }
    }
  else
    {
      if (self->shm == NULL || self->output_source_manager == NULL
          || self->capture_manager == NULL)
        {
          set_stream_error_literal (&stream_error,
                                    _("Hyprland's monitor capture protocol is unavailable"));
          goto out;
        }

      /* wl_output names are emitted by objects bound while processing the
       * first registry roundtrip, so collect their events in a second one. */
      if (!roundtrip_with_timeout (self, &stream_error)
          || !setup_output_capture (self, &stream_error))
        goto out;
    }

  frame_started = g_get_monotonic_time ();
  if (!capture_one_frame (self, NULL, &stream_error))
    goto out;
  wait_for_frame_interval (self, frame_started);

  while (!stream_stop_requested (self))
    {
      g_clear_error (&stream_error);
      frame_started = g_get_monotonic_time ();
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
          wait_for_frame_interval (self, frame_started);
        }
    }

out:
  dmabuf_lease_pool_stop (self->dmabuf_lease_pool);
  stream_clear_frame (self);
  stream_clear_buffer (self);
  stream_clear_dmabuf_buffers (self);
  if (self->capture_session != NULL)
    {
      ext_image_copy_capture_session_v1_destroy (self->capture_session);
      self->capture_session = NULL;
    }
  if (self->capture_source != NULL)
    {
      ext_image_capture_source_v1_destroy (self->capture_source);
      self->capture_source = NULL;
    }
  if (self->capture_manager != NULL)
    {
      ext_image_copy_capture_manager_v1_destroy (self->capture_manager);
      self->capture_manager = NULL;
    }
  if (self->output_source_manager != NULL)
    {
      ext_output_image_capture_source_manager_v1_destroy (
        self->output_source_manager);
      self->output_source_manager = NULL;
    }
  if (self->export_manager != NULL)
    {
      hyprland_toplevel_export_manager_v1_destroy (self->export_manager);
      self->export_manager = NULL;
    }
  if (self->dmabuf_feedback != NULL)
    {
      zwp_linux_dmabuf_feedback_v1_destroy (self->dmabuf_feedback);
      self->dmabuf_feedback = NULL;
    }
  if (self->linux_dmabuf != NULL)
    {
      zwp_linux_dmabuf_v1_destroy (self->linux_dmabuf);
      self->linux_dmabuf = NULL;
    }
  if (self->shm != NULL)
    {
      wl_shm_destroy (self->shm);
      self->shm = NULL;
    }
  g_clear_pointer (&self->outputs, g_ptr_array_unref);
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
  if (self->gbm_device != NULL)
    {
      gbm_device_destroy (self->gbm_device);
      self->gbm_device = NULL;
    }
  if (self->drm_fd >= 0)
    {
      close (self->drm_fd);
      self->drm_fd = -1;
    }

  if (!stream_stop_requested (self) && self->error_cb != NULL)
    {
      if (stream_error == NULL)
        set_stream_error_literal (&stream_error,
                                  self->source == KASASA_HYPRLAND_STREAM_SOURCE_OUTPUT
                                  ? _("Couldn't start Hyprland monitor capture")
                                  : _("Couldn't start Hyprland window capture"));
      self->error_cb (self->user_data, stream_error);
    }

  return NULL;
}

static KasasaHyprlandStream *
start_stream (KasasaHyprlandStreamSource    source,
              guint32                       handle,
              const gchar                  *output_name,
              guint                         frame_rate,
              KasasaHyprlandStreamFrameFunc frame_cb,
              KasasaHyprlandStreamDmabufFrameFunc dmabuf_frame_cb,
              KasasaHyprlandStreamErrorFunc error_cb,
              gpointer                      user_data,
              GDestroyNotify                user_data_destroy,
              GError                      **error)
{
  KasasaHyprlandStream *self;
  g_autoptr (GError) thread_error = NULL;

  g_return_val_if_fail (frame_cb != NULL, NULL);
  g_return_val_if_fail (frame_rate > 0, NULL);
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
  self->source = source;
  self->handle = handle;
  self->output_name = g_strdup (output_name);
  self->frame_rate = frame_rate;
  self->frame_cb = frame_cb;
  self->dmabuf_frame_cb = dmabuf_frame_cb;
  self->error_cb = error_cb;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;
  self->buffer_fd = -1;
  self->drm_fd = -1;
  self->dmabuf_slot = -1;
  self->buffer_modifier = DRM_FORMAT_MOD_INVALID;
  self->outputs = g_ptr_array_new_with_free_func (
    (GDestroyNotify) wayland_output_free);
  self->dmabuf_format_table =
    g_array_new (FALSE, FALSE, sizeof (KasasaDmabufFormat));
  self->dmabuf_formats =
    g_array_new (FALSE, FALSE, sizeof (KasasaDmabufFormat));
  self->dmabuf_tranche_formats =
    g_array_new (FALSE, FALSE, sizeof (KasasaDmabufFormat));
  if (dmabuf_frame_cb != NULL)
    self->dmabuf_lease_pool = dmabuf_lease_pool_new ();

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

KasasaHyprlandStream *
kasasa_hyprland_stream_start (guint32                       handle,
                              guint                         frame_rate,
                              KasasaHyprlandStreamFrameFunc  frame_cb,
                              KasasaHyprlandStreamErrorFunc  error_cb,
                              gpointer                      user_data,
                              GDestroyNotify                user_data_destroy,
                              GError                      **error)
{
  return start_stream (KASASA_HYPRLAND_STREAM_SOURCE_WINDOW,
                       handle,
                       NULL,
                       frame_rate,
                       frame_cb,
                       NULL,
                       error_cb,
                       user_data,
                       user_data_destroy,
                       error);
}

KasasaHyprlandStream *
kasasa_hyprland_stream_start_dmabuf (
  guint32                              handle,
  guint                                frame_rate,
  KasasaHyprlandStreamDmabufFrameFunc  dmabuf_frame_cb,
  KasasaHyprlandStreamFrameFunc        frame_cb,
  KasasaHyprlandStreamErrorFunc        error_cb,
  gpointer                             user_data,
  GDestroyNotify                       user_data_destroy,
  GError                             **error)
{
  g_return_val_if_fail (dmabuf_frame_cb != NULL, NULL);

  return start_stream (KASASA_HYPRLAND_STREAM_SOURCE_WINDOW,
                       handle,
                       NULL,
                       frame_rate,
                       frame_cb,
                       dmabuf_frame_cb,
                       error_cb,
                       user_data,
                       user_data_destroy,
                       error);
}

void
kasasa_hyprland_stream_disable_dmabuf (KasasaHyprlandStream *self)
{
  if (self != NULL)
    g_atomic_int_set (&self->dmabuf_disabled, TRUE);
}

KasasaHyprlandStream *
kasasa_hyprland_stream_start_output (
  const gchar                   *name,
  guint                          frame_rate,
  KasasaHyprlandStreamFrameFunc  frame_cb,
  KasasaHyprlandStreamErrorFunc  error_cb,
  gpointer                      user_data,
  GDestroyNotify                user_data_destroy,
  GError                      **error)
{
  g_return_val_if_fail (name != NULL && *name != '\0', NULL);

  return start_stream (KASASA_HYPRLAND_STREAM_SOURCE_OUTPUT,
                       0,
                       name,
                       frame_rate,
                       frame_cb,
                       NULL,
                       error_cb,
                       user_data,
                       user_data_destroy,
                       error);
}

void
kasasa_hyprland_stream_stop (KasasaHyprlandStream *self)
{
  if (self == NULL)
    return;

  g_atomic_int_set (&self->stop_requested, TRUE);
  dmabuf_lease_pool_stop (self->dmabuf_lease_pool);

  if (self->thread != NULL)
    {
      g_thread_join (self->thread);
      self->thread = NULL;
    }

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  g_free (self->output_name);
  if (self->outputs != NULL)
    g_ptr_array_unref (self->outputs);
  if (self->dmabuf_format_table != NULL)
    g_array_unref (self->dmabuf_format_table);
  if (self->dmabuf_formats != NULL)
    g_array_unref (self->dmabuf_formats);
  if (self->dmabuf_tranche_formats != NULL)
    g_array_unref (self->dmabuf_tranche_formats);
  dmabuf_lease_pool_unref (self->dmabuf_lease_pool);
  g_free (self);
}
