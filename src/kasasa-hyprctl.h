/* kasasa-hyprctl.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

gboolean kasasa_hyprctl_available (void);
gchar *kasasa_hyprctl_query (const gchar *const *argv,
                             GError          **error);

G_END_DECLS
