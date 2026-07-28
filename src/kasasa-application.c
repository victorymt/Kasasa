/* kasasa-application.c
 *
 * Copyright 2024-2026 Kelvin Novais
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

#include "config.h"

#include <glib/gi18n.h>
#include <stdio.h>

#include "kasasa-application.h"
#include "kasasa-hyprland-capture.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-preferences.h"
#include "kasasa-window-query.h"
#include "kasasa-window.h"

/* Exit codes for CLI utilities */
#define KASASA_EXIT_OK            0
#define KASASA_EXIT_ERROR         1
#define KASASA_EXIT_MATCH         2
#define KASASA_EXIT_UNAVAILABLE   3

struct _KasasaApplication
{
  AdwApplication parent_instance;

  gboolean start_with_screencast;
  gboolean list_windows;
  gboolean list_json;
  gchar   *window_spec;
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

static int
query_error_to_exit_code (const GError *error)
{
  if (error == NULL)
    return KASASA_EXIT_ERROR;

  if (g_error_matches (error, KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE))
    return KASASA_EXIT_UNAVAILABLE;

  if (g_error_matches (error, KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_NO_MATCH)
      || g_error_matches (error, KASASA_WINDOW_QUERY_ERROR,
                          KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS))
    return KASASA_EXIT_MATCH;

  return KASASA_EXIT_ERROR;
}

static int
run_list_windows (gboolean as_json)
{
  g_autoptr (GPtrArray) clients = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *text = NULL;

  clients = kasasa_window_query_list_clients (&error);
  if (clients == NULL)
    {
      g_printerr ("%s\n", error != NULL ? error->message : _("Failed to list windows"));
      return query_error_to_exit_code (error);
    }

  text = as_json
         ? kasasa_window_query_format_json (clients)
         : kasasa_window_query_format_table (clients);
  g_print ("%s", text);
  if (as_json)
    g_print ("\n");

  return KASASA_EXIT_OK;
}

static GtkWindow *
ensure_pin_window (KasasaApplication *self,
                   gboolean           keep_mapped)
{
  GtkWindow *window;

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  if (window != NULL)
    {
      gtk_window_present (window);
      return window;
    }

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", self,
                         NULL);
  gtk_window_present (window);
  if (keep_mapped)
    gtk_widget_set_opacity (GTK_WIDGET (window), 0.0);
  else
    gtk_widget_set_visible (GTK_WIDGET (window), FALSE);

  return window;
}

static int
present_targeted_capture (KasasaApplication *self,
                          const gchar       *window_spec,
                          gboolean           screencast)
{
  g_autoptr (KasasaWindowClient) client = NULL;
  g_autoptr (GError) error = NULL;
  GtkWindow *window;

  client = kasasa_window_query_resolve_live (window_spec, &error);
  if (client == NULL)
    {
      g_printerr ("%s\n", error != NULL ? error->message : _("Failed to resolve window"));
      return query_error_to_exit_code (error);
    }

  if (screencast)
    {
      guint32 handle = 0;

      if (!kasasa_hyprland_stream_available ())
        {
          g_printerr ("%s\n",
                      _("Targeted live screencast requires Hyprland Wayland"));
          return KASASA_EXIT_UNAVAILABLE;
        }

      if (!kasasa_hyprland_stream_handle_from_address (client->address,
                                                       &handle,
                                                       &error))
        {
          g_printerr ("%s\n",
                      error != NULL ? error->message : _("Invalid window address"));
          return query_error_to_exit_code (error);
        }

      window = ensure_pin_window (self, TRUE);
      if (!KASASA_IS_WINDOW (window))
        return KASASA_EXIT_ERROR;

      kasasa_window_load_first_hyprland_screencast (KASASA_WINDOW (window),
                                                    handle,
                                                    client->width,
                                                    client->height);
      return KASASA_EXIT_OK;
    }
  else
    {
      g_autofree gchar *uri = NULL;

      uri = kasasa_hyprland_capture_screenshot (client, &error);
      if (uri == NULL)
        {
          g_printerr ("%s\n",
                      error != NULL ? error->message : _("Failed to capture window"));
          return query_error_to_exit_code (error);
        }

      window = ensure_pin_window (self, FALSE);
      if (!KASASA_IS_WINDOW (window))
        return KASASA_EXIT_ERROR;

      kasasa_window_load_first_screenshot_uri (KASASA_WINDOW (window), uri);
      return KASASA_EXIT_OK;
    }
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

  if (self->list_windows)
    {
      int code = run_list_windows (self->list_json);
      g_application_quit (app);
      /* g_application_run will still return 0 from activate; list is handled
       * primarily in command-line / local options. */
      (void) code;
      return;
    }

  if (self->window_spec != NULL)
    {
      present_targeted_capture (self,
                                self->window_spec,
                                self->start_with_screencast);
      return;
    }

  present_or_create_window (self, self->start_with_screencast);
  self->start_with_screencast = FALSE;
}

static gint
kasasa_application_handle_local_options (GApplication *app,
                                         GVariantDict *options)
{
  KasasaApplication *self = KASASA_APPLICATION (app);
  const gchar *window_spec = NULL;

  self->start_with_screencast = g_variant_dict_contains (options, "screencast");
  self->list_windows = g_variant_dict_contains (options, "list-windows");
  self->list_json = g_variant_dict_contains (options, "json");
  g_clear_pointer (&self->window_spec, g_free);

  if (g_variant_dict_lookup (options, "window", "&s", &window_spec)
      && window_spec != NULL)
    self->window_spec = g_strdup (window_spec);

  if (self->list_json && !self->list_windows)
    {
      g_printerr ("%s\n", _("--json requires --list-windows"));
      return KASASA_EXIT_ERROR;
    }

  /* Pure listing: no remote activation / no GUI. */
  if (self->list_windows)
    return run_list_windows (self->list_json);

  return -1;
}

static int
kasasa_application_command_line (GApplication            *app,
                                 GApplicationCommandLine *cmdline)
{
  KasasaApplication *self = KASASA_APPLICATION (app);
  GVariantDict *options = g_application_command_line_get_options_dict (cmdline);
  const gchar *window_spec = NULL;
  gboolean start_with_screencast;
  gboolean list_windows;
  gboolean list_json;

  start_with_screencast = g_variant_dict_contains (options, "screencast");
  list_windows = g_variant_dict_contains (options, "list-windows");
  list_json = g_variant_dict_contains (options, "json");

  if (list_json && !list_windows)
    {
      g_application_command_line_printerr (cmdline,
                                           "%s\n",
                                           _("--json requires --list-windows"));
      return KASASA_EXIT_ERROR;
    }

  if (list_windows)
    return run_list_windows (list_json);

  if (g_variant_dict_lookup (options, "window", "&s", &window_spec)
      && window_spec != NULL)
    {
      return present_targeted_capture (self, window_spec, start_with_screencast);
    }

  present_or_create_window (self, start_with_screencast);
  return KASASA_EXIT_OK;
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
kasasa_application_dispose (GObject *object)
{
  KasasaApplication *self = KASASA_APPLICATION (object);

  g_clear_pointer (&self->window_spec, g_free);

  G_OBJECT_CLASS (kasasa_application_parent_class)->dispose (object);
}

static void
kasasa_application_class_init (KasasaApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = kasasa_application_dispose;
  app_class->activate = kasasa_application_activate;
  app_class->handle_local_options = kasasa_application_handle_local_options;
  app_class->command_line = kasasa_application_command_line;
}

static void
kasasa_application_about_action (GSimpleAction *action,
                                 GVariant *parameter,
                                 gpointer user_data)
{
  static const char *developers[] = {
    "Kelvin Ribeiro Novais",
    "victorymt",
    NULL,
  };
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
                         "issue-url", "https://github.com/victorymt/Kasasa/issues",
                         "website", "https://github.com/victorymt/Kasasa",
                         "developers", developers,
                         "copyright", "© 2024-2026 Kelvin Ribeiro Novais\n© 2026 victorymt",
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
      .description = _("Start by pinning a screencast instead of a screenshot"),
      .arg_description = NULL,
    },
    {
      .long_name = "list-windows",
      .short_name = 'l',
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_NONE,
      .arg_data = NULL,
      .description = _("List capturable windows (Hyprland) and exit"),
      .arg_description = NULL,
    },
    {
      .long_name = "json",
      .short_name = 0,
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_NONE,
      .arg_data = NULL,
      .description = _("With --list-windows, print JSON"),
      .arg_description = NULL,
    },
    {
      .long_name = "window",
      .short_name = 'w',
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_STRING,
      .arg_data = NULL,
      .description = _("Capture a window: class, title:…, address:…, or active"),
      .arg_description = _("SPEC"),
    },
    { NULL }
  };

  self->start_with_screencast = FALSE;
  self->list_windows = FALSE;
  self->list_json = FALSE;
  self->window_spec = NULL;

  g_application_add_main_option_entries (G_APPLICATION (self), entries);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);
  gtk_application_set_accels_for_action (GTK_APPLICATION (self),
                                         "app.quit",
                                         (const char *[]) { "<primary>q", NULL });
}
