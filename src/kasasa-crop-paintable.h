/* kasasa-crop-paintable.h
 *
 * Copyright 2026 victorymt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KASASA_TYPE_CROP_PAINTABLE (kasasa_crop_paintable_get_type ())

G_DECLARE_FINAL_TYPE (KasasaCropPaintable,
                      kasasa_crop_paintable,
                      KASASA,
                      CROP_PAINTABLE,
                      GObject)

KasasaCropPaintable *kasasa_crop_paintable_new (GdkPaintable *source);

void kasasa_crop_paintable_set_source (KasasaCropPaintable *self,
                                       GdkPaintable         *source);

void kasasa_crop_paintable_set_rect (KasasaCropPaintable *self,
                                     gdouble              x,
                                     gdouble              y,
                                     gdouble              width,
                                     gdouble              height);

void kasasa_crop_paintable_get_rect (KasasaCropPaintable *self,
                                     gdouble             *x,
                                     gdouble             *y,
                                     gdouble             *width,
                                     gdouble             *height);

void kasasa_crop_paintable_reset (KasasaCropPaintable *self);

G_END_DECLS
