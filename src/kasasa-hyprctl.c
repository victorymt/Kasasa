/* kasasa-hyprctl.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gio/gio.h>

#include "kasasa-hyprctl.h"

#define KASASA_HYPRCTL_TIMEOUT_USEC (5 * G_TIME_SPAN_SECOND)

typedef struct
{
  GCancellable *cancellable;
  GMutex mutex;
  GCond cond;
  gboolean done;
  gboolean timed_out;
} QueryTimeout;

static void
force_exit_subprocess (GCancellable *cancellable,
                       gpointer      user_data)
{
  (void) cancellable;
  g_subprocess_force_exit (G_SUBPROCESS (user_data));
}

static gpointer
query_timeout_thread (gpointer user_data)
{
  QueryTimeout *timeout = user_data;
  gboolean cancel = FALSE;
  gint64 deadline = g_get_monotonic_time () + KASASA_HYPRCTL_TIMEOUT_USEC;

  g_mutex_lock (&timeout->mutex);
  while (!timeout->done
         && g_cond_wait_until (&timeout->cond, &timeout->mutex, deadline))
    {
    }

  if (!timeout->done)
    {
      timeout->timed_out = TRUE;
      cancel = TRUE;
    }
  g_mutex_unlock (&timeout->mutex);

  if (cancel)
    g_cancellable_cancel (timeout->cancellable);

  return NULL;
}

static gboolean
query_timed_out (QueryTimeout *timeout)
{
  gboolean timed_out;

  g_mutex_lock (&timeout->mutex);
  timed_out = timeout->timed_out;
  timeout->done = TRUE;
  g_cond_signal (&timeout->cond);
  g_mutex_unlock (&timeout->mutex);

  return timed_out;
}

gboolean
kasasa_hyprctl_available (void)
{
  g_autofree gchar *hyprctl = NULL;
  const gchar *instance_signature = g_getenv ("HYPRLAND_INSTANCE_SIGNATURE");

  hyprctl = g_find_program_in_path ("hyprctl");
  return hyprctl != NULL
         && instance_signature != NULL
         && *instance_signature != '\0';
}

gchar *
kasasa_hyprctl_query (const gchar *const *argv,
                      GError          **error)
{
  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE
                           | G_SUBPROCESS_FLAGS_STDERR_PIPE;
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GCancellable) cancellable = NULL;
  g_autofree gchar *stdout_buf = NULL;
  g_autofree gchar *stderr_buf = NULL;
  QueryTimeout timeout = { 0 };
  g_autoptr (GError) timeout_error = NULL;
  GThread *timeout_thread = NULL;
  gulong cancel_id = 0;
  gboolean communicated;
  gboolean timed_out;

  g_return_val_if_fail (argv != NULL && argv[0] != NULL, NULL);

  subprocess = g_subprocess_newv (argv, flags, error);
  if (subprocess == NULL)
    return NULL;

  cancellable = g_cancellable_new ();
  cancel_id = g_cancellable_connect (cancellable,
                                     G_CALLBACK (force_exit_subprocess),
                                     g_object_ref (subprocess),
                                     g_object_unref);
  timeout.cancellable = cancellable;
  g_mutex_init (&timeout.mutex);
  g_cond_init (&timeout.cond);
  timeout_thread = g_thread_try_new ("kasasa-hyprctl-timeout",
                                    query_timeout_thread,
                                    &timeout,
                                    &timeout_error);
  if (timeout_thread == NULL)
    {
      g_cancellable_disconnect (cancellable, cancel_id);
      g_mutex_clear (&timeout.mutex);
      g_cond_clear (&timeout.cond);
      g_propagate_error (error, g_steal_pointer (&timeout_error));
      return NULL;
    }

  communicated = g_subprocess_communicate_utf8 (subprocess,
                                                NULL,
                                                cancellable,
                                                &stdout_buf,
                                                &stderr_buf,
                                                error);
  timed_out = query_timed_out (&timeout);
  g_thread_join (timeout_thread);
  g_cancellable_disconnect (cancellable, cancel_id);
  g_mutex_clear (&timeout.mutex);
  g_cond_clear (&timeout.cond);

  if (!communicated)
    {
      if (timed_out)
        {
          g_clear_error (error);
          g_set_error_literal (error,
                               G_IO_ERROR,
                               G_IO_ERROR_TIMED_OUT,
                               "hyprctl query timed out");
        }
      return NULL;
    }

  if (!g_subprocess_get_successful (subprocess))
    {
      g_set_error (error,
                   G_SPAWN_ERROR,
                   G_SPAWN_ERROR_FAILED,
                   "hyprctl exited unsuccessfully");
      if (stderr_buf != NULL && *stderr_buf != '\0')
        g_prefix_error (error, "%s: ", g_strstrip (stderr_buf));
      return NULL;
    }

  return g_steal_pointer (&stdout_buf);
}
