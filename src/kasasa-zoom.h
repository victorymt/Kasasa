/* kasasa-zoom.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define KASASA_ZOOM_STEP 1.10
#define KASASA_ZOOM_SURFACE_PIXELS_PER_STEP 50.0
#define KASASA_ZOOM_FOLLOW_TAU_MS 96.0

typedef enum
{
  KASASA_ZOOM_INPUT_WHEEL,
  KASASA_ZOOM_INPUT_SURFACE,
} KasasaZoomInput;

typedef enum
{
  KASASA_SCROLL_AXIS_UNDECIDED,
  KASASA_SCROLL_AXIS_HORIZONTAL,
  KASASA_SCROLL_AXIS_VERTICAL,
} KasasaScrollAxis;

KasasaScrollAxis kasasa_zoom_classify_scroll (gdouble          dx,
                                               gdouble          dy,
                                               KasasaScrollAxis current_axis);

gboolean kasasa_zoom_get_logical_content_size (gint     content_width,
                                                gint     content_height,
                                                gdouble  scale,
                                                gdouble *logical_width,
                                                gdouble *logical_height);

gdouble kasasa_zoom_apply_delta (gdouble         current,
                                 gdouble         lower,
                                 gdouble         upper,
                                 gdouble         delta,
                                 KasasaZoomInput input);

gdouble kasasa_zoom_follow_value (gdouble current,
                                  gdouble target,
                                  gdouble elapsed_ms);

G_END_DECLS
