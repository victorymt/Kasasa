/* kasasa-crop-paintable.c
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

#include <math.h>

#include "kasasa-crop-paintable.h"

struct _KasasaCropPaintable
{
  GObject parent_instance;

  GdkPaintable *source;
  gdouble x;
  gdouble y;
  gdouble width;
  gdouble height;
};

static void kasasa_crop_paintable_paintable_init (GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
  KasasaCropPaintable,
  kasasa_crop_paintable,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE,
                          kasasa_crop_paintable_paintable_init))

static void
source_size_invalidated (GdkPaintable        *source,
                         KasasaCropPaintable *self)
{
  gdk_paintable_invalidate_size (GDK_PAINTABLE (self));
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

static void
source_contents_invalidated (GdkPaintable        *source,
                             KasasaCropPaintable *self)
{
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

static void
disconnect_source (KasasaCropPaintable *self)
{
  if (self->source == NULL)
    return;

  g_signal_handlers_disconnect_by_data (self->source, self);
  g_clear_object (&self->source);
}

static gint
crop_dimension (gint    source_dimension,
                gdouble crop_fraction)
{
  if (source_dimension <= 0)
    return 0;

  return MAX (1, (gint) llround ((gdouble) source_dimension * crop_fraction));
}

static void
kasasa_crop_paintable_snapshot (GdkPaintable *paintable,
                                GdkSnapshot  *snapshot,
                                gdouble       width,
                                gdouble       height)
{
  KasasaCropPaintable *self = KASASA_CROP_PAINTABLE (paintable);
  gint source_width;
  gint source_height;
  gdouble crop_width;
  gdouble crop_height;
  graphene_rect_t clip;
  graphene_point_t translation;

  if (self->source == NULL || width <= 0 || height <= 0)
    return;

  source_width = gdk_paintable_get_intrinsic_width (self->source);
  source_height = gdk_paintable_get_intrinsic_height (self->source);
  if (source_width <= 0 || source_height <= 0)
    return;

  crop_width = (gdouble) source_width * self->width;
  crop_height = (gdouble) source_height * self->height;
  if (crop_width <= 0 || crop_height <= 0)
    return;

  clip = GRAPHENE_RECT_INIT (0, 0, width, height);
  translation = GRAPHENE_POINT_INIT (
    -(gdouble) source_width * self->x * width / crop_width,
    -(gdouble) source_height * self->y * height / crop_height);

  gtk_snapshot_save (snapshot);
  gtk_snapshot_push_clip (snapshot, &clip);
  gtk_snapshot_translate (snapshot, &translation);
  gtk_snapshot_scale (snapshot,
                      width / crop_width,
                      height / crop_height);
  gdk_paintable_snapshot (self->source,
                          snapshot,
                          source_width,
                          source_height);
  gtk_snapshot_pop (snapshot);
  gtk_snapshot_restore (snapshot);
}

static GdkPaintableFlags
kasasa_crop_paintable_get_flags (GdkPaintable *paintable)
{
  KasasaCropPaintable *self = KASASA_CROP_PAINTABLE (paintable);

  return self->source != NULL
         ? gdk_paintable_get_flags (self->source)
             & ~(GDK_PAINTABLE_STATIC_SIZE | GDK_PAINTABLE_STATIC_CONTENTS)
         : 0;
}

static gint
kasasa_crop_paintable_get_intrinsic_width (GdkPaintable *paintable)
{
  KasasaCropPaintable *self = KASASA_CROP_PAINTABLE (paintable);

  return self->source == NULL
         ? 0
         : crop_dimension (gdk_paintable_get_intrinsic_width (self->source),
                           self->width);
}

static gint
kasasa_crop_paintable_get_intrinsic_height (GdkPaintable *paintable)
{
  KasasaCropPaintable *self = KASASA_CROP_PAINTABLE (paintable);

  return self->source == NULL
         ? 0
         : crop_dimension (gdk_paintable_get_intrinsic_height (self->source),
                           self->height);
}

static gdouble
kasasa_crop_paintable_get_intrinsic_aspect_ratio (GdkPaintable *paintable)
{
  gint width = kasasa_crop_paintable_get_intrinsic_width (paintable);
  gint height = kasasa_crop_paintable_get_intrinsic_height (paintable);

  return width > 0 && height > 0 ? (gdouble) width / (gdouble) height : 0;
}

static void
kasasa_crop_paintable_dispose (GObject *object)
{
  KasasaCropPaintable *self = KASASA_CROP_PAINTABLE (object);

  disconnect_source (self);
  G_OBJECT_CLASS (kasasa_crop_paintable_parent_class)->dispose (object);
}

static void
kasasa_crop_paintable_class_init (KasasaCropPaintableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kasasa_crop_paintable_dispose;
}

static void
kasasa_crop_paintable_init (KasasaCropPaintable *self)
{
  self->x = 0;
  self->y = 0;
  self->width = 1;
  self->height = 1;
}

static void
kasasa_crop_paintable_paintable_init (GdkPaintableInterface *iface)
{
  iface->snapshot = kasasa_crop_paintable_snapshot;
  iface->get_flags = kasasa_crop_paintable_get_flags;
  iface->get_intrinsic_width = kasasa_crop_paintable_get_intrinsic_width;
  iface->get_intrinsic_height = kasasa_crop_paintable_get_intrinsic_height;
  iface->get_intrinsic_aspect_ratio =
    kasasa_crop_paintable_get_intrinsic_aspect_ratio;
}

KasasaCropPaintable *
kasasa_crop_paintable_new (GdkPaintable *source)
{
  KasasaCropPaintable *self = g_object_new (KASASA_TYPE_CROP_PAINTABLE, NULL);

  kasasa_crop_paintable_set_source (self, source);
  return self;
}

void
kasasa_crop_paintable_set_source (KasasaCropPaintable *self,
                                  GdkPaintable         *source)
{
  g_return_if_fail (KASASA_IS_CROP_PAINTABLE (self));
  g_return_if_fail (source == NULL || GDK_IS_PAINTABLE (source));

  if (self->source == source)
    return;

  disconnect_source (self);
  if (source != NULL)
    {
      self->source = g_object_ref (source);
      g_signal_connect (source,
                        "invalidate-size",
                        G_CALLBACK (source_size_invalidated),
                        self);
      g_signal_connect (source,
                        "invalidate-contents",
                        G_CALLBACK (source_contents_invalidated),
                        self);
    }

  gdk_paintable_invalidate_size (GDK_PAINTABLE (self));
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

void
kasasa_crop_paintable_set_rect (KasasaCropPaintable *self,
                                gdouble              x,
                                gdouble              y,
                                gdouble              width,
                                gdouble              height)
{
  g_return_if_fail (KASASA_IS_CROP_PAINTABLE (self));

  x = CLAMP (x, 0.0, 1.0);
  y = CLAMP (y, 0.0, 1.0);
  width = CLAMP (width, 0.0, 1.0 - x);
  height = CLAMP (height, 0.0, 1.0 - y);

  if (self->x == x && self->y == y
      && self->width == width && self->height == height)
    return;

  self->x = x;
  self->y = y;
  self->width = width;
  self->height = height;
  gdk_paintable_invalidate_size (GDK_PAINTABLE (self));
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}

void
kasasa_crop_paintable_get_rect (KasasaCropPaintable *self,
                                gdouble             *x,
                                gdouble             *y,
                                gdouble             *width,
                                gdouble             *height)
{
  g_return_if_fail (KASASA_IS_CROP_PAINTABLE (self));

  if (x != NULL)
    *x = self->x;
  if (y != NULL)
    *y = self->y;
  if (width != NULL)
    *width = self->width;
  if (height != NULL)
    *height = self->height;
}

void
kasasa_crop_paintable_reset (KasasaCropPaintable *self)
{
  kasasa_crop_paintable_set_rect (self, 0, 0, 1, 1);
}
