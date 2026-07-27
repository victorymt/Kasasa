/* kasasa-window-query.h
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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  KASASA_WINDOW_QUERY_ERROR_FAILED = 0,
  KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
  KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
  KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS,
} KasasaWindowQueryError;

#define KASASA_WINDOW_QUERY_ERROR (kasasa_window_query_error_quark ())
GQuark kasasa_window_query_error_quark (void);

typedef enum
{
  KASASA_WINDOW_SPEC_ACTIVE,
  KASASA_WINDOW_SPEC_ADDRESS,
  KASASA_WINDOW_SPEC_CLASS,
  KASASA_WINDOW_SPEC_TITLE,
  KASASA_WINDOW_SPEC_BARE,
} KasasaWindowSpecKind;

typedef struct
{
  KasasaWindowSpecKind kind;
  gchar *value;
} KasasaWindowSpec;

typedef struct
{
  gchar *address;
  gchar *class_name;
  gchar *title;
  gint workspace_id;
  gchar *workspace_name;
  gint x;
  gint y;
  gint width;
  gint height;
  gint monitor;
  gboolean mapped;
  gboolean floating;
} KasasaWindowClient;

void kasasa_window_spec_clear (KasasaWindowSpec *spec);
gboolean kasasa_window_spec_parse (const gchar      *text,
                                   KasasaWindowSpec *spec,
                                   GError          **error);

void kasasa_window_client_free (KasasaWindowClient *client);
void kasasa_window_client_list_free (GPtrArray *clients);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (KasasaWindowClient, kasasa_window_client_free)

/* Parse hyprctl clients -j JSON into KasasaWindowClient objects. */
GPtrArray *kasasa_window_query_parse_clients_json (const gchar *json,
                                                   GError     **error);

/* Parse hyprctl activewindow -j into a single client (or NULL). */
KasasaWindowClient *kasasa_window_query_parse_active_json (const gchar *json,
                                                           GError     **error);

gboolean kasasa_window_query_backend_available (void);

GPtrArray *kasasa_window_query_list_clients (GError **error);
KasasaWindowClient *kasasa_window_query_get_active (GError **error);

/*
 * Resolve a SPEC against a client list.
 * On success returns a newly allocated KasasaWindowClient copy.
 * On ambiguous match, sets KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS and may set
 * *candidates to a list of matching clients (caller frees with list free).
 */
KasasaWindowClient *kasasa_window_query_resolve (const KasasaWindowSpec *spec,
                                                 GPtrArray              *clients,
                                                 const KasasaWindowClient *active,
                                                 GPtrArray             **candidates,
                                                 GError                **error);

/* Live resolve: query hyprctl, then match. */
KasasaWindowClient *kasasa_window_query_resolve_live (const gchar *spec_text,
                                                      GError     **error);

gchar *kasasa_window_query_format_table (GPtrArray *clients);
gchar *kasasa_window_query_format_json (GPtrArray *clients);
gchar *kasasa_window_query_format_candidates (GPtrArray *clients);

G_END_DECLS
