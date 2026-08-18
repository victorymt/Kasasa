/* kasasa-window-picker-logic.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean kasasa_window_picker_matches_search (const gchar *text,
                                               const gchar *query);

gint kasasa_window_picker_height_for_count (guint count);

G_END_DECLS
