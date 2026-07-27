/* kasasa-hyprland-capture.c
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

#include "config.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "kasasa-hyprland-capture.h"
#include "kasasa-window-query.h"

gboolean
kasasa_hyprland_capture_available (void)
{
  g_autofree gchar *path = g_find_program_in_path ("grim");
  return path != NULL && kasasa_window_query_backend_available ();
}

gchar *
kasasa_hyprland_capture_screenshot (const KasasaWindowClient *client,
                                    GError                  **error)
{
  g_autofree gchar *geometry = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *stderr_buf = NULL;
  gint fd = -1;
  gint status = 0;
  gchar *argv[] = {
    (gchar *) "grim",
    (gchar *) "-g",
    NULL, /* geometry */
    NULL, /* path */
    NULL,
  };

  g_return_val_if_fail (client != NULL, NULL);

  if (!kasasa_hyprland_capture_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland capture requires hyprctl and grim"));
      return NULL;
    }

  if (client->width <= 0 || client->height <= 0)
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Window has an invalid size"));
      return NULL;
    }

  fd = g_file_open_tmp ("kasasa-capture-XXXXXX.png", &path, error);
  if (fd < 0)
    return NULL;
  close (fd);
  g_unlink (path);

  geometry = g_strdup_printf ("%d,%d %dx%d",
                              client->x,
                              client->y,
                              client->width,
                              client->height);
  argv[2] = geometry;
  argv[3] = path;

  if (!g_spawn_sync (NULL,
                     argv,
                     NULL,
                     G_SPAWN_SEARCH_PATH,
                     NULL,
                     NULL,
                     NULL,
                     &stderr_buf,
                     &status,
                     error))
    return NULL;

  if (!g_spawn_check_wait_status (status, error))
    {
      if (stderr_buf != NULL && *stderr_buf != '\0')
        g_prefix_error (error, "%s: ", g_strstrip (stderr_buf));
      g_unlink (path);
      return NULL;
    }

  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("grim did not produce a screenshot file"));
      return NULL;
    }

  return g_filename_to_uri (path, NULL, error);
}
