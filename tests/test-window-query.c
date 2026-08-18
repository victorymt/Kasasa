/* test-window-query.c
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

#include "kasasa-hyprland-stream.h"
#include "kasasa-hyprctl.h"
#include "kasasa-window-query.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static const gchar *clients_json =
  "["
  "  {"
  "    \"address\": \"0xaaa\","
  "    \"class\": \"Alacritty\","
  "    \"title\": \"cv@archlinux:~\","
  "    \"mapped\": true,"
  "    \"floating\": false,"
  "    \"monitor\": 0,"
  "    \"focusHistoryID\": 1,"
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
  "    \"focusHistoryID\": 0,"
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

static const gchar *monitors_json =
  "["
  "  {"
  "    \"id\": 0,"
  "    \"name\": \"DP-1\","
  "    \"description\": \"Primary Display\","
  "    \"width\": 2560,"
  "    \"height\": 1440,"
  "    \"scale\": 1.25,"
  "    \"transform\": 1,"
  "    \"focused\": true"
  "  },"
  "  {"
  "    \"id\": 1,"
  "    \"name\": \"HDMI-A-1\","
  "    \"description\": \"Projector\","
  "    \"width\": 1920,"
  "    \"height\": 1080,"
  "    \"scale\": 1.0,"
  "    \"transform\": 0,"
  "    \"focused\": false"
  "  },"
  "  {\"id\": 2, \"name\": \"\", \"width\": 0, \"height\": 0}"
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

  g_assert_false (kasasa_window_spec_parse ("workspace:1", &spec, &error));
  g_assert_error (error, KASASA_WINDOW_QUERY_ERROR, KASASA_WINDOW_QUERY_ERROR_FAILED);
  g_assert_nonnull (strstr (error->message, "workspace:"));
  g_clear_error (&error);

  g_assert_false (kasasa_window_spec_parse (":foo", &spec, &error));
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
  g_assert_cmpstr (((KasasaWindowClient *) g_ptr_array_index (clients, 0))
                     ->address,
                   ==,
                   "0xbbb");
  g_assert_cmpint (((KasasaWindowClient *) g_ptr_array_index (clients, 0))
                     ->focus_history_id,
                   ==,
                   0);
  g_assert_cmpstr (((KasasaWindowClient *) g_ptr_array_index (clients, 1))
                     ->address,
                   ==,
                   "0xaaa");
  g_assert_cmpint (((KasasaWindowClient *) g_ptr_array_index (clients, 1))
                     ->focus_history_id,
                   ==,
                   1);

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

static void
test_monitor_parse_resolve_and_format (void)
{
  g_autoptr (GPtrArray) monitors = NULL;
  g_autoptr (KasasaMonitor) resolved = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *table = NULL;
  g_autofree gchar *json = NULL;
  KasasaMonitor *primary;

  monitors = kasasa_monitor_query_parse_json (monitors_json, &error);
  g_assert_no_error (error);
  g_assert_nonnull (monitors);
  g_assert_cmpuint (monitors->len, ==, 2);

  primary = g_ptr_array_index (monitors, 0);
  g_assert_cmpstr (primary->name, ==, "DP-1");
  g_assert_cmpint (primary->width, ==, 2560);
  g_assert_cmpint (primary->height, ==, 1440);
  g_assert_cmpfloat_with_epsilon (primary->scale, 1.25, 0.001);
  g_assert_cmpint (primary->transform, ==, 1);
  g_assert_true (primary->focused);

  resolved = kasasa_monitor_query_resolve (monitors, "active", &error);
  g_assert_no_error (error);
  g_assert_nonnull (resolved);
  g_assert_cmpstr (resolved->name, ==, "DP-1");
  g_clear_pointer (&resolved, kasasa_monitor_free);

  resolved = kasasa_monitor_query_resolve (monitors, "HDMI-A-1", &error);
  g_assert_no_error (error);
  g_assert_nonnull (resolved);
  g_assert_cmpint (resolved->id, ==, 1);
  g_clear_pointer (&resolved, kasasa_monitor_free);

  resolved = kasasa_monitor_query_resolve (monitors, "missing", &error);
  g_assert_null (resolved);
  g_assert_error (error,
                  KASASA_WINDOW_QUERY_ERROR,
                  KASASA_WINDOW_QUERY_ERROR_NO_MATCH);
  g_clear_error (&error);

  table = kasasa_monitor_query_format_table (monitors);
  g_assert_nonnull (strstr (table, "DP-1"));
  g_assert_nonnull (strstr (table, "2560x1440"));
  g_assert_nonnull (strstr (table, "Primary Display *"));

  json = kasasa_monitor_query_format_json (monitors);
  g_assert_nonnull (strstr (json, "\"name\" : \"HDMI-A-1\""));
  g_assert_nonnull (strstr (json, "\"focused\" : true"));
}

static void
test_table_format_filters_controls (void)
{
  g_autoptr (GPtrArray) clients =
    g_ptr_array_new_with_free_func ((GDestroyNotify) kasasa_window_client_free);
  g_autoptr (GPtrArray) monitors =
    g_ptr_array_new_with_free_func ((GDestroyNotify) kasasa_monitor_free);
  g_autofree gchar *client_table = NULL;
  g_autofree gchar *monitor_table = NULL;
  KasasaWindowClient *client = g_new0 (KasasaWindowClient, 1);
  KasasaMonitor *monitor = g_new0 (KasasaMonitor, 1);

  client->address = g_strdup ("0xabc\033[31m");
  client->class_name = g_strdup ("term\nclass");
  client->workspace_name = g_strdup ("1\twest");
  client->title = g_strdup ("title\r\nnext");
  g_ptr_array_add (clients, client);

  monitor->name = g_strdup ("DP-1\033[2J");
  monitor->description = g_strdup ("desk\nwest");
  monitor->width = 1920;
  monitor->height = 1080;
  monitor->scale = 1.0;
  g_ptr_array_add (monitors, monitor);

  client_table = kasasa_window_query_format_table (clients);
  monitor_table = kasasa_monitor_query_format_table (monitors);

  for (const gchar *cursor = client_table; *cursor != '\0'; cursor++)
    if ((guchar) *cursor < 0x20)
      g_assert_cmpint (*cursor, ==, '\n');
  for (const gchar *cursor = monitor_table; *cursor != '\0'; cursor++)
    if ((guchar) *cursor < 0x20)
      g_assert_cmpint (*cursor, ==, '\n');
  g_assert_null (strchr (client_table, '\033'));
  g_assert_nonnull (strstr (client_table, "0xabc [31m"));
  g_assert_nonnull (strstr (client_table, "term class"));
  g_assert_nonnull (strstr (client_table, "title  next"));
  g_assert_null (strchr (monitor_table, '\033'));
  g_assert_nonnull (strstr (monitor_table, "DP-1 [2J"));
  g_assert_nonnull (strstr (monitor_table, "desk west"));
}

static void
test_live_active_skips_client_listing (void)
{
  static const gchar script[] =
    "#!/bin/sh\n"
    "printf '%s\\n' \"$*\" >> \"$KASASA_QUERY_LOG\"\n"
    "if [ \"$2\" = activewindow ]; then\n"
    "  printf '%s\\n' '{\"address\":\"0xabc\",\"class\":\"Alacritty\","
    "\"title\":\"shell\",\"mapped\":true,\"floating\":false,"
    "\"monitor\":0,\"workspace\":{\"id\":1,\"name\":\"1\"},"
    "\"at\":[0,0],\"size\":[800,600]}'\n"
    "  exit 0\n"
    "fi\n"
    "exit 42\n";
  g_autofree gchar *old_path = NULL;
  g_autofree gchar *test_path = NULL;
  g_autofree gchar *tmp_dir = NULL;
  g_autofree gchar *hyprctl_path = NULL;
  g_autofree gchar *log_path = NULL;
  g_autofree gchar *log_contents = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaWindowClient) resolved = NULL;

  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 5 * G_TIME_SPAN_SECOND, 0);
      g_test_trap_assert_passed ();
      return;
    }

  tmp_dir = g_dir_make_tmp ("kasasa-window-query-test-XXXXXX", &error);
  g_assert_no_error (error);
  hyprctl_path = g_build_filename (tmp_dir, "hyprctl", NULL);
  log_path = g_build_filename (tmp_dir, "calls.log", NULL);
  g_assert_true (g_file_set_contents (hyprctl_path, script, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_chmod (hyprctl_path, 0700), ==, 0);

  old_path = g_strdup (g_getenv ("PATH"));
  test_path = g_strconcat (tmp_dir, ":", old_path != NULL ? old_path : "", NULL);
  g_setenv ("PATH", test_path, TRUE);
  g_setenv ("HYPRLAND_INSTANCE_SIGNATURE", "kasasa-test-instance", TRUE);
  g_setenv ("KASASA_QUERY_LOG", log_path, TRUE);

  resolved = kasasa_window_query_resolve_live ("active", &error);
  g_assert_no_error (error);
  g_assert_nonnull (resolved);
  g_assert_cmpstr (resolved->address, ==, "0xabc");
  g_assert_true (g_file_get_contents (log_path, &log_contents, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (log_contents, ==, "-j activewindow\n");

  g_assert_cmpint (g_remove (log_path), ==, 0);
  g_assert_cmpint (g_remove (hyprctl_path), ==, 0);
  g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);
}

static void
test_hyprctl_query_times_out (void)
{
  static const gchar script[] =
    "#!/bin/sh\n"
    "sleep 10\n";
  const gchar *argv[2];
  g_autofree gchar *tmp_dir = NULL;
  g_autofree gchar *script_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *output = NULL;
  gint64 started;
  gint64 elapsed;

  tmp_dir = g_dir_make_tmp ("kasasa-hyprctl-test-XXXXXX", &error);
  g_assert_no_error (error);
  script_path = g_build_filename (tmp_dir, "hyprctl", NULL);
  g_assert_true (g_file_set_contents (script_path, script, -1, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_chmod (script_path, 0700), ==, 0);

  argv[0] = script_path;
  argv[1] = NULL;
  started = g_get_monotonic_time ();
  output = kasasa_hyprctl_query (argv, &error);
  elapsed = g_get_monotonic_time () - started;

  g_assert_null (output);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_cmpint (elapsed, <, 8 * G_TIME_SPAN_SECOND);

  g_clear_error (&error);
  g_assert_cmpint (g_remove (script_path), ==, 0);
  g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);
}

static void
ignore_stream_frame (gpointer                    user_data,
                     const guint8               *data,
                     gint                        width,
                     gint                        height,
                     gint                        stride,
                     KasasaHyprlandStreamFormat  format,
                     gboolean                    y_invert,
                     guint32                     transform)
{
  (void) user_data;
  (void) data;
  (void) width;
  (void) height;
  (void) stride;
  (void) format;
  (void) y_invert;
  (void) transform;
}

typedef struct
{
  GError *error;
  gint called;
} StreamErrorResult;

static void
record_stream_error (gpointer      user_data,
                     const GError *error)
{
  StreamErrorResult *result = user_data;

  result->error = g_error_copy (error);
  g_atomic_int_set (&result->called, TRUE);
}

static void
run_stream_connection_failure_test (gboolean output)
{
  g_autofree gchar *old_path = NULL;
  g_autofree gchar *test_path = NULL;
  g_autofree gchar *tmp_dir = NULL;
  g_autofree gchar *hyprctl_path = NULL;
  g_autoptr (GError) setup_error = NULL;
  g_autoptr (GError) error = NULL;
  StreamErrorResult result = { 0 };
  KasasaHyprlandStream *stream;

  if (!g_test_subprocess ())
    {
      g_test_trap_subprocess (NULL, 5 * G_TIME_SPAN_SECOND, 0);
      g_test_trap_assert_passed ();
      return;
    }

  old_path = g_strdup (g_getenv ("PATH"));
  tmp_dir = g_dir_make_tmp ("kasasa-hyprland-stream-test-XXXXXX", &setup_error);
  g_assert_no_error (setup_error);
  g_assert_nonnull (tmp_dir);

  hyprctl_path = g_build_filename (tmp_dir, "hyprctl", NULL);
  g_assert_true (g_file_set_contents (hyprctl_path,
                                      "#!/bin/sh\nexit 0\n",
                                      -1,
                                      &setup_error));
  g_assert_no_error (setup_error);
  g_assert_cmpint (g_chmod (hyprctl_path, 0700), ==, 0);

  test_path = g_strconcat (tmp_dir, ":", old_path != NULL ? old_path : "", NULL);
  g_setenv ("PATH", test_path, TRUE);
  g_setenv ("HYPRLAND_INSTANCE_SIGNATURE", "kasasa-test-instance", TRUE);
  g_setenv ("WAYLAND_DISPLAY", "kasasa-test-display-does-not-exist", TRUE);
  if (output)
    stream = kasasa_hyprland_stream_start_output ("DP-1",
                                                  30,
                                                  ignore_stream_frame,
                                                  record_stream_error,
                                                  &result,
                                                  NULL,
                                                  &error);
  else
    stream = kasasa_hyprland_stream_start (1,
                                           30,
                                           ignore_stream_frame,
                                           record_stream_error,
                                           &result,
                                           NULL,
                                           &error);

  g_assert_no_error (error);
  g_assert_nonnull (stream);

  {
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

    while (!g_atomic_int_get (&result.called)
           && g_get_monotonic_time () < deadline)
      g_usleep (1000);
  }

  kasasa_hyprland_stream_stop (stream);

  g_assert_cmpint (g_remove (hyprctl_path), ==, 0);
  g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);

  g_assert_true (g_atomic_int_get (&result.called));
  g_assert_error (result.error,
                  KASASA_WINDOW_QUERY_ERROR,
                  KASASA_WINDOW_QUERY_ERROR_FAILED);
  g_clear_error (&result.error);
}

static void
test_stream_reports_connection_failure (void)
{
  run_stream_connection_failure_test (FALSE);
}

static void
test_output_stream_reports_connection_failure (void)
{
  run_stream_connection_failure_test (TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/unit/window-query/spec-parse", test_spec_parse);
  g_test_add_func ("/unit/window-query/resolve", test_parse_and_resolve);
  g_test_add_func ("/unit/window-query/handle", test_handle_from_address);
  g_test_add_func ("/unit/monitor-query/parse-resolve-format",
                   test_monitor_parse_resolve_and_format);
  g_test_add_func ("/unit/window-query/table-filters-controls",
                   test_table_format_filters_controls);
  g_test_add_func ("/unit/window-query/live-active",
                   test_live_active_skips_client_listing);
  g_test_add_func ("/unit/hyprctl/query-timeout",
                   test_hyprctl_query_times_out);
  g_test_add_func ("/unit/hyprland-stream/connection-failure",
                   test_stream_reports_connection_failure);
  g_test_add_func ("/unit/hyprland-stream/output-connection-failure",
                   test_output_stream_reports_connection_failure);
  return g_test_run ();
}
