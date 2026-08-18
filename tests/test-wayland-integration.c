/* test-wayland-integration.c
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

#include <gdk/wayland/gdkwayland.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include "kasasa-hyprland-stream.h"
#include "kasasa-window-query.h"

#ifdef KASASA_HAVE_LAYER_SHELL
#include <gtk-layer-shell/gtk-layer-shell.h>
#endif

typedef struct
{
  GMutex mutex;
  GCond cond;
  guint shm_frames;
  guint dmabuf_frames;
  gboolean shm_frame_valid;
  gboolean dmabuf_frame_valid;
  gint width;
  gint height;
  gint stride;
  KasasaHyprlandStreamFormat format;
  GError *error;
} CaptureSmokeState;

static void
capture_smoke_state_init (CaptureSmokeState *state)
{
  g_mutex_init (&state->mutex);
  g_cond_init (&state->cond);
}

static void
capture_smoke_state_clear (CaptureSmokeState *state)
{
  g_clear_error (&state->error);
  g_cond_clear (&state->cond);
  g_mutex_clear (&state->mutex);
}

static void
capture_smoke_frame (gpointer                    user_data,
                     const guint8               *data,
                     gint                        width,
                     gint                        height,
                     gint                        stride,
                     KasasaHyprlandStreamFormat  format,
                     gboolean                    y_invert,
                     guint32                     transform)
{
  CaptureSmokeState *state = user_data;

  (void) y_invert;
  (void) transform;

  g_mutex_lock (&state->mutex);
  state->shm_frames++;
  state->shm_frame_valid = data != NULL
                           && width > 0
                           && height > 0
                           && stride >= width * 4;
  state->width = width;
  state->height = height;
  state->stride = stride;
  state->format = format;
  g_cond_broadcast (&state->cond);
  g_mutex_unlock (&state->mutex);
}

static void
capture_smoke_dmabuf (gpointer      user_data,
                      gint          fd,
                      gint          width,
                      gint          height,
                      gint          stride,
                      guint32       offset,
                      guint32       fourcc,
                      guint64       modifier,
                      gboolean      y_invert,
                      guint32       transform,
                      GDestroyNotify release,
                      gpointer      release_data)
{
  CaptureSmokeState *state = user_data;

  (void) offset;
  (void) fourcc;
  (void) modifier;
  (void) y_invert;
  (void) transform;

  g_mutex_lock (&state->mutex);
  state->dmabuf_frames++;
  state->dmabuf_frame_valid = fd >= 0
                              && width > 0
                              && height > 0
                              && stride >= width * 4;
  state->width = width;
  state->height = height;
  state->stride = stride;
  g_cond_broadcast (&state->cond);
  g_mutex_unlock (&state->mutex);

  /* The smoke test does not import the buffer, so return the lease
   * immediately and let the stream exercise the pool repeatedly. */
  if (release != NULL)
    release (release_data);
}

static void
capture_smoke_error (gpointer      user_data,
                     const GError *error)
{
  CaptureSmokeState *state = user_data;

  g_mutex_lock (&state->mutex);
  if (state->error == NULL)
    state->error = g_error_copy (error);
  g_cond_broadcast (&state->cond);
  g_mutex_unlock (&state->mutex);
}

static gboolean
capture_smoke_wait_for_frame (CaptureSmokeState *state,
                              gint64             timeout_usec)
{
  gint64 deadline = g_get_monotonic_time () + timeout_usec;
  gboolean received;

  g_mutex_lock (&state->mutex);
  while (state->shm_frames == 0
         && state->dmabuf_frames == 0
         && state->error == NULL
         && g_get_monotonic_time () < deadline)
    g_cond_wait_until (&state->cond, &state->mutex, deadline);
  received = state->shm_frames > 0 || state->dmabuf_frames > 0;
  g_mutex_unlock (&state->mutex);

  return received;
}

static void
capture_smoke_assert_result (CaptureSmokeState *state,
                             const gchar      *source_name)
{
  g_autofree gchar *error_message = NULL;
  guint shm_frames;
  guint dmabuf_frames;
  gboolean shm_frame_valid;
  gboolean dmabuf_frame_valid;
  gint width;
  gint height;
  gint stride;
  const gchar *disable_dmabuf;

  disable_dmabuf = g_getenv ("KASASA_DISABLE_DMABUF");

  g_mutex_lock (&state->mutex);
  shm_frames = state->shm_frames;
  dmabuf_frames = state->dmabuf_frames;
  shm_frame_valid = state->shm_frame_valid;
  dmabuf_frame_valid = state->dmabuf_frame_valid;
  width = state->width;
  height = state->height;
  stride = state->stride;
  if (state->error != NULL)
    error_message = g_strdup (state->error->message);
  g_mutex_unlock (&state->mutex);

  g_test_message ("%s capture received %u wl_shm and %u DMA-BUF frames (%dx%d stride=%d)",
                  source_name,
                  shm_frames,
                  dmabuf_frames,
                  width,
                  height,
                  stride);
  g_assert_null (error_message);
  g_assert_cmpuint (shm_frames + dmabuf_frames, >, 0);
  if (shm_frames > 0)
    g_assert_true (shm_frame_valid);
  if (dmabuf_frames > 0)
    g_assert_true (dmabuf_frame_valid);
  if (disable_dmabuf != NULL && g_strcmp0 (disable_dmabuf, "0") != 0)
    {
      g_assert_cmpuint (shm_frames, >, 0);
      g_assert_cmpuint (dmabuf_frames, ==, 0);
    }
}

static gchar *
integration_monitor_name (void)
{
  GdkDisplay *display = gdk_display_get_default ();
  GListModel *monitors;
  g_autoptr (GdkMonitor) monitor = NULL;
  const gchar *connector;

  monitors = gdk_display_get_monitors (display);
  if (g_list_model_get_n_items (monitors) == 0)
    return NULL;

  monitor = g_list_model_get_item (monitors, 0);
  connector = gdk_monitor_get_connector (monitor);
  return connector != NULL && *connector != '\0' ? g_strdup (connector) : NULL;
}

static void
test_monitor_capture_smoke (void)
{
  const gchar *requested_output = g_getenv ("KASASA_INTEGRATION_OUTPUT");
  g_autofree gchar *output_name = NULL;
  g_autoptr (GError) error = NULL;
  CaptureSmokeState state = { 0 };
  KasasaHyprlandStream *stream;
  gint64 stop_started;
  gint64 stop_elapsed;

  output_name = requested_output != NULL && *requested_output != '\0'
                ? g_strdup (requested_output)
                : integration_monitor_name ();
  if (output_name == NULL)
    {
      g_test_skip ("The Wayland display did not expose a monitor connector");
      return;
    }

  capture_smoke_state_init (&state);
  stream = kasasa_hyprland_stream_start_output (output_name,
                                                5,
                                                capture_smoke_frame,
                                                capture_smoke_error,
                                                &state,
                                                NULL,
                                                &error);
  g_assert_no_error (error);
  g_assert_nonnull (stream);
  g_assert_true (capture_smoke_wait_for_frame (&state,
                                               8 * G_TIME_SPAN_SECOND));

  stop_started = g_get_monotonic_time ();
  kasasa_hyprland_stream_stop (stream);
  stop_elapsed = g_get_monotonic_time () - stop_started;
  g_assert_cmpint (stop_elapsed, <, 5 * G_TIME_SPAN_SECOND);
  capture_smoke_assert_result (&state, "monitor");
  capture_smoke_state_clear (&state);
}

static KasasaWindowClient *
find_integration_window (const gchar *title,
                         GError     **error)
{
  GPtrArray *clients;
  KasasaWindowClient *match = NULL;
  guint i;

  clients = kasasa_window_query_list_clients (error);
  if (clients == NULL)
    return NULL;

  for (i = 0; i < clients->len; i++)
    {
      KasasaWindowClient *client = g_ptr_array_index (clients, i);

      if (client->mapped && g_strcmp0 (client->title, title) == 0)
        {
          match = kasasa_window_client_copy (client);
          break;
        }
    }

  kasasa_window_client_list_free (clients);
  return match;
}

static void
run_window_capture_smoke (guint32      handle,
                          const gchar *source_name)
{
  g_autoptr (GError) error = NULL;
  CaptureSmokeState state = { 0 };
  KasasaHyprlandStream *stream;
  gint64 stop_started;
  gint64 stop_elapsed;

  capture_smoke_state_init (&state);
  stream = kasasa_hyprland_stream_start_dmabuf (handle,
                                                5,
                                                capture_smoke_dmabuf,
                                                capture_smoke_frame,
                                                capture_smoke_error,
                                                &state,
                                                NULL,
                                                &error);
  g_assert_no_error (error);
  g_assert_nonnull (stream);
  g_assert_true (capture_smoke_wait_for_frame (&state,
                                               8 * G_TIME_SPAN_SECOND));

  stop_started = g_get_monotonic_time ();
  kasasa_hyprland_stream_stop (stream);
  stop_elapsed = g_get_monotonic_time () - stop_started;
  g_assert_cmpint (stop_elapsed, <, 5 * G_TIME_SPAN_SECOND);
  capture_smoke_assert_result (&state, source_name);
  capture_smoke_state_clear (&state);
}

static void
test_window_capture_smoke (void)
{
  g_autofree gchar *title = NULL;
  g_autoptr (GError) query_error = NULL;
  g_autoptr (GError) parse_error = NULL;
  g_autoptr (KasasaWindowClient) client = NULL;
  GtkWidget *window;
  GtkWidget *label;
  guint32 handle;
  guint attempt;

  title = g_strdup_printf ("Kasasa integration smoke %u", (guint) getpid ());
  window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (window), title);
  gtk_window_set_default_size (GTK_WINDOW (window), 320, 240);
  label = gtk_label_new ("Kasasa integration capture smoke");
  gtk_window_set_child (GTK_WINDOW (window), label);
  gtk_window_present (GTK_WINDOW (window));

  for (attempt = 0; attempt < 50 && client == NULL; attempt++)
    {
      while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);
      client = find_integration_window (title, &query_error);
      if (client != NULL)
        break;
      if (query_error != NULL)
        {
          g_test_message ("window query attempt %u failed: %s",
                          attempt + 1,
                          query_error->message);
          g_clear_error (&query_error);
        }
      g_usleep (100 * G_TIME_SPAN_MILLISECOND);
    }

  g_assert_nonnull (client);
  g_assert_true (kasasa_hyprland_stream_handle_from_address (client->address,
                                                              &handle,
                                                              &parse_error));
  g_assert_no_error (parse_error);

  run_window_capture_smoke (handle, "window");
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_wayland_display (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  g_assert_nonnull (display);
  g_assert_true (GDK_IS_WAYLAND_DISPLAY (display));
}

static void
test_hyprland_session (void)
{
  g_auto (GStrv) desktops = NULL;
  const gchar *current_desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  const gchar *instance = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");
  const gchar *session_type = g_getenv ("XDG_SESSION_TYPE");

  g_assert_cmpstr (session_type, ==, "wayland");
  g_assert_nonnull (current_desktop);
  desktops = g_strsplit (current_desktop, ":", -1);
  g_assert_true (g_strv_contains ((const gchar * const *) desktops,
                                 "Hyprland"));
  g_assert_nonnull (instance);
  g_assert_cmpstr (instance, !=, "");
}

#ifdef KASASA_HAVE_LAYER_SHELL
static void
test_layer_shell_linkage (void)
{
  guint protocol_version = gtk_layer_get_protocol_version ();

  if (protocol_version == 0)
    {
      g_test_skip ("The compositor does not advertise Layer Shell");
      return;
    }

  if (!gtk_layer_is_supported ())
    {
      g_test_skip ("The compositor does not support Layer Shell requests");
      return;
    }

  g_assert_true (gtk_layer_is_supported ());
}
#endif

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/integration/wayland-display", test_wayland_display);
  g_test_add_func ("/integration/hyprland-session", test_hyprland_session);
  g_test_add_func ("/integration/monitor-capture-smoke",
                   test_monitor_capture_smoke);
  g_test_add_func ("/integration/window-capture-smoke",
                   test_window_capture_smoke);
#ifdef KASASA_HAVE_LAYER_SHELL
  g_test_add_func ("/integration/layer-shell-linkage",
                   test_layer_shell_linkage);
#endif

  return g_test_run ();
}
