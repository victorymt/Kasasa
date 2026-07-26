/* kasasa-source.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-source.h"

typedef struct
{
  KasasaSource *source;
  GSourceOnceFunc callback;
  gpointer user_data;
} TimeoutData;

static gboolean
timeout_dispatch (gpointer user_data)
{
  TimeoutData *data = user_data;

  data->source->id = 0;
  data->callback (data->user_data);

  return G_SOURCE_REMOVE;
}

static void
set_timeout_source_once (KasasaSource   *source,
                         GSource        *timeout_source,
                         GSourceOnceFunc callback,
                         gpointer        user_data)
{
  TimeoutData *data;

  g_return_if_fail (source != NULL);
  g_return_if_fail (timeout_source != NULL);
  g_return_if_fail (callback != NULL);

  kasasa_source_clear (source);

  data = g_new0 (TimeoutData, 1);
  data->source = source;
  data->callback = callback;
  data->user_data = user_data;

  g_source_set_callback (timeout_source,
                         timeout_dispatch,
                         data,
                         g_free);
  source->id = g_source_attach (timeout_source, NULL);
  g_source_unref (timeout_source);
}

void
kasasa_source_clear (KasasaSource *source)
{
  g_return_if_fail (source != NULL);

  if (source->id != 0)
    {
      g_source_remove (source->id);
      source->id = 0;
    }
}

void
kasasa_source_set_timeout_once (KasasaSource   *source,
                                guint           interval_ms,
                                GSourceOnceFunc callback,
                                gpointer        user_data)
{
  set_timeout_source_once (source,
                           g_timeout_source_new (interval_ms),
                           callback,
                           user_data);
}

void
kasasa_source_set_timeout_seconds_once (KasasaSource   *source,
                                        guint           interval_seconds,
                                        GSourceOnceFunc callback,
                                        gpointer        user_data)
{
  set_timeout_source_once (source,
                           g_timeout_source_new_seconds (interval_seconds),
                           callback,
                           user_data);
}
