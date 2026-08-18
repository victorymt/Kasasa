/* kasasa-hyprctl.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-hyprctl.h"

gboolean
kasasa_hyprctl_available (void)
{
  g_autofree gchar *hyprctl = NULL;
  const gchar *instance_signature = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");

  hyprctl = g_find_program_in_path ("hyprctl");
  return hyprctl != NULL
         && instance_signature != NULL
         && *instance_signature != '\0';
}

gchar *
kasasa_hyprctl_query (const gchar *const *argv,
                      GError          **error)
{
  g_autofree gchar *stdout_buf = NULL;
  g_autofree gchar *stderr_buf = NULL;
  gint status = 0;

  g_return_val_if_fail (argv != NULL && argv[0] != NULL, NULL);

  if (!g_spawn_sync (NULL,
                     (gchar **) argv,
                     NULL,
                     G_SPAWN_SEARCH_PATH,
                     NULL,
                     NULL,
                     &stdout_buf,
                     &stderr_buf,
                     &status,
                     error))
    return NULL;

  if (!g_spawn_check_wait_status (status, error))
    {
      if (stderr_buf != NULL && *stderr_buf != '\0')
        g_prefix_error (error, "%s: ", g_strstrip (stderr_buf));
      return NULL;
    }

  return g_steal_pointer (&stdout_buf);
}
