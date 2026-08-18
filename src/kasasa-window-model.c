/* kasasa-window-model.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>

#include "kasasa-window-model.h"

static gboolean
is_kasasa_client (const KasasaWindowClient *client)
{
  if (client == NULL || client->class_name == NULL)
    return FALSE;

  return g_strcmp0 (client->class_name, "io.github.kelvinnovais.Kasasa") == 0
         || g_strcmp0 (client->class_name, "kasasa") == 0
         || g_strcmp0 (client->class_name, "Kasasa") == 0;
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
    client->workspace_name = g_strdup ("");

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
kasasa_window_model_parse_clients_json (const gchar *json,
                                        GError     **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *array;
  GPtrArray *clients;

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
  for (guint i = 0; i < json_array_get_length (array); i++)
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
kasasa_window_model_parse_active_json (const gchar *json,
                                       GError      **error)
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
kasasa_window_model_parse_monitors_json (const gchar *json,
                                         GError     **error)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *array;
  GPtrArray *monitors;

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
  for (guint i = 0; i < json_array_get_length (array); i++)
    {
      JsonNode *node = json_array_get_element (array, i);
      JsonObject *object;
      KasasaMonitor *monitor;

      if (!JSON_NODE_HOLDS_OBJECT (node))
        continue;

      object = json_node_get_object (node);
      monitor = g_new0 (KasasaMonitor, 1);
      monitor->id = json_object_get_int_member_with_default (object, "id", -1);
      monitor->name = g_strdup (json_object_get_string_member_with_default (object, "name", ""));
      monitor->description = g_strdup (json_object_get_string_member_with_default (object, "description", ""));
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
