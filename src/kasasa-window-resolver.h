/* kasasa-window-resolver.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "kasasa-window-query.h"

G_BEGIN_DECLS

KasasaWindowClient *kasasa_window_resolver_resolve (
  const KasasaWindowSpec    *spec,
  GPtrArray                 *clients,
  const KasasaWindowClient  *active,
  GPtrArray                **candidates,
  GError                   **error);
gchar *kasasa_window_resolver_format_candidates (GPtrArray *clients);

G_END_DECLS
