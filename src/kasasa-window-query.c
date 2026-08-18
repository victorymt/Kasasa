/* kasasa-window-query.c
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

#include <json-glib/json-glib.h>
#include <glib/gi18n.h>
#include <string.h>

#include "kasasa-hyprctl.h"
#include "kasasa-window-model.h"
#include "kasasa-window-query.h"
#include "kasasa-window-resolver.h"

G_DEFINE_QUARK (kasasa-window-query-error-quark, kasasa_window_query_error)

/* Table output is written directly to a terminal. Replace control characters
 * so window metadata cannot inject cursor movement or additional rows. */
static gchar *
sanitize_terminal_field (const gchar *value)
{
  GString *sanitized;
  const gchar *cursor;

  if (value == NULL)
    return g_strdup ("");

  sanitized = g_string_new (NULL);
  cursor = value;
  while (*cursor != '\0')
    {
      gunichar character = g_utf8_get_char_validated (cursor, -1);

      if (character == (gunichar) -1 || character == (gunichar) -2)
        {
          g_string_append_c (sanitized, '?');
          cursor++;
        }
      else
        {
          if (g_unichar_iscntrl (character))
            g_string_append_c (sanitized, ' ');
          else
            g_string_append_unichar (sanitized, character);
          cursor = g_utf8_next_char (cursor);
        }
    }

  return g_string_free (sanitized, FALSE);
}

void
kasasa_window_spec_clear (KasasaWindowSpec *spec)
{
  if (spec == NULL)
    return;

  g_clear_pointer (&spec->value, g_free);
  spec->kind = KASASA_WINDOW_SPEC_BARE;
}

gboolean
kasasa_window_spec_parse (const gchar      *text,
                          KasasaWindowSpec *spec,
                          GError          **error)
{
  const gchar *colon;

  g_return_val_if_fail (spec != NULL, FALSE);

  kasasa_window_spec_clear (spec);

  if (text == NULL || *text == '\0')
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Window specifier is empty"));
      return FALSE;
    }

  if (g_strcmp0 (text, "active") == 0)
    {
      spec->kind = KASASA_WINDOW_SPEC_ACTIVE;
      return TRUE;
    }

  colon = strchr (text, ':');
  if (colon != NULL)
    {
      g_autofree gchar *prefix = g_strndup (text, colon - text);
      const gchar *value = colon + 1;

      if (*value == '\0')
        {
          g_set_error (error,
                       KASASA_WINDOW_QUERY_ERROR,
                       KASASA_WINDOW_QUERY_ERROR_FAILED,
                       _("Window specifier is missing a value after “%s:”"),
                       prefix);
          return FALSE;
        }

      if (g_strcmp0 (prefix, "class") == 0)
        {
          spec->kind = KASASA_WINDOW_SPEC_CLASS;
          spec->value = g_strdup (value);
          return TRUE;
        }
      if (g_strcmp0 (prefix, "title") == 0)
        {
          spec->kind = KASASA_WINDOW_SPEC_TITLE;
          spec->value = g_strdup (value);
          return TRUE;
        }
      if (g_strcmp0 (prefix, "address") == 0)
        {
          spec->kind = KASASA_WINDOW_SPEC_ADDRESS;
          spec->value = g_strdup (value);
          return TRUE;
        }

      g_set_error (error,
                   KASASA_WINDOW_QUERY_ERROR,
                   KASASA_WINDOW_QUERY_ERROR_FAILED,
                   _("Unknown window specifier prefix '%s:'"),
                   prefix);
      return FALSE;
    }

  if (g_str_has_prefix (text, "0x") || g_str_has_prefix (text, "0X"))
    {
      spec->kind = KASASA_WINDOW_SPEC_ADDRESS;
      spec->value = g_strdup (text);
      return TRUE;
    }

  spec->kind = KASASA_WINDOW_SPEC_BARE;
  spec->value = g_strdup (text);
  return TRUE;
}

void
kasasa_window_client_free (KasasaWindowClient *client)
{
  if (client == NULL)
    return;

  g_free (client->address);
  g_free (client->class_name);
  g_free (client->title);
  g_free (client->workspace_name);
  g_free (client);
}

void
kasasa_window_client_list_free (GPtrArray *clients)
{
  if (clients == NULL)
    return;

  g_ptr_array_free (clients, TRUE);
}

void
kasasa_monitor_free (KasasaMonitor *monitor)
{
  if (monitor == NULL)
    return;

  g_free (monitor->name);
  g_free (monitor->description);
  g_free (monitor);
}

void
kasasa_monitor_list_free (GPtrArray *monitors)
{
  if (monitors != NULL)
    g_ptr_array_free (monitors, TRUE);
}

static KasasaMonitor *
monitor_copy (const KasasaMonitor *src)
{
  KasasaMonitor *copy;

  if (src == NULL)
    return NULL;

  copy = g_new0 (KasasaMonitor, 1);
  *copy = *src;
  copy->name = g_strdup (src->name);
  copy->description = g_strdup (src->description);
  return copy;
}

KasasaWindowClient *
kasasa_window_client_copy (const KasasaWindowClient *src)
{
  KasasaWindowClient *copy;

  if (src == NULL)
    return NULL;

  copy = g_new0 (KasasaWindowClient, 1);
  copy->address = g_strdup (src->address);
  copy->class_name = g_strdup (src->class_name);
  copy->title = g_strdup (src->title);
  copy->workspace_id = src->workspace_id;
  copy->workspace_name = g_strdup (src->workspace_name);
  copy->x = src->x;
  copy->y = src->y;
  copy->width = src->width;
  copy->height = src->height;
  copy->monitor = src->monitor;
  copy->focus_history_id = src->focus_history_id;
  copy->mapped = src->mapped;
  copy->floating = src->floating;
  return copy;
}

GPtrArray *
kasasa_window_query_parse_clients_json (const gchar *json,
                                        GError     **error)
{
  return kasasa_window_model_parse_clients_json (json, error);
}

KasasaWindowClient *
kasasa_window_query_parse_active_json (const gchar *json,
                                       GError     **error)
{
  return kasasa_window_model_parse_active_json (json, error);
}

GPtrArray *
kasasa_monitor_query_parse_json (const gchar *json,
                                 GError     **error)
{
  return kasasa_window_model_parse_monitors_json (json, error);
}

static void
wrap_hyprctl_error (GError **error)
{
  g_autoptr (GError) wrapped = NULL;

  if (error == NULL || *error == NULL
      || (*error)->domain == KASASA_WINDOW_QUERY_ERROR)
    return;

  wrapped = g_error_new (KASASA_WINDOW_QUERY_ERROR,
                         KASASA_WINDOW_QUERY_ERROR_FAILED,
                         "%s", (*error)->message);
  g_clear_error (error);
  g_propagate_error (error, g_steal_pointer (&wrapped));
}

gboolean
kasasa_window_query_backend_available (void)
{
  return kasasa_hyprctl_available ();
}

GPtrArray *
kasasa_window_query_list_clients (GError **error)
{
  g_autofree gchar *json = NULL;
  const gchar *argv[] = { "hyprctl", "-j", "clients", NULL };

  if (!kasasa_window_query_backend_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland window listing requires hyprctl"));
      return NULL;
    }

  json = kasasa_hyprctl_query (argv, error);
  if (json == NULL)
    {
      wrap_hyprctl_error (error);
      return NULL;
    }

  return kasasa_window_query_parse_clients_json (json, error);
}

KasasaWindowClient *
kasasa_window_query_get_active (GError **error)
{
  g_autofree gchar *json = NULL;
  const gchar *argv[] = { "hyprctl", "-j", "activewindow", NULL };

  if (!kasasa_window_query_backend_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland active window selection requires hyprctl"));
      return NULL;
    }

  json = kasasa_hyprctl_query (argv, error);
  if (json == NULL)
    {
      wrap_hyprctl_error (error);
      return NULL;
    }

  return kasasa_window_query_parse_active_json (json, error);
}

GPtrArray *
kasasa_monitor_query_list (GError **error)
{
  g_autofree gchar *json = NULL;
  const gchar *argv[] = { "hyprctl", "-j", "monitors", NULL };

  if (!kasasa_window_query_backend_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _("Hyprland monitor listing requires hyprctl"));
      return NULL;
    }

  json = kasasa_hyprctl_query (argv, error);
  if (json == NULL)
    {
      wrap_hyprctl_error (error);
      return NULL;
    }

  return kasasa_monitor_query_parse_json (json, error);
}

KasasaMonitor *
kasasa_monitor_query_resolve (GPtrArray   *monitors,
                              const gchar *spec,
                              GError     **error)
{
  guint i;

  g_return_val_if_fail (monitors != NULL, NULL);

  if (spec == NULL || *spec == '\0')
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Monitor specifier is empty"));
      return NULL;
    }

  if (g_strcmp0 (spec, "active") == 0)
    {
      for (i = 0; i < monitors->len; i++)
        {
          KasasaMonitor *monitor = g_ptr_array_index (monitors, i);

          if (monitor->focused)
            return monitor_copy (monitor);
        }

      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
                           _("No active monitor"));
      return NULL;
    }

  for (i = 0; i < monitors->len; i++)
    {
      KasasaMonitor *monitor = g_ptr_array_index (monitors, i);

      if (g_strcmp0 (monitor->name, spec) == 0)
        return monitor_copy (monitor);
    }

  g_set_error (error,
               KASASA_WINDOW_QUERY_ERROR,
               KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
               _("No monitor named “%s”"),
               spec);
  return NULL;
}

KasasaMonitor *
kasasa_monitor_query_resolve_live (const gchar *spec,
                                   GError     **error)
{
  g_autoptr (GPtrArray) monitors = kasasa_monitor_query_list (error);

  if (monitors == NULL)
    return NULL;

  return kasasa_monitor_query_resolve (monitors, spec, error);
}

KasasaWindowClient *
kasasa_window_query_resolve (const KasasaWindowSpec   *spec,
                             GPtrArray                *clients,
                             const KasasaWindowClient *active,
                             GPtrArray               **candidates,
                             GError                  **error)
{
  return kasasa_window_resolver_resolve (spec,
                                         clients,
                                         active,
                                         candidates,
                                         error);
}

KasasaWindowClient *
kasasa_window_query_resolve_live (const gchar *spec_text,
                                  GError     **error)
{
  KasasaWindowSpec spec = { 0 };
  g_autoptr (GPtrArray) clients = NULL;
  g_autoptr (KasasaWindowClient) active = NULL;
  g_autoptr (GPtrArray) candidates = NULL;
  KasasaWindowClient *resolved;

  if (!kasasa_window_spec_parse (spec_text, &spec, error))
    return NULL;

  if (spec.kind == KASASA_WINDOW_SPEC_ACTIVE)
    {
      active = kasasa_window_query_get_active (error);
      if (active == NULL && (error == NULL || *error == NULL))
        g_set_error_literal (error,
                             KASASA_WINDOW_QUERY_ERROR,
                             KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
                             _("No active window"));
      kasasa_window_spec_clear (&spec);
      return g_steal_pointer (&active);
    }

  clients = kasasa_window_query_list_clients (error);
  if (clients == NULL)
    {
      kasasa_window_spec_clear (&spec);
      return NULL;
    }

  resolved = kasasa_window_query_resolve (&spec, clients, active,
                                          &candidates, error);
  if (resolved == NULL && candidates != NULL && error != NULL && *error != NULL)
    {
      g_autofree gchar *list = kasasa_window_query_format_candidates (candidates);
      g_prefix_error (error, "%s\n", list);
    }

  kasasa_window_spec_clear (&spec);
  return resolved;
}

gchar *
kasasa_window_query_format_table (GPtrArray *clients)
{
  GString *out;
  guint i;

  g_return_val_if_fail (clients != NULL, NULL);

  out = g_string_new (NULL);
  g_string_append_printf (out,
                          "%-18s  %-16s  %-8s  %s\n",
                          "ADDRESS", "CLASS", "WORKSPACE", "TITLE");

  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);
      g_autofree gchar *address = sanitize_terminal_field (client->address);
      g_autofree gchar *class_name =
        sanitize_terminal_field (client->class_name);
      g_autofree gchar *workspace_name =
        sanitize_terminal_field (client->workspace_name);
      g_autofree gchar *title = sanitize_terminal_field (client->title);

      g_string_append_printf (out,
                              "%-18s  %-16s  %-8s  %s\n",
                              address,
                              class_name,
                              workspace_name,
                              title);
    }

  return g_string_free (out, FALSE);
}

gchar *
kasasa_window_query_format_json (GPtrArray *clients)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  guint i;

  g_return_val_if_fail (clients != NULL, NULL);

  json_builder_begin_array (builder);
  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "address");
      json_builder_add_string_value (builder, client->address ? client->address : "");
      json_builder_set_member_name (builder, "class");
      json_builder_add_string_value (builder, client->class_name ? client->class_name : "");
      json_builder_set_member_name (builder, "title");
      json_builder_add_string_value (builder, client->title ? client->title : "");
      json_builder_set_member_name (builder, "workspace");
      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "id");
      json_builder_add_int_value (builder, client->workspace_id);
      json_builder_set_member_name (builder, "name");
      json_builder_add_string_value (builder,
                                     client->workspace_name ? client->workspace_name : "");
      json_builder_end_object (builder);
      json_builder_set_member_name (builder, "at");
      json_builder_begin_array (builder);
      json_builder_add_int_value (builder, client->x);
      json_builder_add_int_value (builder, client->y);
      json_builder_end_array (builder);
      json_builder_set_member_name (builder, "size");
      json_builder_begin_array (builder);
      json_builder_add_int_value (builder, client->width);
      json_builder_add_int_value (builder, client->height);
      json_builder_end_array (builder);
      json_builder_set_member_name (builder, "monitor");
      json_builder_add_int_value (builder, client->monitor);
      json_builder_set_member_name (builder, "floating");
      json_builder_add_boolean_value (builder, client->floating);
      json_builder_end_object (builder);
    }
  json_builder_end_array (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  return json_generator_to_data (generator, NULL);
}

gchar *
kasasa_window_query_format_candidates (GPtrArray *clients)
{
  return kasasa_window_resolver_format_candidates (clients);
}

gchar *
kasasa_monitor_query_format_table (GPtrArray *monitors)
{
  GString *out;
  guint i;

  g_return_val_if_fail (monitors != NULL, NULL);

  out = g_string_new (NULL);
  g_string_append_printf (out,
                          "%-12s  %-11s  %-7s  %s\n",
                          "NAME", "SIZE", "SCALE", "DESCRIPTION");

  for (i = 0; i < monitors->len; i++)
    {
      KasasaMonitor *monitor = g_ptr_array_index (monitors, i);
      g_autofree gchar *size = g_strdup_printf ("%dx%d",
                                                monitor->width,
                                                monitor->height);
      g_autofree gchar *name = sanitize_terminal_field (monitor->name);
      g_autofree gchar *description =
        sanitize_terminal_field (monitor->description);

      g_string_append_printf (out,
                              "%-12s  %-11s  %-7.2f  %s%s\n",
                              name,
                              size,
                              monitor->scale,
                              description,
                              monitor->focused ? " *" : "");
    }

  return g_string_free (out, FALSE);
}

gchar *
kasasa_monitor_query_format_json (GPtrArray *monitors)
{
  g_autoptr (JsonBuilder) builder = json_builder_new ();
  g_autoptr (JsonGenerator) generator = json_generator_new ();
  g_autoptr (JsonNode) root = NULL;
  guint i;

  g_return_val_if_fail (monitors != NULL, NULL);

  json_builder_begin_array (builder);
  for (i = 0; i < monitors->len; i++)
    {
      KasasaMonitor *monitor = g_ptr_array_index (monitors, i);

      json_builder_begin_object (builder);
      json_builder_set_member_name (builder, "id");
      json_builder_add_int_value (builder, monitor->id);
      json_builder_set_member_name (builder, "name");
      json_builder_add_string_value (builder, monitor->name);
      json_builder_set_member_name (builder, "description");
      json_builder_add_string_value (builder, monitor->description);
      json_builder_set_member_name (builder, "width");
      json_builder_add_int_value (builder, monitor->width);
      json_builder_set_member_name (builder, "height");
      json_builder_add_int_value (builder, monitor->height);
      json_builder_set_member_name (builder, "scale");
      json_builder_add_double_value (builder, monitor->scale);
      json_builder_set_member_name (builder, "transform");
      json_builder_add_int_value (builder, monitor->transform);
      json_builder_set_member_name (builder, "focused");
      json_builder_add_boolean_value (builder, monitor->focused);
      json_builder_end_object (builder);
    }
  json_builder_end_array (builder);

  root = json_builder_get_root (builder);
  json_generator_set_root (generator, root);
  json_generator_set_pretty (generator, TRUE);
  return json_generator_to_data (generator, NULL);
}
