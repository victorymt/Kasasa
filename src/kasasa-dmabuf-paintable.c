/* kasasa-dmabuf-paintable.c
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

#include <wayland-client-protocol.h>

#include "kasasa-dmabuf-paintable.h"

typedef enum
{
  KASASA_TEXTURE_ORIENTATION_IDENTITY,
  KASASA_TEXTURE_ORIENTATION_90_LEFT,
  KASASA_TEXTURE_ORIENTATION_180,
  KASASA_TEXTURE_ORIENTATION_90_RIGHT,
  KASASA_TEXTURE_ORIENTATION_HORIZONTAL,
  KASASA_TEXTURE_ORIENTATION_UPPER_LEFT_LOWER_RIGHT,
  KASASA_TEXTURE_ORIENTATION_VERTICAL,
  KASASA_TEXTURE_ORIENTATION_UPPER_RIGHT_LOWER_LEFT,
} KasasaTextureOrientation;

struct _KasasaDmabufPaintable
{
  GObject parent_instance;

  GdkTexture *texture;
  KasasaTextureOrientation orientation;
  gint intrinsic_width;
  gint intrinsic_height;
};

static void kasasa_dmabuf_paintable_paintable_init (GdkPaintableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (
  KasasaDmabufPaintable,
  kasasa_dmabuf_paintable,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE,
                         kasasa_dmabuf_paintable_paintable_init))

static KasasaTextureOrientation
texture_orientation (guint32  transform,
                     gboolean y_invert)
{
  static const KasasaTextureOrientation normal[] = {
    KASASA_TEXTURE_ORIENTATION_IDENTITY,
    KASASA_TEXTURE_ORIENTATION_90_LEFT,
    KASASA_TEXTURE_ORIENTATION_180,
    KASASA_TEXTURE_ORIENTATION_90_RIGHT,
    KASASA_TEXTURE_ORIENTATION_HORIZONTAL,
    KASASA_TEXTURE_ORIENTATION_UPPER_LEFT_LOWER_RIGHT,
    KASASA_TEXTURE_ORIENTATION_VERTICAL,
    KASASA_TEXTURE_ORIENTATION_UPPER_RIGHT_LOWER_LEFT,
  };
  static const KasasaTextureOrientation inverted[] = {
    KASASA_TEXTURE_ORIENTATION_VERTICAL,
    KASASA_TEXTURE_ORIENTATION_UPPER_RIGHT_LOWER_LEFT,
    KASASA_TEXTURE_ORIENTATION_HORIZONTAL,
    KASASA_TEXTURE_ORIENTATION_UPPER_LEFT_LOWER_RIGHT,
    KASASA_TEXTURE_ORIENTATION_180,
    KASASA_TEXTURE_ORIENTATION_90_RIGHT,
    KASASA_TEXTURE_ORIENTATION_IDENTITY,
    KASASA_TEXTURE_ORIENTATION_90_LEFT,
  };

  if (transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
    transform = WL_OUTPUT_TRANSFORM_NORMAL;

  return y_invert ? inverted[transform] : normal[transform];
}

static gboolean
orientation_swaps_axes (KasasaTextureOrientation orientation)
{
  return orientation == KASASA_TEXTURE_ORIENTATION_90_LEFT
         || orientation == KASASA_TEXTURE_ORIENTATION_90_RIGHT
         || orientation == KASASA_TEXTURE_ORIENTATION_UPPER_LEFT_LOWER_RIGHT
         || orientation == KASASA_TEXTURE_ORIENTATION_UPPER_RIGHT_LOWER_LEFT;
}

static void
kasasa_dmabuf_paintable_snapshot (GdkPaintable *paintable,
                                  GdkSnapshot  *snapshot,
                                  gdouble       width,
                                  gdouble       height)
{
  KasasaDmabufPaintable *self = KASASA_DMABUF_PAINTABLE (paintable);
  graphene_matrix_t matrix;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0, 1, 1);
  gdouble xx = 0;
  gdouble yx = 0;
  gdouble xy = 0;
  gdouble yy = 0;
  gdouble x0 = 0;
  gdouble y0 = 0;

  if (self->texture == NULL)
    return;

  switch (self->orientation)
    {
    case KASASA_TEXTURE_ORIENTATION_IDENTITY:
      xx = width;
      yy = height;
      break;
    case KASASA_TEXTURE_ORIENTATION_90_LEFT:
      xy = width;
      yx = -height;
      y0 = height;
      break;
    case KASASA_TEXTURE_ORIENTATION_180:
      xx = -width;
      yy = -height;
      x0 = width;
      y0 = height;
      break;
    case KASASA_TEXTURE_ORIENTATION_90_RIGHT:
      xy = -width;
      yx = height;
      x0 = width;
      break;
    case KASASA_TEXTURE_ORIENTATION_HORIZONTAL:
      xx = -width;
      yy = height;
      x0 = width;
      break;
    case KASASA_TEXTURE_ORIENTATION_UPPER_LEFT_LOWER_RIGHT:
      xy = width;
      yx = height;
      break;
    case KASASA_TEXTURE_ORIENTATION_VERTICAL:
      xx = width;
      yy = -height;
      y0 = height;
      break;
    case KASASA_TEXTURE_ORIENTATION_UPPER_RIGHT_LOWER_LEFT:
      xy = -width;
      yx = -height;
      x0 = width;
      y0 = height;
      break;
    default:
      g_assert_not_reached ();
    }

  graphene_matrix_init_from_2d (&matrix, xx, yx, xy, yy, x0, y0);
  gtk_snapshot_save (snapshot);
  gtk_snapshot_transform_matrix (snapshot, &matrix);
  gtk_snapshot_append_texture (snapshot, self->texture, &bounds);
  gtk_snapshot_restore (snapshot);
}

static GdkPaintableFlags
kasasa_dmabuf_paintable_get_flags (GdkPaintable *paintable)
{
  return 0;
}

static gint
kasasa_dmabuf_paintable_get_intrinsic_width (GdkPaintable *paintable)
{
  return KASASA_DMABUF_PAINTABLE (paintable)->intrinsic_width;
}

static gint
kasasa_dmabuf_paintable_get_intrinsic_height (GdkPaintable *paintable)
{
  return KASASA_DMABUF_PAINTABLE (paintable)->intrinsic_height;
}

static gdouble
kasasa_dmabuf_paintable_get_intrinsic_aspect_ratio (GdkPaintable *paintable)
{
  KasasaDmabufPaintable *self = KASASA_DMABUF_PAINTABLE (paintable);

  if (self->intrinsic_width <= 0 || self->intrinsic_height <= 0)
    return 0;

  return (gdouble) self->intrinsic_width / (gdouble) self->intrinsic_height;
}

static void
kasasa_dmabuf_paintable_dispose (GObject *object)
{
  KasasaDmabufPaintable *self = KASASA_DMABUF_PAINTABLE (object);

  g_clear_object (&self->texture);

  G_OBJECT_CLASS (kasasa_dmabuf_paintable_parent_class)->dispose (object);
}

static void
kasasa_dmabuf_paintable_class_init (KasasaDmabufPaintableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kasasa_dmabuf_paintable_dispose;
}

static void
kasasa_dmabuf_paintable_init (KasasaDmabufPaintable *self)
{
}

static void
kasasa_dmabuf_paintable_paintable_init (GdkPaintableInterface *iface)
{
  iface->snapshot = kasasa_dmabuf_paintable_snapshot;
  iface->get_flags = kasasa_dmabuf_paintable_get_flags;
  iface->get_intrinsic_width = kasasa_dmabuf_paintable_get_intrinsic_width;
  iface->get_intrinsic_height = kasasa_dmabuf_paintable_get_intrinsic_height;
  iface->get_intrinsic_aspect_ratio =
    kasasa_dmabuf_paintable_get_intrinsic_aspect_ratio;
}

KasasaDmabufPaintable *
kasasa_dmabuf_paintable_new (void)
{
  return g_object_new (KASASA_TYPE_DMABUF_PAINTABLE, NULL);
}

void
kasasa_dmabuf_paintable_set_texture (KasasaDmabufPaintable *self,
                                     GdkTexture             *texture,
                                     guint32                 transform,
                                     gboolean                y_invert)
{
  KasasaTextureOrientation orientation;
  gint width = 0;
  gint height = 0;
  gboolean size_changed;

  g_return_if_fail (KASASA_IS_DMABUF_PAINTABLE (self));
  g_return_if_fail (texture == NULL || GDK_IS_TEXTURE (texture));

  orientation = texture_orientation (transform, y_invert);
  if (texture != NULL)
    {
      width = gdk_texture_get_width (texture);
      height = gdk_texture_get_height (texture);
      if (orientation_swaps_axes (orientation))
        {
          gint swap = width;

          width = height;
          height = swap;
        }
    }

  size_changed = self->intrinsic_width != width
                 || self->intrinsic_height != height;
  self->orientation = orientation;
  self->intrinsic_width = width;
  self->intrinsic_height = height;
  g_set_object (&self->texture, texture);

  if (size_changed)
    gdk_paintable_invalidate_size (GDK_PAINTABLE (self));
  gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
}
