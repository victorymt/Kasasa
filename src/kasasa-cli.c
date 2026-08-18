/* kasasa-cli.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib/gi18n.h>

#include "kasasa-cli.h"
#include "kasasa-window-query.h"

int
kasasa_cli_query_error_to_exit_code (const GError *error)
{
  if (error == NULL)
    return KASASA_CLI_EXIT_ERROR;

  if (g_error_matches (error,
                       KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE))
    return KASASA_CLI_EXIT_UNAVAILABLE;

  if (g_error_matches (error,
                       KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_NO_MATCH)
      || g_error_matches (error,
                          KASASA_WINDOW_QUERY_ERROR,
                          KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS))
    return KASASA_CLI_EXIT_MATCH;

  return KASASA_CLI_EXIT_ERROR;
}

int
kasasa_cli_run_list_windows (gboolean as_json)
{
  g_autoptr (GPtrArray) clients = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *text = NULL;

  clients = kasasa_window_query_list_clients (&error);
  if (clients == NULL)
    {
      g_printerr ("%s\n",
                  error != NULL ? error->message : _ ("Failed to list windows"));
      return kasasa_cli_query_error_to_exit_code (error);
    }

  text = as_json
         ? kasasa_window_query_format_json (clients)
         : kasasa_window_query_format_table (clients);
  g_print ("%s", text);
  if (as_json)
    g_print ("\n");

  return KASASA_CLI_EXIT_OK;
}

int
kasasa_cli_run_list_monitors (gboolean as_json)
{
  g_autoptr (GPtrArray) monitors = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *text = NULL;

  monitors = kasasa_monitor_query_list (&error);
  if (monitors == NULL)
    {
      g_printerr ("%s\n",
                  error != NULL ? error->message : _ ("Failed to list monitors"));
      return kasasa_cli_query_error_to_exit_code (error);
    }

  text = as_json
         ? kasasa_monitor_query_format_json (monitors)
         : kasasa_monitor_query_format_table (monitors);
  g_print ("%s", text);
  if (as_json)
    g_print ("\n");

  return KASASA_CLI_EXIT_OK;
}

const gchar *
kasasa_cli_validate_options (gboolean     screencast,
                              gboolean     list_windows,
                              gboolean     list_monitors,
                              gboolean     list_json,
                              const gchar *window_spec,
                              const gchar *monitor_spec)
{
  if (list_windows && list_monitors)
    return _ ("--list-windows and --list-monitors are mutually exclusive");

  if (list_json && !list_windows && !list_monitors)
    return _ ("--json requires --list-windows or --list-monitors");

  if (window_spec != NULL && monitor_spec != NULL)
    return _ ("--window and --monitor are mutually exclusive");

  if (monitor_spec != NULL && !screencast)
    return _ ("--monitor requires --screencast");

  if ((list_windows || list_monitors)
      && (window_spec != NULL || monitor_spec != NULL))
    return _ ("Listing options cannot be combined with a capture target");

  return NULL;
}
