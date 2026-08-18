/* kasasa-cli.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  KASASA_CLI_EXIT_OK = 0,
  KASASA_CLI_EXIT_ERROR = 1,
  KASASA_CLI_EXIT_MATCH = 2,
  KASASA_CLI_EXIT_UNAVAILABLE = 3,
} KasasaCliExitCode;

int kasasa_cli_query_error_to_exit_code (const GError *error);
int kasasa_cli_run_list_windows (gboolean as_json);
int kasasa_cli_run_list_monitors (gboolean as_json);
const gchar *kasasa_cli_validate_options (gboolean     screencast,
                                          gboolean     list_windows,
                                          gboolean     list_monitors,
                                          gboolean     list_json,
                                          const gchar *window_spec,
                                          const gchar *monitor_spec);

G_END_DECLS
