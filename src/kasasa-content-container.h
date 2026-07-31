/* kasasa-content-container.h
 *
 * Copyright 2024-2025 Kelvin Novais
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

#include <adwaita.h>

#include "kasasa-window-picker.h"

G_BEGIN_DECLS

#define MAX_N_CONTENTS 5

#define KASASA_TYPE_CONTENT_CONTAINER (kasasa_content_container_get_type ())

G_DECLARE_FINAL_TYPE (KasasaContentContainer, kasasa_content_container, KASASA, CONTENT_CONTAINER, AdwBreakpointBin)

typedef struct
{
  gboolean (*screenshot_available) (void);
  gboolean (*screencast_available) (void);
  gboolean (*present_picker) (GtkWindow                  *parent,
                              const gchar                *title,
                              KasasaWindowPickerCallback  callback,
                              gpointer                    user_data,
                              GDestroyNotify              destroy,
                              GError                    **error);
  void (*capture_screenshot_async) (const KasasaWindowClient *client,
                                    GCancellable             *cancellable,
                                    GAsyncReadyCallback       callback,
                                    gpointer                  user_data);
  gchar *(*capture_screenshot_finish) (GAsyncResult *result,
                                       GError      **error);
  gboolean (*window_handle_from_address) (const gchar *address,
                                          guint32     *handle,
                                          GError     **error);
} KasasaNativeCaptureOps;

typedef struct
{
  gboolean (*available) (void);
  void (*capture_async) (GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data);
  gchar *(*capture_finish) (GAsyncResult *result,
                            GError      **error);
} KasasaRegionCaptureOps;

KasasaContentContainer *kasasa_content_container_new (void);
gboolean kasasa_content_container_append_screenshot (KasasaContentContainer *cc,
                                                     const gchar            *uri,
                                                     GError                **error);
void kasasa_content_container_set_native_capture_ops (
  KasasaContentContainer        *cc,
  const KasasaNativeCaptureOps  *ops);
void kasasa_content_container_set_region_capture_ops (
  KasasaContentContainer        *cc,
  const KasasaRegionCaptureOps  *ops);
void
kasasa_content_container_carousel_set_interactive (KasasaContentContainer *cc,
                                                   gboolean                interactive);
void kasasa_content_container_request_first_screenshot (KasasaContentContainer *cc);
void kasasa_content_container_request_first_hyprland_screencast (
  KasasaContentContainer *cc);
void kasasa_content_container_request_hyprland_screencast (
  KasasaContentContainer *cc);
/* Load a local image URI as the first pin (Hyprland/grim path). */
void kasasa_content_container_load_first_screenshot_uri (KasasaContentContainer *cc,
                                                         const gchar            *uri);
/* Pin a live Hyprland window by protocol handle (low 32 bits of address). */
void kasasa_content_container_load_first_hyprland_screencast (KasasaContentContainer *cc,
                                                              guint32                 window_handle,
                                                              gint                    width,
                                                              gint                    height);
/* Pin a live Hyprland monitor by wl_output name (no Portal). */
void kasasa_content_container_load_first_hyprland_monitor_screencast (
  KasasaContentContainer *cc,
  const gchar            *monitor_name,
  gint                    width,
  gint                    height);
gboolean kasasa_content_container_cancel_delayed_screenshot (KasasaContentContainer *cc);
gboolean kasasa_content_container_request_window_resize (KasasaContentContainer *cc);
gboolean kasasa_content_container_request_zoom_resize (KasasaContentContainer *cc,
                                                       gboolean                continuous);
gboolean kasasa_content_container_switch_page (KasasaContentContainer *cc,
                                                gint                    offset);
void kasasa_content_container_reveal_controls (KasasaContentContainer *cc,
                                               gboolean                reveal_child);
gboolean kasasa_content_container_controls_active (KasasaContentContainer *cc);

void kasasa_content_container_wipe_content (KasasaContentContainer *cc);

G_END_DECLS
