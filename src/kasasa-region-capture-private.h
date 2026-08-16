/* kasasa-region-capture-private.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#ifdef KASASA_ENABLE_TESTS
gboolean kasasa_region_capture_test_map_geometry (
  gint   origin_x,
  gint   origin_y,
  guint  logical_width,
  guint  logical_height,
  gint   frame_width,
  gint   frame_height,
  gint   selection_x,
  gint   selection_y,
  guint  selection_width,
  guint  selection_height,
  gint  *pixel_x,
  gint  *pixel_y,
  guint *pixel_width,
  guint *pixel_height);
#endif

G_END_DECLS
