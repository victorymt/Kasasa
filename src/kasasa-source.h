/* kasasa-source.h
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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  guint id;
} KasasaSource;

void kasasa_source_clear (KasasaSource *source);
void kasasa_source_set_timeout_once (KasasaSource   *source,
                                     guint           interval_ms,
                                     GSourceOnceFunc callback,
                                     gpointer        user_data);
void kasasa_source_set_timeout_seconds_once (KasasaSource   *source,
                                             guint           interval_seconds,
                                             GSourceOnceFunc callback,
                                             gpointer        user_data);

G_END_DECLS
