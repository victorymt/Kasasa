/* test-wayland-integration.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>

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

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/integration/wayland-display", test_wayland_display);
  g_test_add_func ("/integration/hyprland-session", test_hyprland_session);

  return g_test_run ();
}
