/* kasasa-content-host.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

#include "kasasa-window-layout.h"

G_BEGIN_DECLS

typedef enum
{
  OPACITY_INCREASE,
  OPACITY_DECREASE
} Opacity;

typedef void (* HideWindowCallback)(gpointer);

typedef struct
{
  gboolean (*is_miniaturized) (gpointer user_data);
  void (*hide_window) (gpointer            user_data,
                       gboolean            hide,
                       HideWindowCallback  callback,
                       GObject            *callback_data);
  void (*change_opacity) (gpointer user_data,
                          Opacity opacity_direction);
  gboolean (*resize) (gpointer               user_data,
                      gdouble                new_height,
                      gdouble                new_width,
                      KasasaSwitchResizeMode mode,
                      gboolean               for_zoom,
                      gboolean               continuous);
  void (*reset_zoom) (gpointer user_data);
  void (*auto_discard_window) (gpointer user_data);
  void (*miniaturize_window) (gpointer user_data,
                              gboolean miniaturize);
  void (*block_miniaturization) (gpointer user_data,
                                 gboolean block);
  void (*set_controls_popup_active) (gpointer user_data,
                                     gboolean active);
  void (*set_crop_mode) (gpointer user_data,
                         gboolean active);
  void (*finish_initial_reveal) (gpointer user_data);
  gboolean (*is_initial_reveal_pending) (gpointer user_data);
} KasasaContentHostOps;

G_END_DECLS
