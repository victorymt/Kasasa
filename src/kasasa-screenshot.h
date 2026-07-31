/* kasasa-screenshot.h
 *
 * Copyright 2024-2025 Kelvin Novais
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

#pragma once

#include <adwaita.h>

#include "kasasa-content.h"

G_BEGIN_DECLS

#define KASASA_TYPE_SCREENSHOT (kasasa_screenshot_get_type ())

G_DECLARE_FINAL_TYPE (KasasaScreenshot, kasasa_screenshot, KASASA, SCREENSHOT, AdwBin)

typedef enum
{
  KASASA_SCREENSHOT_SOURCE_REGION,
  KASASA_SCREENSHOT_SOURCE_WINDOW,
} KasasaScreenshotSource;

KasasaScreenshot *kasasa_screenshot_new (void);
GFile *kasasa_screenshot_get_file (KasasaScreenshot *screenshot);
gboolean kasasa_screenshot_load_screenshot (KasasaScreenshot *screenshot,
                                            const gchar      *uri,
                                            GError          **error);
void kasasa_screenshot_set_source (KasasaScreenshot       *screenshot,
                                   KasasaScreenshotSource  source);
KasasaScreenshotSource kasasa_screenshot_get_source (
  KasasaScreenshot *screenshot);

G_END_DECLS
