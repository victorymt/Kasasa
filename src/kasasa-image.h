/* kasasa-image.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gdk/gdk.h>

G_BEGIN_DECLS

gboolean kasasa_image_load_uri (const gchar  *uri,
                                GFile       **file,
                                GdkTexture  **texture,
                                GError      **error);

G_END_DECLS
