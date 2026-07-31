/* kasasa-hyprland-capture.h
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

#include <gio/gio.h>

#include "kasasa-window-query.h"

G_BEGIN_DECLS

/*
 * Capture a window through Hyprland's toplevel-export protocol. This captures
 * the window itself even when it is not visible on the current workspace.
 * Returns a file:// URI the caller owns (g_free), or NULL on error.
 */
gchar *kasasa_hyprland_capture_screenshot (const KasasaWindowClient *client,
                                           GError                  **error);
void kasasa_hyprland_capture_screenshot_async (
  const KasasaWindowClient *client,
  GCancellable             *cancellable,
  GAsyncReadyCallback       callback,
  gpointer                  user_data);
gchar *kasasa_hyprland_capture_screenshot_finish (GAsyncResult *result,
                                                  GError      **error);

gboolean kasasa_hyprland_capture_available (void);

G_END_DECLS
