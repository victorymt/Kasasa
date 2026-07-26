/* kasasa-image.c
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
