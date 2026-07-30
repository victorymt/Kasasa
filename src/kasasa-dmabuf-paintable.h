/* kasasa-dmabuf-paintable.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KASASA_TYPE_DMABUF_PAINTABLE (kasasa_dmabuf_paintable_get_type ())

G_DECLARE_FINAL_TYPE (KasasaDmabufPaintable,
                      kasasa_dmabuf_paintable,
                      KASASA,
                      DMABUF_PAINTABLE,
                      GObject)

KasasaDmabufPaintable *kasasa_dmabuf_paintable_new (void);

void kasasa_dmabuf_paintable_set_texture (KasasaDmabufPaintable *self,
                                          GdkTexture             *texture,
                                          guint32                 transform,
                                          gboolean                y_invert);

G_END_DECLS
