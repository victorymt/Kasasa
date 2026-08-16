/* kasasa-hyprland-capture-private.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "kasasa-hyprland-stream.h"

G_BEGIN_DECLS

typedef struct
{
  gboolean (*available) (void);
  gboolean (*handle_from_address) (const gchar *address,
                                   guint32     *handle,
                                   GError     **error);
  KasasaHyprlandStream *(*start) (
    guint32                       handle,
    guint                         frame_rate,
    KasasaHyprlandStreamFrameFunc frame_cb,
    KasasaHyprlandStreamErrorFunc error_cb,
    gpointer                      user_data,
    GDestroyNotify                user_data_destroy,
    GError                      **error);
  void (*stop) (KasasaHyprlandStream *stream);
} KasasaHyprlandCaptureBackendOps;

#ifdef KASASA_ENABLE_TESTS
void kasasa_hyprland_capture_test_set_backend (
  const KasasaHyprlandCaptureBackendOps *ops);
void kasasa_hyprland_capture_test_set_timeout (gint64 timeout_usec);
void kasasa_hyprland_capture_test_reset (void);
guint8 *kasasa_hyprland_capture_test_copy_frame (
  const guint8                *data,
  gint                         width,
  gint                         height,
  gint                         stride,
  KasasaHyprlandStreamFormat   format,
  gboolean                     y_invert,
  guint32                      transform,
  gint                        *output_width,
  gint                        *output_height,
  GError                     **error);
#endif

G_END_DECLS
