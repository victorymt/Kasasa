/* kasasa-window-picker.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "kasasa-window-query.h"

G_BEGIN_DECLS

typedef void (*KasasaWindowPickerCallback) (const KasasaWindowClient *client,
                                            gpointer                  user_data);

gboolean kasasa_window_picker_present (GtkWindow                  *parent,
                                       const gchar                *title,
                                       KasasaWindowPickerCallback  callback,
                                       gpointer                    user_data,
                                       GDestroyNotify              destroy,
                                       GError                    **error);

G_END_DECLS
