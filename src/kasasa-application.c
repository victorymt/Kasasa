/* kasasa-application.c
 *
 * Copyright 2024-2026 Kelvin Novais
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

#include "config.h"

#include <glib/gi18n.h>

#include "kasasa-application.h"
#include "kasasa-preferences.h"
#include "kasasa-window.h"

struct _KasasaApplication
{
  AdwApplication parent_instance;

  gboolean start_with_screencast;
};

G_DEFINE_FINAL_TYPE (KasasaApplication, kasasa_application, ADW_TYPE_APPLICATION)

KasasaApplication *
kasasa_application_new (const char *application_id)
{
  g_return_val_if_fail (application_id != NULL, NULL);

  return g_object_new (KASASA_TYPE_APPLICATION,
                       "application-id", application_id,
                       "flags", G_APPLICATION_HANDLES_COMMAND_LINE,
                       NULL);
}

static void
present_or_create_window (KasasaApplication *self,
                          gboolean           start_with_screencast)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (self));

  // Re-activation with an existing window: raise it. If the user asked for a
  // screencast via CLI while already running, append one instead of forcing a
  // brand-new first capture.
  if (window != NULL)
    {
      gtk_window_present (window);
      if (start_with_screencast && KASASA_IS_WINDOW (window))
        kasasa_window_request_screencast (KASASA_WINDOW (window));
      return;
    }

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", self,
                         NULL);

  gtk_window_present (window);

  if (start_with_screencast)
    {
      // Keep the surface mapped: GStreamer gtk4paintablesink / GL needs a live
      // Wayland surface. Hide with opacity instead of unmapping (set_visible
      // FALSE), which triggers "Error 71 (Protocol error)" on Hyprland.
      gtk_widget_set_opacity (GTK_WIDGET (window), 0.0);
      kasasa_window_take_first_screencast (KASASA_WINDOW (window));
    }
  else
    {
      // Screenshots only need a portal file URI — unmap until the pin is ready.
      gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
      kasasa_window_take_first_screenshot (KASASA_WINDOW (window));
    }
}

static void
kasasa_application_activate (GApplication *app)
{
  KasasaApplication *self = KASASA_APPLICATION (app);

  g_assert (KASASA_IS_APPLICATION (app));

  present_or_create_window (self, self->start_with_screencast);
  self->start_with_screencast = FALSE;
}

static gint
kasasa_application_handle_local_options (GApplication *app,
                                         GVariantDict *options)
{
  KasasaApplication *self = KASASA_APPLICATION (app);

  self->start_with_screencast = g_variant_dict_contains (options, "screencast");
  return -1;
}

static int
kasasa_application_command_line (GApplication            *app,
                                 GApplicationCommandLine *cmdline)
{
  KasasaApplication *self = KASASA_APPLICATION (app);
  GVariantDict *options = g_application_command_line_get_options_dict (cmdline);
  gboolean start_with_screencast = g_variant_dict_contains (options, "screencast");

  present_or_create_window (self, start_with_screencast);
  return 0;
}

static void
kasasa_application_preferences_action (GSimpleAction *action,
                                       GVariant *parameter,
                                       gpointer app)
{
  KasasaPreferences *preferences;
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (app));
  preferences = kasasa_preferences_new ();
  adw_dialog_present (ADW_DIALOG (preferences), GTK_WIDGET (window));
}

static void
kasasa_application_class_init (KasasaApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  app_class->activate = kasasa_application_activate;
  app_class->handle_local_options = kasasa_application_handle_local_options;
  app_class->command_line = kasasa_application_command_line;
}

static void
kasasa_application_about_action (GSimpleAction *action,
                                 GVariant *parameter,
                                 gpointer user_data)
{
  static const char *developers[] = { "Kelvin Ribeiro Novais", NULL };
  KasasaApplication *self = user_data;
  GtkWindow *window = NULL;

  g_assert (KASASA_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));

  adw_show_about_dialog (GTK_WIDGET (window),
                         "application-name", _ ("Kasasa"),
                         "application-icon", "io.github.kelvinnovais.Kasasa",
                         "developer-name", "Kelvin Ribeiro Novais",
                         "version", PACKAGE_VERSION,
                         "comments", _ ("Snip and pin useful information"
                                        "\n\nIf you liked the app ❤️, consider giving it a star ⭐:"),
                         "issue-url", "https://github.com/KelvinNovais/Kasasa/issues",
                         "website", "https://github.com/KelvinNovais/Kasasa",
                         "developers", developers,
                         "copyright", "© 2024-2026 Kelvin Ribeiro Novais",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         // Translators: Replace "translator-credits" with your names, one name per line
                         "translator_credits", _ ("translator-credits"),
                         NULL);
}

static void
kasasa_application_quit_action (GSimpleAction *action,
                                GVariant *parameter,
                                gpointer user_data)
{
  KasasaApplication *self = user_data;
  GtkWindow *window;

  g_assert (KASASA_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window != NULL)
    gtk_window_close (window);
  else
    g_application_quit (G_APPLICATION (self));
}

static void
kasasa_application_cancel_delayed_screenshot_action (GSimpleAction *action,
                                                      GVariant      *parameter,
                                                      gpointer       user_data)
{
  KasasaApplication *self = user_data;
  GtkWindow *window;

  g_assert (KASASA_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window == NULL)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));

      if (windows != NULL)
        window = GTK_WINDOW (windows->data);
    }
  if (KASASA_IS_WINDOW (window))
    kasasa_window_cancel_delayed_screenshot (KASASA_WINDOW (window));
}

static const GActionEntry app_actions[] = {
  { "quit", kasasa_application_quit_action },
  { "cancel-delayed-screenshot", kasasa_application_cancel_delayed_screenshot_action },
  { "about", kasasa_application_about_action },
  { "preferences", kasasa_application_preferences_action }
};

static void
kasasa_application_init (KasasaApplication *self)
{
  const GOptionEntry entries[] = {
    {
      .long_name = "screencast",
      .short_name = 'c',
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_NONE,
      .arg_data = NULL,
      .description = N_("Start by pinning a screencast instead of a screenshot"),
      .arg_description = NULL,
    },
    { NULL }
  };

  self->start_with_screencast = FALSE;

  g_application_add_main_option_entries (G_APPLICATION (self), entries);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);
  gtk_application_set_accels_for_action (GTK_APPLICATION (self),
                                         "app.quit",
                                         (const char *[]) { "<primary>q", NULL });
}
