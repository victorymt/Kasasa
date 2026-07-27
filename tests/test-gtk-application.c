/* test-gtk-application.c
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
#include <unistd.h>

#include "kasasa-application.h"
#include "kasasa-preferences.h"
#include "kasasa-window.h"

static gboolean cancel_delayed_requested;
static guint first_screenshot_requests;
static guint first_screencast_requests;
static guint screencast_requests;

/* Keep this test focused on application lifecycle rather than the real window. */
GType
kasasa_window_get_type (void)
{
  return GTK_TYPE_APPLICATION_WINDOW;
}

void
kasasa_window_take_first_screenshot (KasasaWindow *window)
{
  first_screenshot_requests++;
}

void
kasasa_window_take_first_screencast (KasasaWindow *window)
{
  first_screencast_requests++;
}

void
kasasa_window_request_screencast (KasasaWindow *window)
{
  screencast_requests++;
}

void
kasasa_window_load_first_screenshot_uri (KasasaWindow *window,
                                         const gchar  *uri)
{
  (void) window;
  (void) uri;
}

void
kasasa_window_load_first_hyprland_screencast (KasasaWindow *window,
                                              guint32       window_handle,
                                              gint          width,
                                              gint          height)
{
  (void) window;
  (void) window_handle;
  (void) width;
  (void) height;
}

void
kasasa_window_cancel_delayed_screenshot (KasasaWindow *window)
{
  cancel_delayed_requested = TRUE;
}

KasasaPreferences *
kasasa_preferences_new (void)
{
  return NULL;
}

static gboolean
on_close_request (GtkWindow *window,
                  gpointer   user_data)
{
  gboolean *close_requested = user_data;

  *close_requested = TRUE;
  return FALSE;
}

static void
test_repeated_activation_reuses_window (void)
{
  g_autofree gchar *application_id = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  g_autoptr (GError) error = NULL;
  GtkWindow *first_window;

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Activation%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  first_screenshot_requests = 0;
  first_screencast_requests = 0;
  screencast_requests = 0;
  g_application_activate (G_APPLICATION (application));
  first_window = gtk_application_get_active_window (
    GTK_APPLICATION (application));

  g_assert_nonnull (first_window);
  g_assert_cmpuint (first_screenshot_requests, ==, 1);
  g_assert_cmpuint (first_screencast_requests, ==, 0);

  g_application_activate (G_APPLICATION (application));

  g_assert_true (gtk_application_get_active_window (
                   GTK_APPLICATION (application)) == first_window);
  g_assert_cmpuint (first_screenshot_requests, ==, 1);
  g_assert_cmpuint (screencast_requests, ==, 0);

  gtk_window_destroy (first_window);
}

static void
test_screencast_option_starts_screencast (void)
{
  g_autofree gchar *application_id = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariantDict) options = NULL;
  GApplicationClass *app_class;
  GtkWindow *window;

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Screencast%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  first_screenshot_requests = 0;
  first_screencast_requests = 0;
  screencast_requests = 0;

  app_class = G_APPLICATION_GET_CLASS (application);
  options = g_variant_dict_new (NULL);
  g_variant_dict_insert (options, "screencast", "b", TRUE);
  g_assert_cmpint (app_class->handle_local_options (G_APPLICATION (application),
                                                    options),
                   ==,
                   -1);

  g_application_activate (G_APPLICATION (application));
  window = gtk_application_get_active_window (GTK_APPLICATION (application));

  g_assert_nonnull (window);
  g_assert_cmpuint (first_screenshot_requests, ==, 0);
  g_assert_cmpuint (first_screencast_requests, ==, 1);
  g_assert_cmpuint (screencast_requests, ==, 0);

  /* Second --screencast while already running should append a screencast. */
  g_assert_cmpint (app_class->handle_local_options (G_APPLICATION (application),
                                                    options),
                   ==,
                   -1);
  g_application_activate (G_APPLICATION (application));

  g_assert_true (gtk_application_get_active_window (
                   GTK_APPLICATION (application)) == window);
  g_assert_cmpuint (first_screencast_requests, ==, 1);
  g_assert_cmpuint (screencast_requests, ==, 1);

  gtk_window_destroy (window);
}

static void
test_application_lifecycle (void)
{
  g_autofree gchar *application_id = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  g_autoptr (GError) error = NULL;
  GtkWindow *window;
  gboolean close_requested = FALSE;

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Test%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);

  g_assert_cmpint (g_application_get_flags (G_APPLICATION (application)),
                   ==,
                   G_APPLICATION_HANDLES_COMMAND_LINE);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = GTK_WINDOW (gtk_application_window_new (GTK_APPLICATION (application)));
  g_signal_connect (window,
                    "close-request",
                    G_CALLBACK (on_close_request),
                    &close_requested);
  gtk_window_present (window);

  g_assert_true (gtk_application_get_active_window (GTK_APPLICATION (application))
                 == window);
  g_action_group_activate_action (G_ACTION_GROUP (application),
                                  "cancel-delayed-screenshot",
                                  NULL);
  g_assert_true (cancel_delayed_requested);

  g_action_group_activate_action (G_ACTION_GROUP (application), "quit", NULL);
  g_assert_true (close_requested);
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
  g_test_add_func ("/gtk/application/repeated-activation",
                   test_repeated_activation_reuses_window);
  g_test_add_func ("/gtk/application/screencast-option",
                   test_screencast_option_starts_screencast);
  g_test_add_func ("/gtk/application/lifecycle", test_application_lifecycle);

  return g_test_run ();
}
