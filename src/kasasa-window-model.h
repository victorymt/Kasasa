/* kasasa-window-model.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "kasasa-window-query.h"

G_BEGIN_DECLS

GPtrArray *kasasa_window_model_parse_clients_json (const gchar *json,
                                                   GError     **error);
KasasaWindowClient *kasasa_window_model_parse_active_json (const gchar *json,
                                                           GError      **error);
GPtrArray *kasasa_window_model_parse_monitors_json (const gchar *json,
                                                    GError     **error);

G_END_DECLS
