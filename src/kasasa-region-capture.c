/* kasasa-region-capture.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-region-capture.h"

#include <glib/gstdio.h>
#include <unistd.h>

static void
cancel_subprocess (GCancellable *cancellable,
                   gpointer      user_data)
{
  g_subprocess_force_exit (G_SUBPROCESS (user_data));
}

static gboolean
run_subprocess (const gchar * const *argv,
                gboolean             capture_stdout,
                GCancellable        *cancellable,
                gchar              **stdout_text,
                GError             **error)
{
  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDERR_PIPE;
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autofree gchar *stderr_text = NULL;
  gulong cancel_id = 0;

  if (capture_stdout)
    flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;

  subprocess = g_subprocess_newv (argv, flags, error);
  if (subprocess == NULL)
    return FALSE;

  if (cancellable != NULL)
    cancel_id = g_cancellable_connect (cancellable,
                                       G_CALLBACK (cancel_subprocess),
                                       g_object_ref (subprocess),
                                       g_object_unref);

  if (!g_subprocess_communicate_utf8 (subprocess,
                                      NULL,
                                      cancellable,
                                      stdout_text,
                                      &stderr_text,
                                      error))
    {
      if (cancel_id != 0)
        g_cancellable_disconnect (cancellable, cancel_id);
      return FALSE;
    }

  if (cancel_id != 0)
    g_cancellable_disconnect (cancellable, cancel_id);

  if (!g_subprocess_get_successful (subprocess))
    {
      g_autofree gchar *detail = g_strdup (stderr_text);

      if (detail != NULL)
        g_strstrip (detail);
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "%s failed%s%s",
                   argv[0],
                   detail != NULL && *detail != '\0' ? ": " : "",
                   detail != NULL ? detail : "");
      return FALSE;
    }

  return TRUE;
}

static gboolean
geometry_is_valid (const gchar *geometry)
{
  g_autoptr (GRegex) regex = NULL;

  regex = g_regex_new ("^-?[0-9]+,-?[0-9]+ [1-9][0-9]*x[1-9][0-9]*$",
                       G_REGEX_DEFAULT,
                       G_REGEX_MATCH_DEFAULT,
                       NULL);
  return g_regex_match (regex, geometry, G_REGEX_MATCH_DEFAULT, NULL);
}

static gboolean
capture_was_cancelled (GCancellable *cancellable)
{
  return cancellable != NULL && g_cancellable_is_cancelled (cancellable);
}

static void
capture_region_worker (GTask        *task,
                       gpointer      source_object,
                       gpointer      task_data,
                       GCancellable *cancellable)
{
  const gchar *slurp_argv[] = { "slurp", NULL };
  const gchar *grim_argv[] = { "grim", "-g", NULL, NULL, NULL };
  g_autofree gchar *geometry = NULL;
  g_autofree gchar *path = NULL;
  g_autofree gchar *uri = NULL;
  g_autoptr (GError) error = NULL;
  gint fd;

  if (!run_subprocess (slurp_argv, TRUE, cancellable, &geometry, &error))
    {
      if (capture_was_cancelled (cancellable)
          || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED))
        {
          g_clear_error (&error);
          g_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   "Region selection cancelled");
        }
      else
        g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_strstrip (geometry);
  if (!geometry_is_valid (geometry))
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_INVALID_DATA,
                               "slurp returned invalid geometry: %s",
                               geometry);
      return;
    }

  fd = g_file_open_tmp ("kasasa-region-XXXXXX.png", &path, &error);
  if (fd < 0)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }
  close (fd);

  grim_argv[2] = geometry;
  grim_argv[3] = path;
  if (!run_subprocess (grim_argv, FALSE, cancellable, NULL, &error))
    {
      g_unlink (path);
      if (capture_was_cancelled (cancellable))
        {
          g_clear_error (&error);
          g_task_return_new_error (task,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   "Region capture cancelled");
        }
      else
        g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  uri = g_filename_to_uri (path, NULL, &error);
  if (uri == NULL)
    {
      g_unlink (path);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&uri), g_free);
}

gboolean
kasasa_region_capture_available (void)
{
  g_autofree gchar *slurp = g_find_program_in_path ("slurp");
  g_autofree gchar *grim = g_find_program_in_path ("grim");

  return slurp != NULL && grim != NULL;
}

void
kasasa_region_capture_screenshot_async (GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
  g_autoptr (GTask) task = NULL;

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, kasasa_region_capture_screenshot_async);
  g_task_run_in_thread (task, capture_region_worker);
}

gchar *
kasasa_region_capture_screenshot_finish (GAsyncResult *result,
                                         GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (
                          result, kasasa_region_capture_screenshot_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
