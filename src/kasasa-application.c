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
#include <gst/gst.h>
#include <stdio.h>

#include "kasasa-application.h"
#include "kasasa-cli.h"
#include "kasasa-hyprland-capture.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-preferences.h"
#include "kasasa-window-query.h"
#include "kasasa-window.h"

struct _KasasaApplication
{
  AdwApplication parent_instance;

  gboolean start_with_screencast;
  gboolean list_windows;
  gboolean list_monitors;
  gboolean list_json;
  gchar   *window_spec;
  gchar   *monitor_spec;
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

static GtkWindow *
ensure_pin_window (KasasaApplication *self)
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
  kasasa_window_begin_initial_reveal (KASASA_WINDOW (window));
  gtk_window_present (window);

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
      return kasasa_cli_query_error_to_exit_code (error);
    }

  if (screencast)
    {
      guint32 handle = 0;

      if (!kasasa_hyprland_stream_available ())
        {
          g_printerr ("%s\n",
                      _("Targeted live screencast requires Hyprland Wayland"));
          return KASASA_CLI_EXIT_UNAVAILABLE;
        }

      if (!kasasa_hyprland_stream_handle_from_address (client->address,
                                                       &handle,
                                                       &error))
        {
          g_printerr ("%s\n",
                      error != NULL ? error->message : _("Invalid window address"));
          return kasasa_cli_query_error_to_exit_code (error);
        }

      window = ensure_pin_window (self);
      if (!KASASA_IS_WINDOW (window))
        return KASASA_CLI_EXIT_ERROR;

      kasasa_window_load_first_hyprland_screencast (KASASA_WINDOW (window),
                                                    handle,
                                                    client->width,
                                                    client->height);
      return KASASA_CLI_EXIT_OK;
    }
  else
    {
      g_autofree gchar *uri = NULL;

      uri = kasasa_hyprland_capture_screenshot (client, &error);
      if (uri == NULL)
        {
          g_printerr ("%s\n",
                      error != NULL ? error->message : _("Failed to capture window"));
          return kasasa_cli_query_error_to_exit_code (error);
        }

      window = ensure_pin_window (self);
      if (!KASASA_IS_WINDOW (window))
        return KASASA_CLI_EXIT_ERROR;

      kasasa_window_load_first_screenshot_uri (KASASA_WINDOW (window), uri);
      return KASASA_CLI_EXIT_OK;
    }
}

static int
present_monitor_capture (KasasaApplication *self,
                         const gchar       *monitor_spec)
{
  g_autoptr (KasasaMonitor) monitor = NULL;
  g_autoptr (GError) error = NULL;
  GtkWindow *window;

  monitor = kasasa_monitor_query_resolve_live (monitor_spec, &error);
  if (monitor == NULL)
    {
      g_printerr ("%s\n",
                  error != NULL ? error->message : _("Failed to resolve monitor"));
      return kasasa_cli_query_error_to_exit_code (error);
    }

  if (!kasasa_hyprland_stream_available ())
    {
      g_printerr ("%s\n",
                  _("Targeted monitor screencast requires Hyprland Wayland"));
      return KASASA_CLI_EXIT_UNAVAILABLE;
    }

  window = ensure_pin_window (self);
  if (!KASASA_IS_WINDOW (window))
    return KASASA_CLI_EXIT_ERROR;

  kasasa_window_load_first_hyprland_monitor_screencast (
    KASASA_WINDOW (window),
    monitor->name,
    monitor->width,
    monitor->height);
  return KASASA_CLI_EXIT_OK;
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

  kasasa_window_begin_initial_reveal (KASASA_WINDOW (window));
  gtk_window_present (window);

  if (start_with_screencast)
    kasasa_window_take_first_screencast (KASASA_WINDOW (window));
  else
    kasasa_window_take_first_screenshot (KASASA_WINDOW (window));
}

static void
kasasa_application_activate (GApplication *app)
{
  KasasaApplication *self = KASASA_APPLICATION (app);

  g_assert (KASASA_IS_APPLICATION (app));

  if (self->list_windows || self->list_monitors)
    {
      int code = self->list_windows
                 ? kasasa_cli_run_list_windows (self->list_json)
                 : kasasa_cli_run_list_monitors (self->list_json);
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

  if (self->monitor_spec != NULL)
    {
      present_monitor_capture (self, self->monitor_spec);
      return;
    }

  present_or_create_window (self, self->start_with_screencast);
  self->start_with_screencast = FALSE;
}

static gchar *kasasa_application_diagnostics_text (void);

static gint
kasasa_application_handle_local_options (GApplication *app,
                                         GVariantDict *options)
{
  KasasaApplication *self = KASASA_APPLICATION (app);
  const gchar *window_spec = NULL;
  const gchar *monitor_spec = NULL;
  const gchar *validation_error;

  self->start_with_screencast = g_variant_dict_contains (options, "screencast");
  self->list_windows = g_variant_dict_contains (options, "list-windows");
  self->list_monitors = g_variant_dict_contains (options, "list-monitors");
  self->list_json = g_variant_dict_contains (options, "json");
  g_clear_pointer (&self->window_spec, g_free);
  g_clear_pointer (&self->monitor_spec, g_free);

  if (g_variant_dict_contains (options, "diagnostics"))
    {
      if (self->start_with_screencast || self->list_windows
          || self->list_monitors || self->list_json
          || g_variant_dict_contains (options, "window")
          || g_variant_dict_contains (options, "monitor"))
        {
          g_printerr ("--diagnostics cannot be combined with capture or listing options\n");
          return KASASA_CLI_EXIT_ERROR;
        }

      {
        g_autofree gchar *text = kasasa_application_diagnostics_text ();
        g_print ("%s", text);
      }
      return KASASA_CLI_EXIT_OK;
    }

  if (g_variant_dict_lookup (options, "window", "&s", &window_spec)
      && window_spec != NULL)
    self->window_spec = g_strdup (window_spec);

  if (g_variant_dict_lookup (options, "monitor", "&s", &monitor_spec)
      && monitor_spec != NULL)
    self->monitor_spec = g_strdup (monitor_spec);

  validation_error = kasasa_cli_validate_options (self->start_with_screencast,
                                                  self->list_windows,
                                                  self->list_monitors,
                                                  self->list_json,
                                                  self->window_spec,
                                                  self->monitor_spec);
  if (validation_error != NULL)
    {
      g_printerr ("%s\n", validation_error);
      return KASASA_CLI_EXIT_ERROR;
    }

  /* Pure listing: no remote activation / no GUI. */
  if (self->list_windows)
    return kasasa_cli_run_list_windows (self->list_json);
  if (self->list_monitors)
    return kasasa_cli_run_list_monitors (self->list_json);

  return -1;
}

static gchar *
kasasa_application_diagnostics_text (void)
{
  static const gchar *programs[] = { "hyprctl", "grim", "slurp", NULL };
  const gchar *wayland_display;
  const gchar *desktop;
  const gchar *hyprland_signature;
  const gchar *x_display;
  const gchar *disable_dmabuf;
  g_autoptr (GstElementFactory) paintable_sink = NULL;
  guint gst_major, gst_minor, gst_micro, gst_nano;
  GString *text;
  guint i;

  wayland_display = g_getenv ("WAYLAND_DISPLAY");
  desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  hyprland_signature = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");
  x_display = g_getenv ("DISPLAY");
  disable_dmabuf = g_getenv ("KASASA_DISABLE_DMABUF");
  gst_version (&gst_major, &gst_minor, &gst_micro, &gst_nano);
  paintable_sink = gst_element_factory_find ("gtk4paintablesink");

  text = g_string_new (NULL);
  g_string_append_printf (text,
                          "Kasasa %s\n"
                          "Build environment:\n"
                          "  source: meson\n"
                          "  wayland-protocols: >= 1.35\n"
                          "Runtime environment:\n"
                          "  WAYLAND_DISPLAY: %s\n"
                          "  XDG_CURRENT_DESKTOP: %s\n"
                          "  HYPRLAND_INSTANCE_SIGNATURE: %s\n"
                          "  DISPLAY: %s\n"
                          "  KASASA_DISABLE_DMABUF: %s\n"
                          "GStreamer: %u.%u.%u.%u\n"
                          "  gtk4paintablesink: %s\n",
                          PACKAGE_VERSION,
                          wayland_display != NULL ? wayland_display : "(unset)",
                          desktop != NULL ? desktop : "(unset)",
                          hyprland_signature != NULL ? "set" : "(unset)",
                          x_display != NULL ? x_display : "(unset)",
                          disable_dmabuf != NULL ? disable_dmabuf : "(unset)",
                          gst_major,
                          gst_minor,
                          gst_micro,
                          gst_nano,
                          paintable_sink != NULL ? "available" : "missing");

  g_string_append (text, "External tools:\n");
  for (i = 0; programs[i] != NULL; i++)
    {
      g_autofree gchar *path = g_find_program_in_path (programs[i]);

      g_string_append_printf (text,
                              "  %s: %s\n",
                              programs[i],
                              path != NULL ? path : "missing");
    }

#ifdef KASASA_HAVE_LAYER_SHELL
  g_string_append (text, "GTK layer shell: compiled in\n");
#else
  g_string_append (text, "GTK layer shell: not compiled in\n");
#endif

  return g_string_free (text, FALSE);
}

static int
kasasa_application_print_diagnostics (GApplicationCommandLine *cmdline)
{
  g_autofree gchar *text = kasasa_application_diagnostics_text ();

  g_application_command_line_print (cmdline, "%s", text);
  return KASASA_CLI_EXIT_OK;
}

static int
kasasa_application_command_line (GApplication            *app,
                                 GApplicationCommandLine *cmdline)
{
  KasasaApplication *self = KASASA_APPLICATION (app);
  GVariantDict *options = g_application_command_line_get_options_dict (cmdline);
  const gchar *window_spec = NULL;
  const gchar *monitor_spec = NULL;
  const gchar *validation_error;
  gboolean start_with_screencast;
  gboolean list_windows;
  gboolean list_monitors;
  gboolean list_json;
  gboolean diagnostics;

  start_with_screencast = g_variant_dict_contains (options, "screencast");
  list_windows = g_variant_dict_contains (options, "list-windows");
  list_monitors = g_variant_dict_contains (options, "list-monitors");
  list_json = g_variant_dict_contains (options, "json");
  diagnostics = g_variant_dict_contains (options, "diagnostics");

  if (diagnostics)
    {
      if (start_with_screencast || list_windows || list_monitors || list_json
          || g_variant_dict_contains (options, "window")
          || g_variant_dict_contains (options, "monitor"))
        {
          g_application_command_line_printerr (
            cmdline,
            "--diagnostics cannot be combined with capture or listing options\n");
          return KASASA_CLI_EXIT_ERROR;
        }

      return kasasa_application_print_diagnostics (cmdline);
    }

  g_variant_dict_lookup (options, "window", "&s", &window_spec);
  g_variant_dict_lookup (options, "monitor", "&s", &monitor_spec);

  validation_error = kasasa_cli_validate_options (start_with_screencast,
                                                  list_windows,
                                                  list_monitors,
                                                  list_json,
                                                  window_spec,
                                                  monitor_spec);
  if (validation_error != NULL)
    {
      g_application_command_line_printerr (cmdline,
                                           "%s\n",
                                           validation_error);
      return KASASA_CLI_EXIT_ERROR;
    }

  if (list_windows)
    return kasasa_cli_run_list_windows (list_json);
  if (list_monitors)
    return kasasa_cli_run_list_monitors (list_json);

  if (window_spec != NULL)
    {
      return present_targeted_capture (self, window_spec, start_with_screencast);
    }

  if (monitor_spec != NULL)
    return present_monitor_capture (self, monitor_spec);

  present_or_create_window (self, start_with_screencast);
  return KASASA_CLI_EXIT_OK;
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
  g_clear_pointer (&self->monitor_spec, g_free);

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
      .description = _("With a listing option, print JSON"),
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
    {
      .long_name = "list-monitors",
      .short_name = 0,
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_NONE,
      .arg_data = NULL,
      .description = _("List capturable monitors (Hyprland) and exit"),
      .arg_description = NULL,
    },
    {
      .long_name = "diagnostics",
      .short_name = 0,
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_NONE,
      .arg_data = NULL,
      .description = _("Print runtime diagnostics and exit"),
      .arg_description = NULL,
    },
    {
      .long_name = "monitor",
      .short_name = 'm',
      .flags = G_OPTION_FLAG_NONE,
      .arg = G_OPTION_ARG_STRING,
      .arg_data = NULL,
      .description = _("Capture a monitor live: output name or active"),
      .arg_description = _("NAME"),
    },
    { NULL }
  };

  self->start_with_screencast = FALSE;
  self->list_windows = FALSE;
  self->list_monitors = FALSE;
  self->list_json = FALSE;
  self->window_spec = NULL;
  self->monitor_spec = NULL;

  g_application_set_option_context_summary (
    G_APPLICATION (self),
    _("Pin screenshots and screencasts above other windows"));
  g_application_set_option_context_description (
    G_APPLICATION (self),
    _("Capture modes:\n"
      "  kasasa                              Select a screen region to capture\n"
      "  kasasa --screencast                 Select a Hyprland window to preview live\n"
      "  kasasa --window=active              Capture the active Hyprland window\n"
      "  kasasa --screencast --monitor=active\n"
      "                                      Capture the active Hyprland monitor live\n"
      "\n"
      "Use --list-windows or --list-monitors to inspect Hyprland capture targets.\n"
      "Native monitor capture requires --monitor together with --screencast."));

  g_application_add_main_option_entries (G_APPLICATION (self), entries);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   app_actions,
                                   G_N_ELEMENTS (app_actions),
                                   self);
  gtk_application_set_accels_for_action (GTK_APPLICATION (self),
                                         "app.quit",
                                         (const char *[]) { "<primary>q", NULL });
}
