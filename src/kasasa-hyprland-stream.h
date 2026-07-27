/* kasasa-hyprland-stream.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _KasasaHyprlandStream KasasaHyprlandStream;

/*
 * Called from the stream's worker thread with a tightly packed BGRA/BGRx frame
 * (4 bytes per pixel, stride == width * 4). The callback must copy the pixels
 * if it needs them after returning.
 */
typedef void (*KasasaHyprlandStreamFrameFunc) (gpointer     user_data,
                                               const guint8 *data,
                                               gint          width,
                                               gint          height,
                                               gint          stride,
                                               gboolean      has_alpha);

/* Parse hyprctl address ("0x…") to the uint32 handle used by toplevel-export. */
gboolean kasasa_hyprland_stream_handle_from_address (const gchar *address,
                                                     guint32     *handle,
                                                     GError     **error);

gboolean kasasa_hyprland_stream_available (void);

/*
 * Start continuous capture of the window identified by handle.
 * frame_cb is invoked from a background thread.
 */
KasasaHyprlandStream *kasasa_hyprland_stream_start (guint32                       handle,
                                                    KasasaHyprlandStreamFrameFunc  frame_cb,
                                                    gpointer                      user_data,
                                                    GDestroyNotify                user_data_destroy,
                                                    GError                      **error);

void kasasa_hyprland_stream_stop (KasasaHyprlandStream *stream);

G_END_DECLS
