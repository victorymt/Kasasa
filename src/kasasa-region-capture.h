/* kasasa-region-capture.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean kasasa_region_capture_available (void);
void kasasa_region_capture_screenshot_async (GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);
gchar *kasasa_region_capture_screenshot_finish (GAsyncResult *result,
                                                GError      **error);

G_END_DECLS
