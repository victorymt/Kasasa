/* test-gtk-preferences.c
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

#include <adwaita.h>

#include "kasasa-preferences.h"

static GtkWidget *
find_widget_by_id (GtkWidget  *widget,
                   const char *id)
{
  GtkWidget *child;
  const char *widget_id;

  widget_id = gtk_buildable_get_buildable_id (GTK_BUILDABLE (widget));
  if (g_strcmp0 (widget_id, id) == 0)
    return widget;

  for (child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *match = find_widget_by_id (child, id);

      if (match != NULL)
        return match;
    }

  return NULL;
}

static void
reset_settings (GSettings *settings)
{
  const char *keys[] = {
    "change-opacity",
    "opacity",
    "miniaturize-window",
    "auto-hide-menu",
    "controls-timeout",
    "occupy-screen",
    "image-switch-resize-mode",
    "auto-discard-window",
    "auto-discard-window-time",
    "screenshot-delay",
    "auto-trash-image",
  };

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    g_settings_reset (settings, keys[i]);
}

static KasasaPreferences *
create_preferences (void)
{
  KasasaPreferences *preferences = kasasa_preferences_new ();

  return g_object_ref_sink (preferences);
}

static GtkWidget *
get_preferences_child (KasasaPreferences *preferences,
                       const char        *id)
{
  GtkWidget *dialog_child;
  GtkWidget *widget;

  dialog_child = adw_dialog_get_child (ADW_DIALOG (preferences));
  g_assert_nonnull (dialog_child);
  widget = find_widget_by_id (dialog_child, id);
  g_assert_nonnull (widget);

  return widget;
}

static void
test_hiding_modes_are_mutually_exclusive (void)
{
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (KasasaPreferences) preferences = NULL;
  AdwExpanderRow *opacity_row;
  AdwSwitchRow *miniaturize_row;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  reset_settings (settings);
  g_assert_true (g_settings_set_boolean (settings, "change-opacity", FALSE));
  g_assert_true (g_settings_set_boolean (settings, "miniaturize-window", FALSE));

  preferences = create_preferences ();
  opacity_row = ADW_EXPANDER_ROW (
    get_preferences_child (preferences, "opacity_expander_row"));
  miniaturize_row = ADW_SWITCH_ROW (
    get_preferences_child (preferences, "miniaturize_switch"));

  adw_expander_row_set_enable_expansion (opacity_row, TRUE);
  g_assert_true (g_settings_get_boolean (settings, "change-opacity"));
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (miniaturize_row)));

  adw_expander_row_set_enable_expansion (opacity_row, FALSE);
  g_assert_false (g_settings_get_boolean (settings, "change-opacity"));
  g_assert_true (gtk_widget_get_sensitive (GTK_WIDGET (miniaturize_row)));

  adw_switch_row_set_active (miniaturize_row, TRUE);
  g_assert_true (g_settings_get_boolean (settings, "miniaturize-window"));
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (opacity_row)));

  adw_switch_row_set_active (miniaturize_row, FALSE);
  g_assert_false (g_settings_get_boolean (settings, "miniaturize-window"));
  g_assert_true (gtk_widget_get_sensitive (GTK_WIDGET (opacity_row)));
}

static void
test_all_settings_bindings_persist (void)
{
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (KasasaPreferences) preferences = NULL;
  AdwExpanderRow *opacity_row;
  AdwSwitchRow *auto_hide_row;
  AdwSwitchRow *auto_discard_row;
  AdwSwitchRow *auto_trash_row;
  AdwComboRow *image_switch_resize_combo;
  AdwSpinRow *opacity_spin;
  AdwSpinRow *controls_timeout_spin;
  AdwSpinRow *occupy_screen_spin;
  AdwSpinRow *auto_discard_time_spin;
  AdwSpinRow *screenshot_delay_spin;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  reset_settings (settings);
  preferences = create_preferences ();

  opacity_row = ADW_EXPANDER_ROW (
    get_preferences_child (preferences, "opacity_expander_row"));
  opacity_spin = ADW_SPIN_ROW (
    get_preferences_child (preferences, "opacity_spin_row"));
  auto_hide_row = ADW_SWITCH_ROW (
    get_preferences_child (preferences, "auto_hide_menu_switch"));
  controls_timeout_spin = ADW_SPIN_ROW (
    get_preferences_child (preferences, "controls_timeout_spin_row"));
  occupy_screen_spin = ADW_SPIN_ROW (
    get_preferences_child (preferences, "occupy_screen_spin_row"));
  image_switch_resize_combo = ADW_COMBO_ROW (
    get_preferences_child (preferences, "image_switch_resize_combo"));
  auto_discard_row = ADW_SWITCH_ROW (
    get_preferences_child (preferences, "auto_discard_window_switch"));
  auto_discard_time_spin = ADW_SPIN_ROW (
    get_preferences_child (preferences,
                           "auto_discard_window_time_spin_row"));
  screenshot_delay_spin = ADW_SPIN_ROW (
    get_preferences_child (preferences, "screenshot_delay_spin_row"));
  auto_trash_row = ADW_SWITCH_ROW (
    get_preferences_child (preferences, "auto_trash_image_switch"));

  adw_expander_row_set_enable_expansion (opacity_row, TRUE);
  gtk_adjustment_set_value (adw_spin_row_get_adjustment (opacity_spin), 0.63);
  adw_switch_row_set_active (auto_hide_row, FALSE);
  gtk_adjustment_set_value (
    adw_spin_row_get_adjustment (controls_timeout_spin), 1.25);
  gtk_adjustment_set_value (
    adw_spin_row_get_adjustment (occupy_screen_spin), 42.0);
  adw_combo_row_set_selected (image_switch_resize_combo, 2);
  adw_switch_row_set_active (auto_discard_row, TRUE);
  gtk_adjustment_set_value (
    adw_spin_row_get_adjustment (auto_discard_time_spin), 17.0);
  gtk_adjustment_set_value (
    adw_spin_row_get_adjustment (screenshot_delay_spin), 9.0);
  adw_switch_row_set_active (auto_trash_row, TRUE);

  g_assert_true (g_settings_get_boolean (settings, "change-opacity"));
  g_assert_cmpfloat_with_epsilon (
    g_settings_get_double (settings, "opacity"), 0.63, 0.0001);
  g_assert_false (g_settings_get_boolean (settings, "auto-hide-menu"));
  g_assert_cmpfloat_with_epsilon (
    g_settings_get_double (settings, "controls-timeout"), 1.25, 0.0001);
  g_assert_cmpint (g_settings_get_int (settings, "occupy-screen"), ==, 42);
  g_assert_cmpuint (g_settings_get_uint (settings, "image-switch-resize-mode"),
                    ==,
                    2);
  g_assert_true (g_settings_get_boolean (settings, "auto-discard-window"));
  g_assert_cmpfloat_with_epsilon (
    g_settings_get_double (settings, "auto-discard-window-time"),
    17.0,
    0.0001);
  g_assert_cmpuint (g_settings_get_uint (settings, "screenshot-delay"), ==, 9);
  g_assert_true (g_settings_get_boolean (settings, "auto-trash-image"));

  g_clear_object (&preferences);
  preferences = create_preferences ();

  g_assert_true (adw_expander_row_get_enable_expansion (
    ADW_EXPANDER_ROW (get_preferences_child (preferences,
                                             "opacity_expander_row"))));
  g_assert_cmpfloat_with_epsilon (
    gtk_adjustment_get_value (adw_spin_row_get_adjustment (
      ADW_SPIN_ROW (get_preferences_child (preferences, "opacity_spin_row")))),
    0.63,
    0.0001);
  g_assert_false (adw_switch_row_get_active (
    ADW_SWITCH_ROW (get_preferences_child (preferences,
                                           "auto_hide_menu_switch"))));
  g_assert_cmpfloat_with_epsilon (
    gtk_adjustment_get_value (adw_spin_row_get_adjustment (
      ADW_SPIN_ROW (get_preferences_child (preferences,
                                           "controls_timeout_spin_row")))),
    1.25,
    0.0001);
  g_assert_cmpfloat_with_epsilon (
    gtk_adjustment_get_value (adw_spin_row_get_adjustment (
      ADW_SPIN_ROW (get_preferences_child (preferences,
                                           "occupy_screen_spin_row")))),
    42.0,
    0.0001);
  g_assert_cmpuint (adw_combo_row_get_selected (
                      ADW_COMBO_ROW (get_preferences_child (
                        preferences, "image_switch_resize_combo"))),
                    ==,
                    2);
  g_assert_true (adw_switch_row_get_active (
    ADW_SWITCH_ROW (get_preferences_child (preferences,
                                           "auto_discard_window_switch"))));
  g_assert_cmpfloat_with_epsilon (
    gtk_adjustment_get_value (adw_spin_row_get_adjustment (
      ADW_SPIN_ROW (get_preferences_child (
        preferences, "auto_discard_window_time_spin_row")))),
    17.0,
    0.0001);
  g_assert_cmpfloat_with_epsilon (
    gtk_adjustment_get_value (adw_spin_row_get_adjustment (
      ADW_SPIN_ROW (get_preferences_child (preferences,
                                           "screenshot_delay_spin_row")))),
    9.0,
    0.0001);
  g_assert_true (adw_switch_row_get_active (
    ADW_SWITCH_ROW (get_preferences_child (preferences,
                                           "auto_trash_image_switch"))));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    {
      if (g_getenv ("KASASA_REQUIRE_DISPLAY") != NULL)
        g_error ("A display is required for GTK component tests");

      g_test_message ("No display available; skipping GTK component tests");
      return 77;
    }

  adw_init ();
  g_test_add_func ("/gtk/preferences/hiding-modes-mutually-exclusive",
                   test_hiding_modes_are_mutually_exclusive);
  g_test_add_func ("/gtk/preferences/all-bindings-persist",
                   test_all_settings_bindings_persist);

  return g_test_run ();
}
