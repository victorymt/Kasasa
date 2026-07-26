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
  g_test_add_func ("/source/clear-cancels", test_clear_cancels_callback);
  g_test_add_func ("/source/replacement-cancels-previous",
                   test_replacement_cancels_previous_callback);
  g_test_add_func ("/source/callback-can-rearm",
                   test_callback_can_rearm_source);

  return g_test_run ();
}
