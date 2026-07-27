/* kasasa-window.h
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

#include "kasasa-zoom.h"

G_BEGIN_DECLS

typedef enum
{
  OPACITY_INCREASE,
  OPACITY_DECREASE
} Opacity;

typedef enum
{
  KASASA_SWITCH_RESIZE_FIT = 0,
  KASASA_SWITCH_RESIZE_KEEP_WIDTH,
  KASASA_SWITCH_RESIZE_KEEP_HEIGHT,
} KasasaSwitchResizeMode;

#define WINDOW_HIDING_DURATION 110
#define WINDOW_WAITING_HIDING_DURATION (2 * WINDOW_HIDING_DURATION)

#define WINDOW_MINIATURIZATION_DELAY 3

#define WINDOW_RESIZING_DURATION 500

/* Scroll-wheel zoom relative to the auto-fitted (occupy-screen) size */
#define WINDOW_ZOOM_MIN      0.25
#define WINDOW_ZOOM_MAX      4.00

// Due to miniaturization, the real min dimensions are set here (width-request
// and height-request)
#define WINDOW_MIN_HEIGHT 110
#define WINDOW_MIN_WIDTH  212

#define KASASA_TYPE_WINDOW (kasasa_window_get_type ())

G_DECLARE_FINAL_TYPE (KasasaWindow, kasasa_window, KASASA, WINDOW, AdwApplicationWindow)

typedef void (* HideWindowCallback)(gpointer);

KasasaWindow * kasasa_window_get_window_reference (GtkWidget *widget);
gboolean kasasa_window_get_trash_button_active (KasasaWindow *window);
gboolean kasasa_window_is_miniaturized (KasasaWindow *window);
void kasasa_window_hide_window (KasasaWindow           *window,
                                 gboolean                hide,
                                 HideWindowCallback      callback,
                                 GObject                *callback_data);
void kasasa_window_change_opacity (KasasaWindow *window,
                                   Opacity       opacity_direction);
void kasasa_window_resize_window (KasasaWindow *window,
                                  gdouble       new_height,
                                  gdouble       new_width);
gboolean kasasa_window_resize_window_scaling (KasasaWindow *window,
                                              gdouble       new_height,
                                              gdouble       new_width);
gboolean kasasa_window_resize_window_scaling_for_zoom (KasasaWindow *window,
                                                       gdouble       new_height,
                                                       gdouble       new_width);
gboolean kasasa_window_resize_for_content_switch (KasasaWindow           *window,
                                                  gdouble                 new_height,
                                                  gdouble                 new_width,
                                                  KasasaSwitchResizeMode  mode);
void kasasa_window_reset_zoom (KasasaWindow *window);
gboolean kasasa_window_apply_zoom_delta (KasasaWindow   *window,
                                         gdouble         delta,
                                         KasasaZoomInput input);
gdouble kasasa_window_get_zoom_factor (KasasaWindow *window);
void kasasa_window_auto_discard_window (KasasaWindow *window);
void kasasa_window_miniaturize_window (KasasaWindow *window,
                                       gboolean      miniaturize);
void kasasa_window_block_miniaturization (KasasaWindow *window,
                                          gboolean      block);
void kasasa_window_take_first_screenshot (KasasaWindow *window);
void kasasa_window_take_first_screencast (KasasaWindow *window);
void kasasa_window_request_screencast (KasasaWindow *window);
/* Pin a local screenshot URI as the first capture (no Portal). */
void kasasa_window_load_first_screenshot_uri (KasasaWindow *window,
                                              const gchar  *uri);
void kasasa_window_load_first_hyprland_screencast (KasasaWindow *window,
                                                   guint32       window_handle,
                                                   gint          width,
                                                   gint          height);
void kasasa_window_cancel_delayed_screenshot (KasasaWindow *window);

G_END_DECLS

/*
 * Descanxe em paz, tia Eldenir
 * nós te amamos muito.
 *
 * 10/05/25
 */
