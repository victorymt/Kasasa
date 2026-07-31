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

#include "kasasa-window-query.h"

G_DEFINE_QUARK (kasasa-window-query-error-quark, kasasa_window_query_error)

static gboolean
is_kasasa_client (const KasasaWindowClient *client)
{
  if (client == NULL || client->class_name == NULL)
    return FALSE;

  return g_strcmp0 (client->class_name, "io.github.kelvinnovais.Kasasa") == 0
         || g_strcmp0 (client->class_name, "kasasa") == 0
         || g_strcmp0 (client->class_name, "Kasasa") == 0;
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
  if (colon != NULL && colon != text)
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

static KasasaWindowClient *
client_from_json_object (JsonObject *object)
{
  KasasaWindowClient *client;
  JsonObject *workspace = NULL;
  JsonArray *at = NULL;
  JsonArray *size = NULL;

  client = g_new0 (KasasaWindowClient, 1);
  client->address = g_strdup (json_object_get_string_member_with_default (object, "address", ""));
  client->class_name = g_strdup (json_object_get_string_member_with_default (object, "class", ""));
  client->title = g_strdup (json_object_get_string_member_with_default (object, "title", ""));
  client->mapped = json_object_get_boolean_member_with_default (object, "mapped", FALSE);
  client->floating = json_object_get_boolean_member_with_default (object, "floating", FALSE);
  client->monitor = json_object_get_int_member_with_default (object, "monitor", 0);
  client->focus_history_id = json_object_get_int_member_with_default (
    object, "focusHistoryID", G_MAXINT);

  if (json_object_has_member (object, "workspace")
      && JSON_NODE_HOLDS_OBJECT (json_object_get_member (object, "workspace")))
    {
      workspace = json_object_get_object_member (object, "workspace");
      client->workspace_id = json_object_get_int_member_with_default (workspace, "id", 0);
      client->workspace_name = g_strdup (json_object_get_string_member_with_default (workspace, "name", ""));
    }
  else
    {
      client->workspace_name = g_strdup ("");
    }

  if (json_object_has_member (object, "at")
      && JSON_NODE_HOLDS_ARRAY (json_object_get_member (object, "at")))
    {
      at = json_object_get_array_member (object, "at");
      if (json_array_get_length (at) >= 2)
        {
          client->x = json_array_get_int_element (at, 0);
          client->y = json_array_get_int_element (at, 1);
        }
    }

  if (json_object_has_member (object, "size")
      && JSON_NODE_HOLDS_ARRAY (json_object_get_member (object, "size")))
    {
      size = json_object_get_array_member (object, "size");
      if (json_array_get_length (size) >= 2)
        {
          client->width = json_array_get_int_element (size, 0);
          client->height = json_array_get_int_element (size, 1);
        }
    }

  return client;
}

static gint
compare_clients_by_focus_history (gconstpointer a,
                                  gconstpointer b)
{
  const KasasaWindowClient *client_a = *(KasasaWindowClient * const *) a;
  const KasasaWindowClient *client_b = *(KasasaWindowClient * const *) b;
  gint focus_a = client_a->focus_history_id >= 0
                 ? client_a->focus_history_id : G_MAXINT;
  gint focus_b = client_b->focus_history_id >= 0
                 ? client_b->focus_history_id : G_MAXINT;
  gint result;

  if (focus_a != focus_b)
    return focus_a < focus_b ? -1 : 1;

  result = g_strcmp0 (client_a->class_name, client_b->class_name);
  if (result != 0)
    return result;

  result = g_strcmp0 (client_a->title, client_b->title);
  if (result != 0)
    return result;

  return g_strcmp0 (client_a->address, client_b->address);
}

GPtrArray *
kasasa_window_query_parse_clients_json (const gchar *json,
                                        GError     **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *array;
  GPtrArray *clients;
  guint i;

  g_return_val_if_fail (json != NULL, NULL);

  if (!json_parser_load_from_data (parser, json, -1, error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Unexpected hyprctl clients JSON"));
      return NULL;
    }

  array = json_node_get_array (root);
  clients = g_ptr_array_new_with_free_func ((GDestroyNotify) kasasa_window_client_free);

  for (i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *node = json_array_get_element (array, i);
      KasasaWindowClient *client;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        continue;

      client = client_from_json_object (json_node_get_object (node));
      if (!client->mapped || is_kasasa_client (client)
          || client->width <= 0 || client->height <= 0)
        {
          kasasa_window_client_free (client);
          continue;
        }

      g_ptr_array_add (clients, client);
    }

  g_ptr_array_sort (clients, compare_clients_by_focus_history);

  return clients;
}

KasasaWindowClient *
kasasa_window_query_parse_active_json (const gchar *json,
                                       GError     **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  KasasaWindowClient *client;

  g_return_val_if_fail (json != NULL, NULL);

  if (!json_parser_load_from_data (parser, json, -1, error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Unexpected hyprctl activewindow JSON"));
      return NULL;
    }

  client = client_from_json_object (json_node_get_object (root));
  if (!client->mapped || client->address == NULL || client->address[0] == '\0'
      || is_kasasa_client (client))
    {
      kasasa_window_client_free (client);
      return NULL;
    }

  return client;
}

GPtrArray *
kasasa_monitor_query_parse_json (const gchar *json,
                                 GError     **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *array;
  GPtrArray *monitors;
  guint i;

  g_return_val_if_fail (json != NULL, NULL);

  if (!json_parser_load_from_data (parser, json, -1, error))
    return NULL;

  root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_FAILED,
                           _("Unexpected hyprctl monitors JSON"));
      return NULL;
    }

  array = json_node_get_array (root);
  monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) kasasa_monitor_free);

  for (i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *node = json_array_get_element (array, i);
      JsonObject *object;
      KasasaMonitor *monitor;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        continue;

      object = json_node_get_object (node);
      monitor = g_new0 (KasasaMonitor, 1);
      monitor->id = json_object_get_int_member_with_default (object, "id", -1);
      monitor->name = g_strdup (
        json_object_get_string_member_with_default (object, "name", ""));
      monitor->description = g_strdup (
        json_object_get_string_member_with_default (object, "description", ""));
      monitor->width = json_object_get_int_member_with_default (object, "width", 0);
      monitor->height = json_object_get_int_member_with_default (object, "height", 0);
      monitor->scale = json_object_get_double_member_with_default (object, "scale", 1.0);
      monitor->transform = json_object_get_int_member_with_default (object, "transform", 0);
      monitor->focused = json_object_get_boolean_member_with_default (object,
                                                                      "focused",
                                                                      FALSE);

      if (monitor->name[0] == '\0' || monitor->width <= 0 || monitor->height <= 0)
        {
          kasasa_monitor_free (monitor);
          continue;
        }

      g_ptr_array_add (monitors, monitor);
    }

  return monitors;
}

static gchar *
run_hyprctl (const gchar *const *argv,
             GError            **error)
{
  g_autofree gchar *stdout_buf = NULL;
  g_autofree gchar *stderr_buf = NULL;
  gint status = 0;

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
  g_autofree gchar *hyprctl = NULL;
  const gchar *instance_signature = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");

  hyprctl = g_find_program_in_path ("hyprctl");
  return hyprctl != NULL
         && instance_signature != NULL
         && *instance_signature != '\0';
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

  json = run_hyprctl (argv, error);
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

  json = run_hyprctl (argv, error);
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

  json = run_hyprctl (argv, error);
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

static gboolean
address_equal (const gchar *a,
               const gchar *b)
{
  if (a == NULL || b == NULL)
    return FALSE;
  return g_ascii_strcasecmp (a, b) == 0;
}

static void
collect_class_matches (GPtrArray   *clients,
                       const gchar *class_name,
                       GPtrArray   *out)
{
  guint i;

  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);
      if (g_strcmp0 (client->class_name, class_name) == 0)
        g_ptr_array_add (out, kasasa_window_client_copy (client));
    }
}

static void
collect_title_matches (GPtrArray   *clients,
                       const gchar *needle,
                       GPtrArray   *out)
{
  guint i;

  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);
      if (client->title != NULL && strstr (client->title, needle) != NULL)
        g_ptr_array_add (out, kasasa_window_client_copy (client));
    }
}

static void
collect_address_matches (GPtrArray   *clients,
                         const gchar *address,
                         GPtrArray   *out)
{
  guint i;

  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);
      if (address_equal (client->address, address))
        g_ptr_array_add (out, kasasa_window_client_copy (client));
    }
}

static KasasaWindowClient *
finish_matches (GPtrArray  **matches_ptr,
                GPtrArray  **candidates,
                GError     **error)
{
  GPtrArray *matches;
  KasasaWindowClient *chosen;

  g_return_val_if_fail (matches_ptr != NULL && *matches_ptr != NULL, NULL);

  matches = *matches_ptr;

  if (matches->len == 0)
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
                           _("No window matched the specifier"));
      return NULL;
    }

  if (matches->len > 1)
    {
      if (candidates != NULL)
        *candidates = g_steal_pointer (matches_ptr);
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS,
                           _("Multiple windows matched the specifier"));
      return NULL;
    }

  chosen = g_ptr_array_steal_index (matches, 0);
  return chosen;
}

KasasaWindowClient *
kasasa_window_query_resolve (const KasasaWindowSpec   *spec,
                             GPtrArray                *clients,
                             const KasasaWindowClient *active,
                             GPtrArray               **candidates,
                             GError                  **error)
{
  g_autoptr (GPtrArray) matches = NULL;

  g_return_val_if_fail (spec != NULL, NULL);
  g_return_val_if_fail (clients != NULL, NULL);

  if (candidates != NULL)
    *candidates = NULL;

  matches = g_ptr_array_new_with_free_func ((GDestroyNotify) kasasa_window_client_free);

  switch (spec->kind)
    {
    case KASASA_WINDOW_SPEC_ACTIVE:
      if (active == NULL)
        {
          g_set_error_literal (error,
                               KASASA_WINDOW_QUERY_ERROR,
                               KASASA_WINDOW_QUERY_ERROR_NO_MATCH,
                               _("No active window"));
          return NULL;
        }
      return kasasa_window_client_copy (active);

    case KASASA_WINDOW_SPEC_ADDRESS:
      collect_address_matches (clients, spec->value, matches);
      return finish_matches (&matches, candidates, error);

    case KASASA_WINDOW_SPEC_CLASS:
      collect_class_matches (clients, spec->value, matches);
      return finish_matches (&matches, candidates, error);

    case KASASA_WINDOW_SPEC_TITLE:
      collect_title_matches (clients, spec->value, matches);
      return finish_matches (&matches, candidates, error);

    case KASASA_WINDOW_SPEC_BARE:
      collect_class_matches (clients, spec->value, matches);
      if (matches->len >= 1)
        return finish_matches (&matches, candidates, error);

      /* No class hit: try title substring. */
      collect_title_matches (clients, spec->value, matches);
      return finish_matches (&matches, candidates, error);

    default:
      g_assert_not_reached ();
    }
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
      g_string_append_printf (out,
                              "%-18s  %-16s  %-8s  %s\n",
                              client->address != NULL ? client->address : "",
                              client->class_name != NULL ? client->class_name : "",
                              client->workspace_name != NULL ? client->workspace_name : "",
                              client->title != NULL ? client->title : "");
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
  GString *out;
  guint i;

  g_return_val_if_fail (clients != NULL, NULL);

  out = g_string_new (_("Candidates:"));
  g_string_append_c (out, '\n');
  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);
      g_string_append_printf (out,
                              "  %s  class=%s  title=%s\n",
                              client->address != NULL ? client->address : "?",
                              client->class_name != NULL ? client->class_name : "",
                              client->title != NULL ? client->title : "");
    }

  return g_string_free (out, FALSE);
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

      g_string_append_printf (out,
                              "%-12s  %-11s  %-7.2f  %s%s\n",
                              monitor->name,
                              size,
                              monitor->scale,
                              monitor->description,
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
