/* kasasa-hyprland-stream.h
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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _KasasaHyprlandStream KasasaHyprlandStream;

typedef enum
{
  KASASA_HYPRLAND_STREAM_FORMAT_BGRX,
  KASASA_HYPRLAND_STREAM_FORMAT_BGRA,
  KASASA_HYPRLAND_STREAM_FORMAT_RGBX,
  KASASA_HYPRLAND_STREAM_FORMAT_RGBA,
} KasasaHyprlandStreamFormat;

/*
 * Called from the stream's worker thread with the Wayland SHM frame. The
 * callback must copy the pixels if it needs them after returning.
 */
typedef void (*KasasaHyprlandStreamFrameFunc) (
  gpointer                     user_data,
  const guint8                *data,
  gint                         width,
  gint                         height,
  gint                         stride,
  KasasaHyprlandStreamFormat   format,
  gboolean                     y_invert,
  guint32                      transform);

/* Called from the worker thread if startup fails or an active stream ends. */
typedef void (*KasasaHyprlandStreamErrorFunc) (
  gpointer                     user_data,
  const GError                *error);

/* Parse hyprctl address ("0x…") to the uint32 handle used by toplevel-export. */
gboolean kasasa_hyprland_stream_handle_from_address (const gchar *address,
                                                     guint32     *handle,
                                                     GError     **error);

gboolean kasasa_hyprland_stream_available (void);

/*
 * Start continuous capture of the window identified by handle.
 * The function returns after the worker thread is created. Wayland connection
 * and first-frame failures are reported through error_cb. frame_cb and
 * error_cb are invoked from the worker thread.
 */
KasasaHyprlandStream *kasasa_hyprland_stream_start (guint32                       handle,
                                                    KasasaHyprlandStreamFrameFunc  frame_cb,
                                                    KasasaHyprlandStreamErrorFunc  error_cb,
                                                    gpointer                      user_data,
                                                    GDestroyNotify                user_data_destroy,
                                                    GError                      **error);

/* Start native capture of the wl_output whose compositor name matches NAME. */
KasasaHyprlandStream *kasasa_hyprland_stream_start_output (
  const gchar                   *name,
  KasasaHyprlandStreamFrameFunc  frame_cb,
  KasasaHyprlandStreamErrorFunc  error_cb,
  gpointer                      user_data,
  GDestroyNotify                user_data_destroy,
  GError                      **error);

void kasasa_hyprland_stream_stop (KasasaHyprlandStream *stream);

G_END_DECLS
