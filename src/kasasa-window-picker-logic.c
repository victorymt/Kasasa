/* kasasa-window-picker-logic.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-window-picker-logic.h"

#include <string.h>

#define PICKER_ROW_HEIGHT 72
#define PICKER_BASE_HEIGHT 148
#define PICKER_MIN_HEIGHT 280
#define PICKER_MAX_HEIGHT 560

gboolean
kasasa_window_picker_matches_search (const gchar *text,
                                     const gchar *query)
{
  g_autofree gchar *folded_text = NULL;
  g_autofree gchar *folded_query = NULL;

  if (query == NULL || *query == '\0')
    return TRUE;
  if (text == NULL)
    return FALSE;

  folded_text = g_utf8_casefold (text, -1);
  folded_query = g_utf8_casefold (query, -1);
  return strstr (folded_text, folded_query) != NULL;
}

gint
kasasa_window_picker_height_for_count (guint count)
{
  guint visible_rows = MIN (count, 6);

  return CLAMP (PICKER_BASE_HEIGHT + (gint) visible_rows * PICKER_ROW_HEIGHT,
                PICKER_MIN_HEIGHT,
                PICKER_MAX_HEIGHT);
}
