/* kasasa-window-resolver.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib/gi18n.h>
#include <string.h>

#include "kasasa-window-resolver.h"

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
  for (guint i = 0; i < clients->len; i++)
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
  for (guint i = 0; i < clients->len; i++)
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
  for (guint i = 0; i < clients->len; i++)
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

  return g_ptr_array_steal_index (matches, 0);
}

KasasaWindowClient *
kasasa_window_resolver_resolve (const KasasaWindowSpec    *spec,
                                GPtrArray                 *clients,
                                const KasasaWindowClient  *active,
                                GPtrArray                **candidates,
                                GError                   **error)
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
      collect_title_matches (clients, spec->value, matches);
      return finish_matches (&matches, candidates, error);

    default:
      g_assert_not_reached ();
    }
}

gchar *
kasasa_window_resolver_format_candidates (GPtrArray *clients)
{
  GString *out;

  g_return_val_if_fail (clients != NULL, NULL);
  out = g_string_new (_("Candidates:"));
  g_string_append_c (out, '\n');
  for (guint i = 0; i < clients->len; i++)
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
