/* test-source.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>

#include "kasasa-source.h"

typedef struct
{
  KasasaSource *source;
  guint count;
} RearmData;

typedef struct
{
  GMainLoop *loop;
  guint count;
  gboolean timed_out;
} SecondsData;

static void
increment_counter (gpointer user_data)
{
  guint *counter = user_data;

  (*counter)++;
}

static void
rearm_until_three (gpointer user_data)
{
  RearmData *data = user_data;

  data->count++;
  if (data->count < 3)
    kasasa_source_set_timeout_once (data->source,
                                    0,
                                    rearm_until_three,
                                    data);
}

static void
seconds_callback (gpointer user_data)
{
  SecondsData *data = user_data;

  data->count++;
  g_main_loop_quit (data->loop);
}

static gboolean
seconds_guard_timeout (gpointer user_data)
{
  SecondsData *data = user_data;

  data->timed_out = TRUE;
  g_main_loop_quit (data->loop);
  return G_SOURCE_REMOVE;
}

static void
dispatch_pending_sources (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
test_dispatches_once (void)
{
  KasasaSource source = { 0 };
  guint counter = 0;

  kasasa_source_set_timeout_once (&source, 0, increment_counter, &counter);
  g_assert_cmpuint (source.id, !=, 0);

  dispatch_pending_sources ();

  g_assert_cmpuint (counter, ==, 1);
  g_assert_cmpuint (source.id, ==, 0);
}

static void
test_seconds_dispatches_once (void)
{
  KasasaSource source = { 0 };
  GSource *guard_source;
  SecondsData data = { 0 };

  data.loop = g_main_loop_new (NULL, FALSE);
  guard_source = g_timeout_source_new (3000);
  g_source_set_callback (guard_source,
                         seconds_guard_timeout,
                         &data,
                         NULL);
  g_source_attach (guard_source, NULL);

  kasasa_source_set_timeout_seconds_once (&source,
                                          1,
                                          seconds_callback,
                                          &data);
  g_assert_cmpuint (source.id, !=, 0);

  g_main_loop_run (data.loop);

  g_assert_false (data.timed_out);
  g_assert_cmpuint (data.count, ==, 1);
  g_assert_cmpuint (source.id, ==, 0);

  g_source_destroy (guard_source);
  g_source_unref (guard_source);
  g_main_loop_unref (data.loop);
}

static void
test_clear_cancels_callback (void)
{
  KasasaSource source = { 0 };
  guint counter = 0;

  kasasa_source_set_timeout_once (&source, 0, increment_counter, &counter);
  kasasa_source_clear (&source);
  dispatch_pending_sources ();

  g_assert_cmpuint (counter, ==, 0);
  g_assert_cmpuint (source.id, ==, 0);
}

static void
test_replacement_cancels_previous_callback (void)
{
  KasasaSource source = { 0 };
  guint first_counter = 0;
  guint second_counter = 0;

  kasasa_source_set_timeout_once (&source,
                                  1000,
                                  increment_counter,
                                  &first_counter);
  kasasa_source_set_timeout_once (&source,
                                  0,
                                  increment_counter,
                                  &second_counter);
  dispatch_pending_sources ();

  g_assert_cmpuint (first_counter, ==, 0);
  g_assert_cmpuint (second_counter, ==, 1);
  g_assert_cmpuint (source.id, ==, 0);
}

static void
test_callback_can_rearm_source (void)
{
  KasasaSource source = { 0 };
  RearmData data = { &source, 0 };

  kasasa_source_set_timeout_once (&source, 0, rearm_until_three, &data);
  dispatch_pending_sources ();

  g_assert_cmpuint (data.count, ==, 3);
  g_assert_cmpuint (source.id, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/source/dispatches-once", test_dispatches_once);
  g_test_add_func ("/source/seconds-dispatches-once",
                   test_seconds_dispatches_once);
  g_test_add_func ("/source/clear-cancels", test_clear_cancels_callback);
  g_test_add_func ("/source/replacement-cancels-previous",
                   test_replacement_cancels_previous_callback);
  g_test_add_func ("/source/callback-can-rearm",
                   test_callback_can_rearm_source);

  return g_test_run ();
}
