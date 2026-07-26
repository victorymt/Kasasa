/* kasasa-source.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  guint id;
} KasasaSource;

void kasasa_source_clear (KasasaSource *source);
void kasasa_source_set_timeout_once (KasasaSource   *source,
                                     guint           interval_ms,
                                     GSourceOnceFunc callback,
                                     gpointer        user_data);
void kasasa_source_set_timeout_seconds_once (KasasaSource   *source,
                                             guint           interval_seconds,
                                             GSourceOnceFunc callback,
                                             gpointer        user_data);

G_END_DECLS
