/* test-wayland-integration.c
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

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>

#ifdef KASASA_HAVE_LAYER_SHELL
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif

static void
test_wayland_display (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  g_assert_nonnull (display);
  g_assert_true (GDK_IS_WAYLAND_DISPLAY (display));
}

static void
test_hyprland_session (void)
{
  g_auto (GStrv) desktops = NULL;
  const gchar *current_desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  const gchar *instance = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");
  const gchar *session_type = g_getenv ("XDG_SESSION_TYPE");

  g_assert_cmpstr (session_type, ==, "wayland");
  g_assert_nonnull (current_desktop);
  desktops = g_strsplit (current_desktop, ":", -1);
  g_assert_true (g_strv_contains ((const gchar * const *) desktops,
                                 "Hyprland"));
  g_assert_nonnull (instance);
  g_assert_cmpstr (instance, !=, "");
}

#ifdef KASASA_HAVE_LAYER_SHELL
static void
test_layer_shell_linkage (void)
{
  guint protocol_version = gtk_layer_get_protocol_version ();

  if (protocol_version == 0)
    {
      g_test_skip ("The compositor does not advertise Layer Shell");
      return;
    }

  g_assert_true (gtk_layer_is_supported ());
}
#endif

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/integration/wayland-display", test_wayland_display);
  g_test_add_func ("/integration/hyprland-session", test_hyprland_session);
#ifdef KASASA_HAVE_LAYER_SHELL
  g_test_add_func ("/integration/layer-shell-linkage",
                   test_layer_shell_linkage);
#endif

  return g_test_run ();
}
