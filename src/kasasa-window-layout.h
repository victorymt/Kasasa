/* kasasa-window-layout.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* These values are part of the window's sizing contract, but do not require
 * GTK.  Keeping them here lets the sizing algorithm be tested headlessly. */
#define KASASA_WINDOW_LAYOUT_ZOOM_MIN 0.25
#define KASASA_WINDOW_LAYOUT_ZOOM_MAX 4.00
#define KASASA_WINDOW_LAYOUT_MIN_HEIGHT 110
#define KASASA_WINDOW_LAYOUT_MIN_WIDTH 212

typedef enum
{
  KASASA_SWITCH_RESIZE_FIT = 0,
  KASASA_SWITCH_RESIZE_KEEP_WIDTH,
  KASASA_SWITCH_RESIZE_KEEP_HEIGHT,
} KasasaSwitchResizeMode;

typedef struct
{
  gint content_width;
  gint content_height;
  gdouble monitor_width;
  gdouble monitor_height;
  gdouble content_scale;
  gint occupy_screen;
  gdouble zoom_factor;
} KasasaWindowLayoutInput;

typedef struct
{
  gdouble width;
  gdouble height;
  gdouble zoom_min;
  gdouble zoom_max;
  gdouble zoom_factor;
} KasasaWindowLayoutOutput;

/* Returns TRUE on success.  The function has no GTK or GSettings dependency. */
gboolean kasasa_window_layout_compute (const KasasaWindowLayoutInput  *input,
                                       KasasaWindowLayoutOutput       *output);

G_END_DECLS
