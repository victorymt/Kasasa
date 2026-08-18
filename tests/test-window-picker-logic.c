/* test-window-picker-logic.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>

#include "kasasa-window-picker-logic.h"

static void
test_matches_search (void)
{
  g_assert_true (kasasa_window_picker_matches_search ("Alacritty", "alac"));
  g_assert_true (kasasa_window_picker_matches_search ("Résumé", "RÉSUM"));
  g_assert_true (kasasa_window_picker_matches_search ("anything", NULL));
  g_assert_true (kasasa_window_picker_matches_search (NULL, ""));
  g_assert_false (kasasa_window_picker_matches_search (NULL, "window"));
  g_assert_false (kasasa_window_picker_matches_search ("terminal", "browser"));
}

static void
test_height_limits (void)
{
  g_assert_cmpint (kasasa_window_picker_height_for_count (0), ==, 280);
  g_assert_cmpint (kasasa_window_picker_height_for_count (1), ==, 280);
  g_assert_cmpint (kasasa_window_picker_height_for_count (2), ==, 292);
  g_assert_cmpint (kasasa_window_picker_height_for_count (6), ==, 560);
  g_assert_cmpint (kasasa_window_picker_height_for_count (G_MAXUINT), ==, 560);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unit/window-picker/matches-search", test_matches_search);
  g_test_add_func ("/unit/window-picker/height-limits", test_height_limits);
  return g_test_run ();
}
