/* kasasa-hyprland-capture.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "kasasa-window-query.h"

G_BEGIN_DECLS

/*
 * Capture a window with grim using the client geometry.
 * Returns a file:// URI the caller owns (g_free), or NULL on error.
 */
gchar *kasasa_hyprland_capture_screenshot (const KasasaWindowClient *client,
                                           GError                  **error);

gboolean kasasa_hyprland_capture_available (void);

G_END_DECLS
