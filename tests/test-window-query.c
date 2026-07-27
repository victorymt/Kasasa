/* test-window-query.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-hyprland-stream.h"
#include "kasasa-window-query.h"

static const gchar *clients_json =
  "["
  "  {"
  "    \"address\": \"0xaaa\","
  "    \"class\": \"Alacritty\","
  "    \"title\": \"cv@archlinux:~\","
  "    \"mapped\": true,"
  "    \"floating\": false,"
  "    \"monitor\": 0,"
  "    \"workspace\": {\"id\": 1, \"name\": \"1\"},"
  "    \"at\": [7, 39],"
  "    \"size\": [1906, 1154]"
  "  },"
  "  {"
  "    \"address\": \"0xbbb\","
  "    \"class\": \"Alacritty\","
  "    \"title\": \"nvim config\","
  "    \"mapped\": true,"
  "    \"floating\": false,"
  "    \"monitor\": 0,"
  "    \"workspace\": {\"id\": 2, \"name\": \"2\"},"
  "    \"at\": [100, 100],"
  "    \"size\": [800, 600]"
  "  },"
  "  {"
  "    \"address\": \"0xccc\","
  "    \"class\": \"chromium\","
  "    \"title\": \"Docs\","
  "    \"mapped\": true,"
  "    \"floating\": false,"
  "    \"monitor\": 0,"
  "    \"workspace\": {\"id\": 1, \"name\": \"1\"},"
  "    \"at\": [0, 0],"
  "    \"size\": [100, 100]"
  "  },"
  "  {"
  "    \"address\": \"0xddd\","
  "    \"class\": \"hidden\","
  "    \"title\": \"nope\","
  "    \"mapped\": false,"
  "    \"floating\": false,"
  "    \"monitor\": 0,"
  "    \"workspace\": {\"id\": 1, \"name\": \"1\"},"
  "    \"at\": [0, 0],"
  "    \"size\": [100, 100]"
  "  }"
  "]";

static void
test_spec_parse (void)
{
  KasasaWindowSpec spec = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (kasasa_window_spec_parse ("active", &spec, &error));
  g_assert_no_error (error);
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_ACTIVE);
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("class:Alacritty", &spec, &error));
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_CLASS);
  g_assert_cmpstr (spec.value, ==, "Alacritty");
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("title:nvim", &spec, &error));
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_TITLE);
  g_assert_cmpstr (spec.value, ==, "nvim");
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("address:0xabc", &spec, &error));
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_ADDRESS);
  g_assert_cmpstr (spec.value, ==, "0xabc");
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("0xdef", &spec, &error));
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_ADDRESS);
  g_assert_cmpstr (spec.value, ==, "0xdef");
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("Alacritty", &spec, &error));
  g_assert_cmpint (spec.kind, ==, KASASA_WINDOW_SPEC_BARE);
  g_assert_cmpstr (spec.value, ==, "Alacritty");
  kasasa_window_spec_clear (&spec);

  g_assert_false (kasasa_window_spec_parse ("", &spec, &error));
  g_assert_error (error, KASASA_WINDOW_QUERY_ERROR, KASASA_WINDOW_QUERY_ERROR_FAILED);
  g_clear_error (&error);
}

static void
test_parse_and_resolve (void)
{
  g_autoptr (GPtrArray) clients = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaWindowClient) resolved = NULL;
  g_autoptr (GPtrArray) candidates = NULL;
  KasasaWindowSpec spec = { 0 };
  KasasaWindowClient *active;

  clients = kasasa_window_query_parse_clients_json (clients_json, &error);
  g_assert_no_error (error);
  g_assert_nonnull (clients);
  /* unmapped filtered out */
  g_assert_cmpuint (clients->len, ==, 3);

  active = g_ptr_array_index (clients, 0);

  g_assert_true (kasasa_window_spec_parse ("title:nvim", &spec, &error));
  resolved = kasasa_window_query_resolve (&spec, clients, active, &candidates, &error);
  g_assert_no_error (error);
  g_assert_nonnull (resolved);
  g_assert_cmpstr (resolved->address, ==, "0xbbb");
  g_assert_null (candidates);
  kasasa_window_spec_clear (&spec);
  g_clear_pointer (&resolved, kasasa_window_client_free);

  g_assert_true (kasasa_window_spec_parse ("Alacritty", &spec, &error));
  resolved = kasasa_window_query_resolve (&spec, clients, active, &candidates, &error);
  g_assert_error (error, KASASA_WINDOW_QUERY_ERROR, KASASA_WINDOW_QUERY_ERROR_AMBIGUOUS);
  g_assert_null (resolved);
  g_assert_nonnull (candidates);
  g_assert_cmpuint (candidates->len, ==, 2);
  g_clear_error (&error);
  g_clear_pointer (&candidates, kasasa_window_client_list_free);
  kasasa_window_spec_clear (&spec);

  g_assert_true (kasasa_window_spec_parse ("chromium", &spec, &error));
  resolved = kasasa_window_query_resolve (&spec, clients, active, &candidates, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (resolved->class_name, ==, "chromium");
  kasasa_window_spec_clear (&spec);
  g_clear_pointer (&resolved, kasasa_window_client_free);

  g_assert_true (kasasa_window_spec_parse ("active", &spec, &error));
  resolved = kasasa_window_query_resolve (&spec, clients, active, &candidates, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (resolved->address, ==, active->address);
  kasasa_window_spec_clear (&spec);
  g_clear_pointer (&resolved, kasasa_window_client_free);

  g_assert_true (kasasa_window_spec_parse ("title:missing", &spec, &error));
  resolved = kasasa_window_query_resolve (&spec, clients, active, &candidates, &error);
  g_assert_error (error, KASASA_WINDOW_QUERY_ERROR, KASASA_WINDOW_QUERY_ERROR_NO_MATCH);
  g_assert_null (resolved);
  g_clear_error (&error);
  kasasa_window_spec_clear (&spec);
}

static void
test_handle_from_address (void)
{
  guint32 handle = 0;
  g_autoptr (GError) error = NULL;

  g_assert_true (kasasa_hyprland_stream_handle_from_address ("0x55d517efd3c0",
                                                             &handle,
                                                             &error));
  g_assert_no_error (error);
  g_assert_cmpuint (handle, ==, 0x17efd3c0u);

  g_assert_true (kasasa_hyprland_stream_handle_from_address ("3512854448",
                                                             &handle,
                                                             &error));
  g_assert_cmpuint (handle, ==, 3512854448u);

  g_assert_false (kasasa_hyprland_stream_handle_from_address ("not-a-handle",
                                                              &handle,
                                                              &error));
  g_assert_nonnull (error);
  g_clear_error (&error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unit/window-query/spec-parse", test_spec_parse);
  g_test_add_func ("/unit/window-query/resolve", test_parse_and_resolve);
  g_test_add_func ("/unit/window-query/handle", test_handle_from_address);
  return g_test_run ();
}
