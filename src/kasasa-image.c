/* kasasa-image.c
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

#include "kasasa-image.h"

gboolean
kasasa_image_load_uri (const gchar  *uri,
                       GFile       **file,
                       GdkTexture  **texture,
                       GError      **error)
{
  g_autoptr (GFile) candidate_file = NULL;
  g_autoptr (GdkTexture) candidate_texture = NULL;

  g_return_val_if_fail (uri != NULL, FALSE);
  g_return_val_if_fail (file != NULL, FALSE);
  g_return_val_if_fail (texture != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  candidate_file = g_file_new_for_uri (uri);
  candidate_texture = gdk_texture_new_from_file (candidate_file, error);
  if (candidate_texture == NULL)
    return FALSE;

  *file = g_steal_pointer (&candidate_file);
  *texture = g_steal_pointer (&candidate_texture);

  return TRUE;
}
