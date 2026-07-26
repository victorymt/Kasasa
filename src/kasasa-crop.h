/* kasasa-crop.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  KASASA_CROP_RESULT_INVALID,
  KASASA_CROP_RESULT_EMPTY,
  KASASA_CROP_RESULT_FOUND
} KasasaCropResult;

typedef struct
{
  gint top;
  gint right;
  gint bottom;
  gint left;
  gint width;
  gint height;
} KasasaCrop;

KasasaCropResult kasasa_crop_find_rgb32 (const guint8 *data,
                                         gsize         size,
                                         gint          width,
                                         gint          height,
                                         gsize         stride,
                                         KasasaCrop   *crop);

G_END_DECLS
