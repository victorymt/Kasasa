/* kasasa-window.c
 *
 * Copyright 2024-2026 Kelvin Novais
 * Copyright 2026 victorymt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <math.h>

#include "kasasa-content-container.h"
#include "kasasa-source.h"
#include "kasasa-window.h"
#include "kasasa-zoom.h"

// Defined on GSchema and preferences
#define MIN_OCCUPY_SCREEN 0.1

struct _KasasaWindow
{
  AdwApplicationWindow parent_instance;

  /* Template widgets */
  KasasaContentContainer *content_container;
  AdwHeaderBar *header_bar;
  GtkRevealer *header_bar_revealer;
  GtkMenuButton *menu_button;
  GtkToggleButton *auto_discard_button;
  GtkToggleButton *auto_trash_button;
  GtkToggleButton *lock_button;
  GtkProgressBar *progress_bar;
  GtkStack *stack;

  /* State variables */
  gboolean hide_menu_requested;
  gboolean mouse_over_window;
  gboolean hiding_window;
  gboolean block_miniaturization;
  gboolean window_is_miniaturized;
  gboolean pending_resize;
  gboolean first_resize;
  gboolean carousel_locked_for_resize;
  gboolean zoom_scheduled; /* coalesce wheel events to one apply per frame */
  KasasaScrollAxis scroll_axis;
  gdouble  zoom_factor;
  gdouble  zoom_min;
  gdouble  zoom_max;
  gdouble  pending_content_height;
  gdouble  pending_content_width;
  gdouble  zoom_current_width;
  gdouble  zoom_current_height;
  gdouble  zoom_to_width;
  gdouble  zoom_to_height;
  gint64   zoom_last_frame_time;
  guint    zoom_tick_id;
  gint     resize_from_width;
  gint     resize_from_height;
  gint     resize_to_width;
  gint     resize_to_height;

  /* Instance variables */
  GSettings *settings;
  AdwAnimation *window_opacity_animation;
  AdwAnimation *resize_animation;
  KasasaSource hide_header_bar_source;
  KasasaSource hide_toolbar_source;
  KasasaSource reveal_header_bar_source;
  KasasaSource miniaturization_source;
  guint auto_discard_source;
};

typedef struct
{
  HideWindowCallback function;
  GWeakRef weak_data;
  gboolean has_data;
} HideWindowCallbackInfo;

G_DEFINE_FINAL_TYPE (KasasaWindow, kasasa_window, ADW_TYPE_APPLICATION_WINDOW)

void
kasasa_window_take_first_screenshot (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_content_container_request_first_screenshot (self->content_container);
}

void
kasasa_window_take_first_screencast (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_content_container_request_first_screencast (self->content_container);
}

void
kasasa_window_request_screencast (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_content_container_request_screencast (self->content_container);
}

void
kasasa_window_load_first_screenshot_uri (KasasaWindow *self,
                                         const gchar  *uri)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  g_return_if_fail (uri != NULL);
  kasasa_content_container_load_first_screenshot_uri (self->content_container,
                                                      uri);
}

void
kasasa_window_load_first_hyprland_screencast (KasasaWindow *self,
                                              guint32       window_handle,
                                              gint          width,
                                              gint          height)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_content_container_load_first_hyprland_screencast (self->content_container,
                                                           window_handle,
                                                           width,
                                                           height);
}

void
kasasa_window_load_first_hyprland_monitor_screencast (KasasaWindow *self,
                                                      const gchar  *monitor_name,
                                                      gint          width,
                                                      gint          height)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  g_return_if_fail (monitor_name != NULL && *monitor_name != '\0');
  kasasa_content_container_load_first_hyprland_monitor_screencast (
    self->content_container,
    monitor_name,
    width,
    height);
}

void
kasasa_window_cancel_delayed_screenshot (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_content_container_cancel_delayed_screenshot (self->content_container);
}

KasasaWindow *
kasasa_window_get_window_reference (GtkWidget *widget)
{
  GtkRoot *root = NULL;
  KasasaWindow *window = NULL;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

  root = gtk_widget_get_root (GTK_WIDGET (widget));
  window = KASASA_WINDOW (root);

  g_return_val_if_fail (KASASA_IS_WINDOW (window), NULL);

  return window;
}

gboolean
kasasa_window_get_trash_button_active (KasasaWindow *window)
{
  g_return_val_if_fail (KASASA_IS_WINDOW (window), FALSE);
  return gtk_toggle_button_get_active (window->auto_trash_button);
}

gboolean
kasasa_window_is_miniaturized (KasasaWindow *self)
{
  g_return_val_if_fail (KASASA_IS_WINDOW (self), FALSE);

  return self->window_is_miniaturized;
};

static gboolean
has_different_scalings (gdouble *max_scale)
{
  GdkDisplay *display = NULL;
  GListModel *monitors = NULL;
  GObject *monitor = NULL;
  gdouble min_s, max_s, current_scale;
  guint n_items;

  display = gdk_display_get_default ();
  if (display == NULL)
    {
      g_warning ("Can't check for different scalings");
      return FALSE;
    }

  monitors = gdk_display_get_monitors (display);
  n_items = g_list_model_get_n_items (monitors);
  g_info ("Number of monitors: %u", n_items);

  if (n_items <= 1)
    {
      g_info ("Detected %u monitor(s); no different scales to reconcile", n_items);
      return FALSE;
    }

  monitor = g_list_model_get_object (monitors, 0);
  min_s = max_s = gdk_monitor_get_scale (GDK_MONITOR (monitor));
  g_object_unref (monitor);

  for (guint i = 1; i < n_items; i++)
    {
      monitor = g_list_model_get_object (monitors, i);
      current_scale = gdk_monitor_get_scale (GDK_MONITOR (monitor));

      min_s = MIN (current_scale, min_s);
      max_s = MAX (current_scale, max_s);

      g_object_unref (monitor);
    }

  if (min_s != max_s)
    {
      g_info ("Monitors have different scales: %.2f and %.2f [min, max]",
              min_s, max_s);
      *max_scale = max_s;
      return TRUE;
    }
  else
    {
      g_info ("Monitors have same scales");
      return FALSE;
    }
}

static gboolean
scaling (GtkWidget *widget,
         gdouble *scale)
{
  GdkDisplay *display = NULL;
  GtkNative *native = NULL;
  GdkSurface *surface = NULL;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), TRUE);

  display = gdk_display_get_default ();
  if (display == NULL)
    {
      g_warning ("Couldn't get GdkDisplay");
      return TRUE;
    }

  native = gtk_widget_get_native (widget);
  if (native == NULL)
    {
      g_warning ("Couldn't get GtkNative");
      return TRUE;
    }

  surface = gtk_native_get_surface (native);
  if (surface == NULL)
    {
      g_warning ("Couldn't get GdkSurface");
      return TRUE;
    }

  *scale = gdk_surface_get_scale (surface);

  return FALSE;
}

static gboolean
monitor_size (GtkWidget *widget,
              gdouble *monitor_width,
              gdouble *monitor_height)
{
  GdkDisplay *display = NULL;
  GtkNative *native = NULL;
  GdkSurface *surface = NULL;
  GdkMonitor *monitor = NULL;
  GdkRectangle monitor_geometry;

  g_return_val_if_fail (GTK_IS_WIDGET (widget), TRUE);

  display = gdk_display_get_default ();
  if (display == NULL)
    {
      g_warning ("Couldn't get GdkDisplay");
      return TRUE;
    }

  native = gtk_widget_get_native (widget);
  if (native == NULL)
    {
      g_warning ("Couldn't get GtkNative");
      return TRUE;
    }

  surface = gtk_native_get_surface (native);
  if (surface == NULL)
    {
      g_warning ("Couldn't get GdkSurface");
      return TRUE;
    }

  monitor = gdk_display_get_monitor_at_surface (display, surface);
  if (monitor == NULL)
    {
      g_warning ("Couldn't get GdkMonitor");
      return TRUE;
    }

  gdk_monitor_get_geometry (monitor, &monitor_geometry);

  *monitor_width = monitor_geometry.width;
  *monitor_height = monitor_geometry.height;

  return FALSE;
}

// Compute the window size
// Based on:
// https://gitlab.gnome.org/GNOME/Incubator/showtime/-/blob/main/showtime/window.py?ref_type=heads#L836
// https://gitlab.gnome.org/GNOME/loupe/-/blob/4ca5f9e03d18667db5d72325597cebc02887777a/src/widgets/image/rendering.rs#L151
static gboolean
compute_size (KasasaWindow *self,
              gdouble *nat_width,
              gdouble *nat_height,
              const gint content_height,
              const gint content_width)
{
  gdouble image_width, image_height, image_area, max_width, max_height,
      monitor_width, monitor_height, monitor_area,
      occupy_area_factor, size_scale, target_scale, hidpi_scale, max_scale,
      content_scale,
      min_zoom, max_zoom, zoom_lower, zoom_upper;

  g_autoptr (GSettings) settings = g_settings_new ("io.github.kelvinnovais.Kasasa");

  if (content_height <= 0 || content_width <= 0)
    {
      g_warning ("Content width or height must be > 0");
      return TRUE;
    }

  if (monitor_size (GTK_WIDGET (self), &monitor_width, &monitor_height))
    {
      g_warning ("Couldn't get monitor size");
      return TRUE;
    }

  if (!isfinite (monitor_width) || !isfinite (monitor_height)
      || monitor_width <= 0.0 || monitor_height <= 0.0)
    {
      g_warning ("Monitor width and height must be finite and > 0");
      return TRUE;
    }

  if (scaling (GTK_WIDGET (self), &hidpi_scale))
    {
      g_warning ("Couldn't get HiDPI scale");
      return TRUE;
    }

  if (!isfinite (hidpi_scale) || hidpi_scale <= 0.0)
    {
      g_warning ("HiDPI scale must be finite and > 0");
      return TRUE;
    }

  // If the user has different scales for the monitors and the current scale is
  // less than the max scale, divide the image dimentions by the max scale. This
  // is needed because the screenshot size follows the max scale
  content_scale = hidpi_scale;
  if (has_different_scalings (&max_scale))
    {
      if (!isfinite (max_scale) || max_scale <= 0.0)
        {
          g_warning ("Maximum monitor scale must be finite and > 0");
          return TRUE;
        }

      content_scale = max_scale;
    }

  if (!kasasa_zoom_get_logical_content_size (content_width,
                                             content_height,
                                             content_scale,
                                             &image_width,
                                             &image_height))
    {
      g_warning ("Scaled content width and height must be finite and > 0");
      return TRUE;
    }

  // AREAS
  monitor_area = monitor_width * monitor_height;
  image_area = image_height * image_width;

  occupy_area_factor = g_settings_get_int (settings, "occupy-screen") / 100.0;
  if (!isfinite (occupy_area_factor) || occupy_area_factor <= 0.0)
    {
      g_warning ("Screen occupation factor must be finite and > 0");
      return TRUE;
    }

  // factor for width and height that will achieve the desired area
  // occupation derived from:
  // monitor_area * occupy_area_factor ==
  //   (image_width * size_scale) * (image_height * size_scale)
  size_scale = sqrt (monitor_area / image_area * occupy_area_factor);
  if (!isfinite (size_scale))
    {
      g_warning ("Couldn't compute a finite content scale");
      return TRUE;
    }
  g_debug ("size_scale @ %d: %f", __LINE__, size_scale);
  // ensure that size_scale is not ~ 0 (if image is too big, size_scale can reach 0)
  size_scale = MAX (size_scale, MIN_OCCUPY_SCREEN);
  g_debug ("size_scale @ %d: %f", __LINE__, size_scale);
  // ensure that we never increase image size
  target_scale = MIN (1, size_scale);
  g_debug ("target_scale @ %d: %f", __LINE__, target_scale);
  *nat_width = image_width * target_scale;
  *nat_height = image_height * target_scale;
  g_debug ("[nat_width, nat_height] @ %d: [%f, %f]",
           __LINE__, *nat_width, *nat_height);

  // Scale down if targeted occupation does not fit horizontally
  // Add some margin to not touch corners
  max_width = MAX (1.0, monitor_width /*- 20*/);
  if (*nat_width > max_width)
    {
      *nat_width = max_width;
      *nat_height = image_height * (*nat_width) / image_width;
      g_debug ("[nat_width, nat_height] @ %d: [%f, %f]",
               __LINE__, *nat_width, *nat_height);
    }

  // Same for vertical size. The header bar overlays the screenshot, so only
  // reserve space for the desktop shell here.
  max_height = MAX (1.0, monitor_height - 35 /*+ 20*/);
  if (*nat_height > max_height)
    {
      *nat_height = max_height;
      *nat_width = image_width * (*nat_height) / image_height;
      g_debug ("[nat_width, nat_height] @ %d: [%f, %f]",
               __LINE__, *nat_width, *nat_height);
    }

  // Clamp the stored zoom to values that can actually change the window. This
  // prevents invisible zoom from accumulating at screen/minimum-size limits.
  min_zoom = MAX (WINDOW_MIN_WIDTH / *nat_width,
                  WINDOW_MIN_HEIGHT / *nat_height);
  max_zoom = MIN (max_width / *nat_width,
                  max_height / *nat_height);
  zoom_lower = MAX (WINDOW_ZOOM_MIN, min_zoom);
  zoom_upper = MIN (WINDOW_ZOOM_MAX, max_zoom);
  if (zoom_lower > zoom_upper)
    zoom_lower = zoom_upper;

  self->zoom_min = zoom_lower;
  self->zoom_max = zoom_upper;
  self->zoom_factor = CLAMP (self->zoom_factor, zoom_lower, zoom_upper);

  // User zoom is relative to the auto-fitted size (occupy-screen base)
  *nat_width *= self->zoom_factor;
  *nat_height *= self->zoom_factor;
  g_debug ("[nat_width, nat_height] after zoom (%.2f) @ %d: [%f, %f]",
           self->zoom_factor, __LINE__, *nat_width, *nat_height);

  // Re-clamp to the monitor after zoom so the window never exceeds the screen
  if (*nat_width > max_width)
    {
      *nat_height = *nat_height * (max_width / *nat_width);
      *nat_width = max_width;
    }
  if (*nat_height > max_height)
    {
      *nat_width = *nat_width * (max_height / *nat_height);
      *nat_height = max_height;
    }

  if (!isfinite (*nat_width) || !isfinite (*nat_height)
      || *nat_width <= 0.0 || *nat_height <= 0.0)
    {
      g_warning ("Computed window width and height must be finite and > 0");
      return TRUE;
    }

  *nat_width = round (*nat_width);
  *nat_height = round (*nat_height);
  g_debug ("[nat_width, nat_height] @ %d: [%f, %f]",
           __LINE__, *nat_width, *nat_height);

  // Ensure that the scaled image isn't smaller than the min window size
  *nat_width = MAX (WINDOW_MIN_WIDTH, *nat_width);
  *nat_height = MAX (WINDOW_MIN_HEIGHT, *nat_height);

  g_info ("Logical monitor dimensions: %.2f x %.2f",
          monitor_width, monitor_height);
  g_info ("HiDPI scale: %.2f", hidpi_scale);
  g_info ("Image dimensions: %.2f x %.2f", image_width, image_height);
  g_info ("Zoom factor: %.2f", self->zoom_factor);
  g_info ("Scaled image dimensions: %.2f x %.2f", *nat_width, *nat_height);

  return FALSE;
}

/*
 * Read the size the user actually sees. Mid-tween, default-width/height can
 * already be the animation target while the widget is still at another size.
 */
static void
kasasa_window_get_visual_size (KasasaWindow *self,
                               gint         *width,
                               gint         *height)
{
  gint w = gtk_widget_get_width (GTK_WIDGET (self));
  gint h = gtk_widget_get_height (GTK_WIDGET (self));

  if (w < 2 || h < 2)
    gtk_window_get_default_size (GTK_WINDOW (self), &w, &h);

  *width = w;
  *height = h;
}

static void
kasasa_window_set_resize_lock (KasasaWindow *self,
                               gboolean      locked)
{
  if (self->carousel_locked_for_resize == locked)
    return;

  self->carousel_locked_for_resize = locked;
  kasasa_content_container_carousel_set_interactive (self->content_container,
                                                      !locked);
}

static void
kasasa_window_stop_resize_animations (KasasaWindow *self)
{
  gint w, h;

  if (!ADW_IS_ANIMATION (self->resize_animation))
    {
      kasasa_window_set_resize_lock (self, FALSE);
      return;
    }

  /* Freeze at the on-screen size before dropping tweens — otherwise the
   * default-size property can jump to the old animation's end value and the
   * window edges flash. */
  kasasa_window_get_visual_size (self, &w, &h);

  adw_animation_pause (self->resize_animation);
  g_clear_object (&self->resize_animation);

  gtk_window_set_default_size (GTK_WINDOW (self), w, h);
  kasasa_window_set_resize_lock (self, FALSE);
}

static void
on_resize_animation_value (gdouble  value,
                           gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  gint height;
  gint width;

  width = (gint) round (self->resize_from_width
                        + (self->resize_to_width
                           - self->resize_from_width) * value);
  height = (gint) round (self->resize_from_height
                         + (self->resize_to_height
                            - self->resize_from_height) * value);

  gtk_window_set_default_size (GTK_WINDOW (self),
                               MAX (width, 1),
                               MAX (height, 1));
}

static void
on_resize_animation_done (AdwAnimation *animation,
                          gpointer      user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  gtk_window_set_default_size (GTK_WINDOW (self),
                               self->resize_to_width,
                               self->resize_to_height);
  kasasa_window_set_resize_lock (self, FALSE);
}

/*
 * Apply window geometry for first map / hard snap only.
 * Avoid calling this on every zoom tick — size-request churn flashes edges.
 */
static void
kasasa_window_apply_geometry (KasasaWindow *self,
                              gint          width,
                              gint          height)
{
  width = MAX (width, 1);
  height = MAX (height, 1);

  gtk_window_set_default_size (GTK_WINDOW (self), width, height);
}

static void
kasasa_window_stop_zoom_follow (KasasaWindow *self)
{
  if (self->zoom_tick_id != 0)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->zoom_tick_id);
      self->zoom_tick_id = 0;
    }

  self->zoom_scheduled = FALSE;
  self->zoom_current_width = 0.0;
  self->zoom_current_height = 0.0;
  self->zoom_to_width = 0.0;
  self->zoom_to_height = 0.0;
  self->zoom_last_frame_time = 0;
}

/*
 * Resize the window.
 *
 * Zoom updates a persistent frame follower's target. It never restarts easing
 * during continuous input, and width+height are still written together.
 */
static void
kasasa_window_resize_window_internal (KasasaWindow *self,
                                      gdouble       new_height,
                                      gdouble       new_width,
                                      gboolean      animate,
                                      gboolean      from_zoom)
{
  AdwAnimationTarget *target = NULL;
  gint from_width, from_height;
  gint target_w_px, target_h_px;
  gboolean use_animation;

  use_animation = animate && !self->first_resize;

  target_w_px = (gint) round (new_width);
  target_h_px = (gint) round (new_height);

  if (from_zoom && use_animation)
    {
      if (self->zoom_current_width <= 0.0
          || self->zoom_current_height <= 0.0)
        {
          kasasa_window_stop_resize_animations (self);
          kasasa_window_get_visual_size (self, &from_width, &from_height);
          self->zoom_current_width = from_width;
          self->zoom_current_height = from_height;
          self->zoom_last_frame_time = 0;
        }

      self->zoom_to_width = MAX (target_w_px, 1);
      self->zoom_to_height = MAX (target_h_px, 1);
      kasasa_window_set_resize_lock (self, TRUE);
      return;
    }

  /* Content changes and miniaturization supersede an active zoom gesture. */
  kasasa_window_stop_zoom_follow (self);
  kasasa_window_stop_resize_animations (self);

  if (!use_animation)
    {
      // Snap immediately — first show / hard snap only
      kasasa_window_apply_geometry (self, target_w_px, target_h_px);
      self->first_resize = FALSE;
      return;
    }

  kasasa_window_get_visual_size (self, &from_width, &from_height);

  // Already there (e.g. repeated zoom coalesce to same size)
  if (from_width == target_w_px && from_height == target_h_px)
    return;

  // Disable the carousel navigation while the window is being resized
  kasasa_window_set_resize_lock (self, TRUE);

  /* Keep width and height on one timeline and submit one configure request per
   * frame. Independent property animations visibly oscillate on Wayland. */
  gtk_window_set_default_size (GTK_WINDOW (self), from_width, from_height);

  self->resize_from_width = from_width;
  self->resize_from_height = from_height;
  self->resize_to_width = target_w_px;
  self->resize_to_height = target_h_px;
  target = adw_callback_animation_target_new (on_resize_animation_value,
                                               self,
                                               NULL);
  self->resize_animation = adw_timed_animation_new (
      GTK_WIDGET (self),
      0.0,
      1.0,
      WINDOW_RESIZING_DURATION,
      target);
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->resize_animation),
                                  ADW_EASE_OUT_CUBIC);

  g_signal_connect (self->resize_animation, "done",
                    G_CALLBACK (on_resize_animation_done), self);
  adw_animation_play (self->resize_animation);
}

// Resize the window with an animation (content change, miniaturize, …)
void
kasasa_window_resize_window (KasasaWindow *self,
                             gdouble new_height,
                             gdouble new_width)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  kasasa_window_resize_window_internal (self, new_height, new_width,
                                        TRUE, FALSE);
}

static gboolean
resize_window_scaling (KasasaWindow *self,
                       gdouble       new_height,
                       gdouble       new_width,
                       gboolean      animate,
                       gboolean      from_zoom)
{
  gdouble nat_width = -1.0;
  gdouble nat_height = -1.0;

  g_return_val_if_fail (KASASA_IS_WINDOW (self), FALSE);

  // Remember the last content pixel size so we can re-fit after map / zoom
  self->pending_content_height = new_height;
  self->pending_content_width = new_width;

  // On Wayland the surface/monitor often is not ready while the window is still
  // hidden for the portal screenshot. Defer until mapped instead of applying a
  // broken size that later "jumps" on the first scroll.
  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      self->pending_resize = TRUE;
      g_debug ("Deferring window resize until mapped");
      return FALSE;
    }

  if (compute_size (self,
                    &nat_width, &nat_height,
                    new_height, new_width))
    {
      // Transient failure (no surface yet, etc.): retry on next map
      self->pending_resize = TRUE;
      g_warning ("Window resize skipped; will retry when mapped");
      return FALSE;
    }

  self->pending_resize = FALSE;

  kasasa_window_resize_window_internal (self, nat_height, nat_width,
                                        animate, from_zoom);
  return TRUE;
}

gboolean
kasasa_window_resize_window_scaling (KasasaWindow *self,
                                     gdouble       new_height,
                                     gdouble       new_width)
{
  return resize_window_scaling (self, new_height, new_width, TRUE, FALSE);
}

gboolean
kasasa_window_resize_window_scaling_for_zoom (KasasaWindow *self,
                                              gdouble       new_height,
                                              gdouble       new_width,
                                              gboolean      continuous)
{
  return resize_window_scaling (self,
                                new_height,
                                new_width,
                                continuous,
                                continuous);
}

gboolean
kasasa_window_resize_for_content_switch (KasasaWindow          *self,
                                         gdouble                new_height,
                                         gdouble                new_width,
                                         KasasaSwitchResizeMode mode)
{
  gdouble base_height;
  gdouble base_width;
  gdouble base_zoom;
  gdouble desired_zoom;
  gdouble previous_zoom;
  gint current_height;
  gint current_width;

  g_return_val_if_fail (KASASA_IS_WINDOW (self), FALSE);

  switch (mode)
    {
    case KASASA_SWITCH_RESIZE_FIT:
      return kasasa_window_resize_window_scaling (self,
                                                  new_height,
                                                  new_width);
    case KASASA_SWITCH_RESIZE_KEEP_WIDTH:
    case KASASA_SWITCH_RESIZE_KEEP_HEIGHT:
      break;
    default:
      g_return_val_if_reached (FALSE);
    }

  /* The first page has no meaningful visible dimension to preserve. */
  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return kasasa_window_resize_window_scaling (self,
                                                new_height,
                                                new_width);

  kasasa_window_get_visual_size (self, &current_width, &current_height);
  previous_zoom = self->zoom_factor;
  self->zoom_factor = 1.0;

  if (compute_size (self,
                    &base_width,
                    &base_height,
                    new_height,
                    new_width))
    {
      self->zoom_factor = previous_zoom;
      return kasasa_window_resize_window_scaling (self,
                                                  new_height,
                                                  new_width);
    }

  /* compute_size may clamp 100% for unusually small or large content. */
  base_zoom = self->zoom_factor;
  if (mode == KASASA_SWITCH_RESIZE_KEEP_WIDTH)
    desired_zoom = base_zoom * current_width / base_width;
  else
    desired_zoom = base_zoom * current_height / base_height;

  self->zoom_factor = CLAMP (desired_zoom, self->zoom_min, self->zoom_max);
  return kasasa_window_resize_window_scaling (self,
                                              new_height,
                                              new_width);
}

void
kasasa_window_reset_zoom (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));
  self->zoom_factor = 1.0;
}

static void
kasasa_window_apply_pending_resize (KasasaWindow *self)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));

  if (!self->pending_resize)
    return;

  if (self->pending_content_height <= 0 || self->pending_content_width <= 0)
    {
      // Fall back to whatever content is currently shown
      kasasa_content_container_request_window_resize (self->content_container);
      return;
    }

  kasasa_window_resize_window_scaling (self,
                                       self->pending_content_height,
                                       self->pending_content_width);
}

static void
apply_pending_resize_idle (gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (KASASA_IS_WINDOW (self))
    kasasa_window_apply_pending_resize (self);

  g_object_unref (self);
}

static void
on_window_map (GtkWidget *widget,
               gpointer   user_data)
{
  KasasaWindow *self = KASASA_WINDOW (widget);

  if (!self->pending_resize)
    return;

  // Idle so allocation / monitor info are fully available on Wayland
  g_idle_add_once (apply_pending_resize_idle, g_object_ref (self));
}

static void
window_opacity_progress_cb (double        value,
                            KasasaWindow *self)
{
  gtk_widget_set_opacity (GTK_WIDGET (self), value);
}

static void
content_opacity_progress_cb (double        value,
                             KasasaWindow *self)
{
  gtk_widget_set_opacity (GTK_WIDGET (self->content_container), value);
}

void
kasasa_window_change_opacity (KasasaWindow *self,
                              Opacity opacity_direction)
{
  AdwAnimationTarget *target = NULL;
  gdouble opacity = g_settings_get_double (self->settings, "opacity");

  // Set from and to target values, according to the mode (increase or decrease opacity)
  gdouble from = gtk_widget_get_opacity (GTK_WIDGET (self->content_container));
  gdouble to = (opacity_direction == OPACITY_INCREASE) ? 1.00 : opacity;

  // Return if this option is disabled
  if (!g_settings_get_boolean (self->settings, "change-opacity"))
    return;

  // Return if the window is hiding/hidden when retaking the screenshot
  // it prevents the opacity increase again if the mouse leave the window
  if (self->hiding_window)
    return;

  // Motion controllers can report the same target repeatedly while crossing
  // overlay children. Do not restart an identical animation on every event.
  if (ABS (from - to) < 0.0001)
    return;

  if (ADW_IS_TIMED_ANIMATION (self->window_opacity_animation)
      && adw_animation_get_state (self->window_opacity_animation)
           == ADW_ANIMATION_PLAYING
      && ABS (adw_timed_animation_get_value_to (
                ADW_TIMED_ANIMATION (self->window_opacity_animation)) - to)
           < 0.0001)
    return;

  // Pause an animation
  // The "if" verifies if the animation was called at least once
  if (ADW_IS_ANIMATION (self->window_opacity_animation))
    adw_animation_pause (self->window_opacity_animation);

  g_clear_object (&self->window_opacity_animation);

  target =
      adw_callback_animation_target_new ((AdwAnimationTargetFunc) content_opacity_progress_cb,
                                         self,
                                         NULL);

  self->window_opacity_animation = adw_timed_animation_new (
      GTK_WIDGET (self), // widget
      from, to,          // opacity from to
      270,               // duration
      target             // target
  );

  adw_animation_play (self->window_opacity_animation);
}

static void
on_window_hidden (AdwAnimation *animation,
                   gpointer user_data)
{
  HideWindowCallbackInfo *cb = user_data;
  gpointer data = cb->has_data ? g_weak_ref_get (&cb->weak_data) : NULL;

  if (!cb->has_data || data != NULL)
    cb->function (data);

  if (data != NULL)
    g_object_unref (data);
}

static void
hide_window_callback_info_free (gpointer user_data,
                                GClosure *closure)
{
  HideWindowCallbackInfo *cb = user_data;

  if (cb->has_data)
    g_weak_ref_clear (&cb->weak_data);
  g_free (cb);
}

/*
 * Hide/reveal the window by changing its opacity
 * Hide if 'hide' is TRUE
 * Reveal if 'hide' is FALSE
 *
 * This trick is required because by using gtk_widget_set_visible (window, FALSE),
 * cause the window to be unpinned.
 *
 * Optionally, this functon can receive a 'callback', that is called when hiding
 * window is finished. This argument can be NULL, as well 'callback_data'.
 */
void
kasasa_window_hide_window (KasasaWindow *self,
                           gboolean hide,
                           HideWindowCallback callback,
                           GObject *callback_data)
{
  AdwAnimationTarget *target = NULL;
  gdouble from, to;

  g_return_if_fail (KASASA_IS_WINDOW (self));

  // Pause an animation
  // The "if" verifies if the animation was called at least once
  if (ADW_IS_ANIMATION (self->window_opacity_animation))
    {
      adw_animation_pause (self->window_opacity_animation);
      g_clear_object (&self->window_opacity_animation);
    }

  // Set 'from' and 'to' target values
  from = gtk_widget_get_opacity (GTK_WIDGET (self));
  to = (hide) ? 0.00 : 1.00;

  // Set if the window is hiding or being revealed
  self->hiding_window = hide;

  target =
      adw_callback_animation_target_new ((AdwAnimationTargetFunc) window_opacity_progress_cb,
                                         self,
                                         NULL);

  self->window_opacity_animation = adw_timed_animation_new (
      GTK_WIDGET (self),                     // widget
      from, to,                              // opacity from to
      (hide) ? WINDOW_HIDING_DURATION : 200, // duration
      target                                 // target
  );

  if (callback != NULL)
    {
      HideWindowCallbackInfo *cb_info = g_new0 (HideWindowCallbackInfo, 1);
      cb_info->function = callback;
      cb_info->has_data = callback_data != NULL;
      if (cb_info->has_data)
        g_weak_ref_init (&cb_info->weak_data, callback_data);
      g_signal_connect_data (self->window_opacity_animation, "done",
                             G_CALLBACK (on_window_hidden), cb_info,
                             hide_window_callback_info_free, 0);
    }

  adw_animation_play (self->window_opacity_animation);
}

static gboolean
auto_discard_window_cb (gpointer user_data)
{
  gdouble new_fraction = 0;
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (!gtk_toggle_button_get_active (self->auto_discard_button))
    {
      self->auto_discard_source = 0;
      gtk_progress_bar_set_fraction (self->progress_bar, 0.0);
      return G_SOURCE_REMOVE;
    }

  new_fraction = gtk_progress_bar_get_fraction (self->progress_bar) - 0.005;

  if (new_fraction <= 0)
    {
      self->auto_discard_source = 0;
      gtk_window_close (GTK_WINDOW (self));
      return G_SOURCE_REMOVE;
    }
  else
    {
      gtk_progress_bar_set_fraction (self->progress_bar, new_fraction);
    }

  return G_SOURCE_CONTINUE;
}

void
kasasa_window_auto_discard_window (KasasaWindow *self)
{
  gdouble time_seconds;
  guint time_miliseconds;

  g_return_if_fail (KASASA_IS_WINDOW (self));

  time_seconds = 60 * g_settings_get_double (self->settings,
                                             "auto-discard-window-time");
  // We are goning to decrease the ProgressBar 0.005 (or 0.5%) each function call
  time_miliseconds = (guint) ((time_seconds / 200) * 1000);

  if (self->auto_discard_source != 0)
    g_source_remove (self->auto_discard_source);

  gtk_progress_bar_set_fraction (self->progress_bar, 1.0);
  self->auto_discard_source =
    g_timeout_add (time_miliseconds, auto_discard_window_cb, self);
}

static gboolean
on_modal_close_request (GtkWindow *window,
                        gpointer user_data)
{
  kasasa_window_miniaturize_window (KASASA_WINDOW (user_data), TRUE);
  return FALSE;
}

/**
 * This function recognizes if there's a Preferences/About dialog (modals);
 * Since the window is not resizeble, a dialog can presented as a transient
 * window, and this function seems to be the only way to get if there's a modal
 * or not
 */
static gboolean
has_modal (KasasaWindow *self)
{
  GListModel *windows = gtk_window_get_toplevels ();
  guint n_items = g_list_model_get_n_items (windows);

  for (guint i = 0; i < n_items; i++)
    {
      gpointer item = g_list_model_get_item (windows, i);

      if (GTK_IS_WINDOW (item) && !GTK_IS_SHORTCUTS_WINDOW (item) && gtk_window_get_modal (GTK_WINDOW (item)))
        {
          g_info ("Window has a modal");
          g_signal_connect (item, "close-request",
                            G_CALLBACK (on_modal_close_request), self);
          g_object_unref (item);
          return TRUE;
        }
      g_object_unref (item);
    }

  return FALSE;
}

static void
window_miniaturization_cb (gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (self->window_is_miniaturized
      || self->block_miniaturization
      || !g_settings_get_boolean (self->settings, "miniaturize-window")
      || gtk_toggle_button_get_active (self->lock_button)
      || has_modal (self))
    return;

  self->window_is_miniaturized = TRUE;
  gtk_stack_set_visible_child_name (self->stack, "miniature_page");
  gtk_widget_add_css_class (GTK_WIDGET (self), "circular-window");
  kasasa_window_resize_window (self, 75, 75);
}

/*
 * If miniaturize == TRUE, this function will miniaturize the window after some time
 *
 * If miniaturize == FALSE, previous requests will be cancelled, and the window
 * will immediately return to its default visual
 */
void
kasasa_window_miniaturize_window (KasasaWindow *self,
                                  gboolean miniaturize)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));

  kasasa_source_clear (&self->miniaturization_source);

  if (miniaturize)
    {
      if (self->window_is_miniaturized || !g_settings_get_boolean (self->settings, "miniaturize-window") || self->block_miniaturization || gtk_toggle_button_get_active (self->lock_button))
        return;

      kasasa_source_set_timeout_seconds_once (&self->miniaturization_source,
                                              WINDOW_MINIATURIZATION_DELAY,
                                              window_miniaturization_cb,
                                              self);
    }
  else
    {
      if (!self->window_is_miniaturized)
        return;

      self->window_is_miniaturized = FALSE;
      kasasa_content_container_request_window_resize (self->content_container);
      gtk_widget_remove_css_class (GTK_WIDGET (self), "circular-window");
      gtk_stack_set_visible_child_name (self->stack, "main_page");
    }
}

void
kasasa_window_block_miniaturization (KasasaWindow *self,
                                     gboolean block)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));

  if (block)
    {
      // Sets a block
      self->block_miniaturization = TRUE;
      // Restore window
      kasasa_window_miniaturize_window (self, FALSE);
    }
  else
    {
      // Remove the block
      self->block_miniaturization = FALSE;
      // Miniaturize window
      kasasa_window_miniaturize_window (self, TRUE);
    }
}

static gboolean
controls_should_remain_visible (KasasaWindow *self)
{
  return self->mouse_over_window
         || kasasa_content_container_controls_active (self->content_container);
}

static void
hide_header_bar_cb (gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  self->hide_menu_requested = FALSE;

  /*
   * Hidding was queried because at some moment the mouse pointer left the window,
   * however, don't hide the HeaderBar if it returned and is still over the window
   */
  if (controls_should_remain_visible (self))
    return;

  gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_bar_revealer), FALSE);
}

static void
hide_toolbar_cb (gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  /*
   * Hidding was queried because at some moment the mouse pointer left the window,
   * however, don't hide the Toolbar if it returned and is still over the window
   */
  if (controls_should_remain_visible (self))
    return;

  kasasa_content_container_reveal_controls (self->content_container, FALSE);
}

static void
reveal_header_bar_cb (gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_bar_revealer), TRUE);
}

static void
hide_header_bar (KasasaWindow *self)
{
  // Hide the vertical menu if this option is enabled
  if (g_settings_get_boolean (self->settings, "auto-hide-menu"))
    // As soon as this action has a delay:
    // if already requested, do nothing; else, request hiding
    if (self->hide_menu_requested == FALSE)
      {
        guint interval = (guint) 1000 * g_settings_get_double (self->settings,
                                                               "controls-timeout");
        self->hide_menu_requested = TRUE;

        kasasa_source_set_timeout_once (&self->hide_header_bar_source,
                                        interval,
                                        hide_header_bar_cb,
                                        self);
      }
}

static void
schedule_toolbar_hide (KasasaWindow *self)
{
  guint interval;

  if (controls_should_remain_visible (self))
    return;

  interval = (guint) (1000 * g_settings_get_double (self->settings,
                                                     "controls-timeout"));
  kasasa_source_set_timeout_once (&self->hide_toolbar_source,
                                  interval,
                                  hide_toolbar_cb,
                                  self);
}

void
kasasa_window_set_controls_popup_active (KasasaWindow *self,
                                         gboolean      active)
{
  g_return_if_fail (KASASA_IS_WINDOW (self));

  kasasa_window_block_miniaturization (self, active);

  if (active)
    {
      kasasa_source_clear (&self->hide_toolbar_source);
      kasasa_source_clear (&self->hide_header_bar_source);
      self->hide_menu_requested = FALSE;
      return;
    }

  if (!self->mouse_over_window)
    {
      schedule_toolbar_hide (self);
      hide_header_bar (self);
    }
}

static void
on_mouse_enter_content_container (GtkEventControllerMotion *event_controller_motion,
                                  gdouble x,
                                  gdouble y,
                                  gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  // See Note [1]
  if (gtk_menu_button_get_active (self->menu_button))
    return;

  kasasa_window_change_opacity (self, OPACITY_DECREASE);

  // Do not reveal HeaderBar/Toolbar if miniaturization is active; this will done
  // after clicking the window
  if (g_settings_get_boolean (self->settings, "miniaturize-window"))
    return;

  if (g_settings_get_boolean (self->settings, "auto-hide-menu"))
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_bar_revealer), TRUE);

  kasasa_content_container_reveal_controls (self->content_container, TRUE);
}

static void
on_mouse_leave_content_container (GtkEventControllerMotion *event_controller_motion,
                                  gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  // See Note [1]
  if (gtk_menu_button_get_active (self->menu_button))
    return;
  if (kasasa_content_container_controls_active (self->content_container))
    return;

  kasasa_window_change_opacity (self, OPACITY_INCREASE);
  hide_header_bar (self);
}

static void
on_mouse_enter_header_bar (GtkEventControllerMotion *event_controller_motion,
                           gdouble x,
                           gdouble y,
                           gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  // See Note [1]
  if (gtk_menu_button_get_active (self->menu_button))
    return;

  kasasa_window_change_opacity (self, OPACITY_INCREASE);
}

// Increase window opacity when the pointer leaves it
static void
on_mouse_leave_header_bar (GtkEventControllerMotion *event_controller_motion,
                           gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  // See Note [1]
  if (gtk_menu_button_get_active (self->menu_button))
    return;

  hide_header_bar (self);
}

static void
on_mouse_enter_window (GtkEventControllerMotion *event_controller_motion,
                       gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  self->mouse_over_window = TRUE;
  kasasa_window_miniaturize_window (self, FALSE);
}

static void
on_mouse_leave_window (GtkEventControllerMotion *event_controller_motion,
                       gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  self->mouse_over_window = FALSE;

  schedule_toolbar_hide (self);

  if (gtk_toggle_button_get_active (self->lock_button))
    return;

  // See Note [1]
  if (kasasa_content_container_controls_active (self->content_container))
    return;

  kasasa_window_miniaturize_window (self, TRUE);
}

static void
on_window_click_released (GtkGestureClick *gesture_click,
                          gint n_press,
                          gdouble x,
                          gdouble y,
                          gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (!g_settings_get_boolean (self->settings, "miniaturize-window"))
    return;

  if (g_settings_get_boolean (self->settings, "auto-hide-menu"))
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_bar_revealer), TRUE);

  kasasa_content_container_reveal_controls (self->content_container, TRUE);
}

static gboolean
zoom_tick_cb (GtkWidget     *widget,
              GdkFrameClock *frame_clock,
              gpointer       user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  gint64 frame_time;
  gdouble elapsed_ms;
  gboolean at_target;
  gint width, height;

  if (self->zoom_scheduled)
    {
      self->zoom_scheduled = FALSE;
      if (!kasasa_content_container_request_zoom_resize (
            self->content_container, TRUE))
        {
          self->zoom_tick_id = 0;
          self->zoom_current_width = 0.0;
          self->zoom_current_height = 0.0;
          self->zoom_to_width = 0.0;
          self->zoom_to_height = 0.0;
          self->zoom_last_frame_time = 0;
          kasasa_window_set_resize_lock (self, FALSE);
          return G_SOURCE_REMOVE;
        }
    }

  if (self->zoom_current_width <= 0.0
      || self->zoom_current_height <= 0.0)
    {
      self->zoom_tick_id = 0;
      kasasa_window_set_resize_lock (self, FALSE);
      return G_SOURCE_REMOVE;
    }

  frame_time = gdk_frame_clock_get_frame_time (frame_clock);
  elapsed_ms = self->zoom_last_frame_time == 0
               ? 1000.0 / 60.0
               : (frame_time - self->zoom_last_frame_time) / 1000.0;
  self->zoom_last_frame_time = frame_time;

  self->zoom_current_width =
    kasasa_zoom_follow_value (self->zoom_current_width,
                              self->zoom_to_width,
                              elapsed_ms);
  self->zoom_current_height =
    kasasa_zoom_follow_value (self->zoom_current_height,
                              self->zoom_to_height,
                              elapsed_ms);

  at_target = ABS (self->zoom_to_width - self->zoom_current_width) < 0.5
              && ABS (self->zoom_to_height - self->zoom_current_height) < 0.5;
  if (at_target)
    {
      self->zoom_current_width = self->zoom_to_width;
      self->zoom_current_height = self->zoom_to_height;
    }

  width = MAX ((gint) round (self->zoom_current_width), 1);
  height = MAX ((gint) round (self->zoom_current_height), 1);
  gtk_window_set_default_size (GTK_WINDOW (self), width, height);

  if (!at_target)
    return G_SOURCE_CONTINUE;

  self->zoom_tick_id = 0;
  self->zoom_current_width = 0.0;
  self->zoom_current_height = 0.0;
  self->zoom_last_frame_time = 0;
  kasasa_window_set_resize_lock (self, FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_scroll_begin (GtkEventControllerScroll *controller,
                 gpointer                  user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  self->scroll_axis = KASASA_SCROLL_AXIS_UNDECIDED;
}

static void
on_scroll_end (GtkEventControllerScroll *controller,
               gpointer                  user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  self->scroll_axis = KASASA_SCROLL_AXIS_UNDECIDED;
}

static void
kasasa_window_schedule_zoom_apply (KasasaWindow *self)
{
  self->zoom_scheduled = TRUE;

  if (self->zoom_tick_id != 0)
    return;

  self->zoom_tick_id =
    gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                  zoom_tick_cb,
                                  self,
                                  NULL);
}

gboolean
kasasa_window_apply_zoom_delta (KasasaWindow   *self,
                                gdouble         delta,
                                KasasaZoomInput input)
{
  gdouble next_zoom;
  gdouble previous_zoom;

  g_return_val_if_fail (KASASA_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (input == KASASA_ZOOM_INPUT_WHEEL
                        || input == KASASA_ZOOM_INPUT_SURFACE,
                        FALSE);

  if (self->window_is_miniaturized)
    kasasa_window_miniaturize_window (self, FALSE);

  previous_zoom = self->zoom_factor;
  next_zoom = kasasa_zoom_apply_delta (previous_zoom,
                                       self->zoom_min,
                                       self->zoom_max,
                                       delta,
                                       input);
  if (ABS (next_zoom - previous_zoom) < 0.000001)
    return FALSE;

  self->zoom_factor = next_zoom;
  g_debug ("Zoom factor: %.3f (%s input)",
           self->zoom_factor,
           input == KASASA_ZOOM_INPUT_SURFACE ? "surface" : "wheel");

  /* Wheel notches can arrive in quick, opposite bursts while the pointer is
   * crossing the pin. Feed both wheel and surface input into the persistent
   * frame follower so those bursts retarget one resize instead of producing
   * a sequence of abrupt 10% geometry jumps. */
  kasasa_window_schedule_zoom_apply (self);
  kasasa_window_change_opacity (self, OPACITY_INCREASE);
  return TRUE;
}

gdouble
kasasa_window_get_zoom_factor (KasasaWindow *self)
{
  g_return_val_if_fail (KASASA_IS_WINDOW (self), 1.0);

  return self->zoom_factor;
}

static gboolean
on_scroll (GtkEventControllerScroll *controller,
           gdouble                   dx,
           gdouble                   dy,
           gpointer                  user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  KasasaZoomInput input;
  KasasaScrollAxis axis;
  GdkScrollUnit unit;

  unit = gtk_event_controller_scroll_get_unit (controller);
  input = unit == GDK_SCROLL_UNIT_SURFACE
          ? KASASA_ZOOM_INPUT_SURFACE
          : KASASA_ZOOM_INPUT_WHEEL;
  axis = kasasa_zoom_classify_scroll (
    dx, dy,
    input == KASASA_ZOOM_INPUT_SURFACE
      ? self->scroll_axis
      : KASASA_SCROLL_AXIS_UNDECIDED);

  if (input == KASASA_ZOOM_INPUT_SURFACE)
    self->scroll_axis = axis;

  // Horizontal and empty gestures belong to the screenshot carousel.
  if (axis != KASASA_SCROLL_AXIS_VERTICAL)
    return GDK_EVENT_PROPAGATE;

  kasasa_window_apply_zoom_delta (self, dy, input);

  return GDK_EVENT_STOP;
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);
  gint offset;

  if ((state & gtk_accelerator_get_default_mod_mask ()) != 0)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Left)
    offset = -1;
  else if (keyval == GDK_KEY_Right)
    offset = 1;
  else
    return GDK_EVENT_PROPAGATE;

  return kasasa_content_container_switch_page (self->content_container,
                                                offset)
         ? GDK_EVENT_STOP
         : GDK_EVENT_PROPAGATE;
}

static void
on_auto_discard_button_toggled (GtkToggleButton *button,
                                gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (gtk_toggle_button_get_active (button))
    kasasa_window_auto_discard_window (self);
  else
    {
      if (self->auto_discard_source != 0)
        {
          g_source_remove (self->auto_discard_source);
          self->auto_discard_source = 0;
        }
      gtk_progress_bar_set_fraction (self->progress_bar, 0.0);
    }
}

static void
on_lock_button_toggled (GtkToggleButton *button,
                        gpointer user_data)
{
  if (gtk_toggle_button_get_active (button))
    {
      gtk_button_set_icon_name (GTK_BUTTON (button), "padlock2-symbolic");
      gtk_widget_set_tooltip_text (GTK_WIDGET (button),
                                   _ ("Unblock miniaturization"));
    }
  else
    {
      gtk_button_set_icon_name (GTK_BUTTON (button), "padlock2-open-symbolic");
      gtk_widget_set_tooltip_text (GTK_WIDGET (button),
                                   _ ("Block miniaturization"));
    }
}

static void
on_settings_updated (GSettings *settings,
                     gchar *key,
                     gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  if (g_strcmp0 (key, "auto-hide-menu") == 0)
    {
      gboolean auto_hide = g_settings_get_boolean (self->settings, "auto-hide-menu");

      if (auto_hide)
        gtk_widget_add_css_class (GTK_WIDGET (self->header_bar), "headerbar-no-dimming");
      else
        gtk_widget_remove_css_class (GTK_WIDGET (self->header_bar), "headerbar-no-dimming");

      if (auto_hide)
        {
          kasasa_source_clear (&self->reveal_header_bar_source);
          hide_header_bar (self);
        }
      else
        {
          kasasa_source_clear (&self->hide_header_bar_source);
          self->hide_menu_requested = FALSE;
          kasasa_source_set_timeout_seconds_once (&self->reveal_header_bar_source,
                                                  2,
                                                  reveal_header_bar_cb,
                                                  self);
        }

    }

  else if (g_strcmp0 (key, "auto-discard-window") == 0)
    {
      // Just change the button state;
      // the button callback signal will trigger the auto discarding
      if (g_settings_get_boolean (self->settings, "auto-discard-window"))
        gtk_toggle_button_set_active (self->auto_discard_button, TRUE);
      else
        gtk_toggle_button_set_active (self->auto_discard_button, FALSE);
    }

  else if (g_strcmp0 (key, "auto-trash-image") == 0)
    {
      if (g_settings_get_boolean (self->settings, "auto-trash-image"))
        gtk_toggle_button_set_active (self->auto_trash_button, TRUE);
      else
        gtk_toggle_button_set_active (self->auto_trash_button, FALSE);
    }

  else if (g_strcmp0 (key, "miniaturize-window") == 0)
    {
      if (g_settings_get_boolean (self->settings, "miniaturize-window"))
        kasasa_window_miniaturize_window (self, TRUE);
      else
        kasasa_window_miniaturize_window (self, FALSE);
    }

  else if (g_strcmp0 (key, "occupy-screen") == 0)
    {
      // Re-fit current content when the user changes the base size preference
      if (!self->window_is_miniaturized)
        kasasa_content_container_request_window_resize (self->content_container);
    }
}

static gboolean
on_close_request (GtkWindow *window,
                  gpointer user_data)
{
  KasasaWindow *self = KASASA_WINDOW (user_data);

  kasasa_content_container_wipe_content (self->content_container);

  return FALSE;
}

static void
kasasa_window_dispose (GObject *kasasa_window)
{
  KasasaWindow *self = KASASA_WINDOW (kasasa_window);

  kasasa_window_stop_zoom_follow (self);

  if (self->auto_discard_source != 0)
    {
      g_source_remove (self->auto_discard_source);
      self->auto_discard_source = 0;
    }
  kasasa_source_clear (&self->hide_header_bar_source);
  kasasa_source_clear (&self->hide_toolbar_source);
  kasasa_source_clear (&self->reveal_header_bar_source);
  kasasa_source_clear (&self->miniaturization_source);

  g_clear_object (&self->settings);
  g_clear_object (&self->window_opacity_animation);
  kasasa_window_stop_resize_animations (self);

  gtk_widget_dispose_template (GTK_WIDGET (kasasa_window), KASASA_TYPE_WINDOW);

  G_OBJECT_CLASS (kasasa_window_parent_class)->dispose (kasasa_window);
}

static void
kasasa_window_finalize (GObject *kasasa_window)
{
  G_OBJECT_CLASS (kasasa_window_parent_class)->finalize (kasasa_window);
}

static void
kasasa_window_class_init (KasasaWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = kasasa_window_dispose;
  object_class->finalize = kasasa_window_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, "/io/github/kelvinnovais/Kasasa/kasasa-window.ui");
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, content_container);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, header_bar_revealer);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, header_bar);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, menu_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, auto_discard_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, auto_trash_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, lock_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, progress_bar);
  gtk_widget_class_bind_template_child (widget_class, KasasaWindow, stack);
}

static void
kasasa_window_init (KasasaWindow *self)
{
  GtkEventController *cc_motion_event_controller = NULL;
  GtkEventController *hb_motion_event_controller = NULL;
  GtkEventController *win_key_event_controller = NULL;
  GtkEventController *win_motion_event_controller = NULL;
  GtkEventController *win_scroll_event_controller = NULL;
  GtkGesture *win_gesture_click = NULL;

  g_type_ensure (KASASA_TYPE_CONTENT_CONTAINER);

  gtk_widget_init_template (GTK_WIDGET (self));

  // Initialize self variables
  self->settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  self->mouse_over_window = FALSE;
  self->hiding_window = FALSE;
  self->pending_resize = FALSE;
  self->first_resize = TRUE;
  self->carousel_locked_for_resize = FALSE;
  self->zoom_scheduled = FALSE;
  self->scroll_axis = KASASA_SCROLL_AXIS_UNDECIDED;
  self->zoom_factor = 1.0;
  self->zoom_min = WINDOW_ZOOM_MIN;
  self->zoom_max = WINDOW_ZOOM_MAX;
  self->pending_content_height = 0;
  self->pending_content_width = 0;
  self->zoom_current_width = 0;
  self->zoom_current_height = 0;
  self->zoom_to_width = 0;
  self->zoom_to_height = 0;
  self->zoom_last_frame_time = 0;
  self->zoom_tick_id = 0;
  self->resize_from_width = 0;
  self->resize_from_height = 0;
  self->resize_to_width = 0;
  self->resize_to_height = 0;
  self->resize_animation = NULL;
  self->hide_header_bar_source.id = 0;
  self->hide_toolbar_source.id = 0;
  self->reveal_header_bar_source.id = 0;
  self->miniaturization_source.id = 0;
  self->auto_discard_source = 0;

  // Clip the resizing screenshot, but leave the top-level window unclipped so
  // libadwaita can repaint its client-side border and shadow atomically.
  gtk_widget_set_overflow (GTK_WIDGET (self->content_container),
                           GTK_OVERFLOW_HIDDEN);

  g_signal_connect (self->settings, "changed", G_CALLBACK (on_settings_updated), self);
  g_signal_connect (self, "map", G_CALLBACK (on_window_map), NULL);

  // PERFFORM ACTIONS ON WIDGETS
  // Auto discard button
  if (g_settings_get_boolean (self->settings, "auto-discard-window"))
    gtk_toggle_button_set_active (self->auto_discard_button, TRUE);

  // Auto trash button
  if (g_settings_get_boolean (self->settings, "auto-trash-image"))
    gtk_toggle_button_set_active (self->auto_trash_button, TRUE);

  if (g_settings_get_boolean (self->settings, "auto-hide-menu"))
    {
      gtk_widget_add_css_class (GTK_WIDGET (self->header_bar), "headerbar-no-dimming");
      gtk_revealer_set_reveal_child (GTK_REVEALER (self->header_bar_revealer), FALSE);
    }

  // Lock button
  g_settings_bind (self->settings,
                   "miniaturize-window",
                   self->lock_button,
                   "visible",
                   G_SETTINGS_BIND_GET);

  // MOTION EVENT CONTROLLERS: Create motion event controllers to monitor when
  // the mouse cursor is over the content container or the menu
  // (I) Content container
  cc_motion_event_controller = gtk_event_controller_motion_new ();
  g_signal_connect (cc_motion_event_controller,
                    "enter",
                    G_CALLBACK (on_mouse_enter_content_container),
                    self);
  g_signal_connect (cc_motion_event_controller,
                    "leave",
                    G_CALLBACK (on_mouse_leave_content_container),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self->content_container), cc_motion_event_controller);

  // (II) HeaderBar
  hb_motion_event_controller = gtk_event_controller_motion_new ();
  g_signal_connect (hb_motion_event_controller,
                    "enter",
                    G_CALLBACK (on_mouse_enter_header_bar),
                    self);
  g_signal_connect (hb_motion_event_controller,
                    "leave",
                    G_CALLBACK (on_mouse_leave_header_bar),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self->header_bar),
                             hb_motion_event_controller);

  // (III) Window
  win_motion_event_controller = gtk_event_controller_motion_new ();
  g_signal_connect (win_motion_event_controller,
                    "enter",
                    G_CALLBACK (on_mouse_enter_window),
                    self);
  g_signal_connect (win_motion_event_controller,
                    "leave",
                    G_CALLBACK (on_mouse_leave_window),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self),
                             win_motion_event_controller);

  win_gesture_click = gtk_gesture_click_new ();
  g_signal_connect (win_gesture_click,
                    "released",
                    G_CALLBACK (on_window_click_released),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self),
                             GTK_EVENT_CONTROLLER (win_gesture_click));

  win_key_event_controller = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (win_key_event_controller,
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (win_key_event_controller,
                    "key-pressed",
                    G_CALLBACK (on_key_pressed),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self), win_key_event_controller);

  // Capture both axes so horizontal-dominant touchpad gestures can continue to
  // AdwCarousel while vertical-dominant gestures zoom the pin.
  win_scroll_event_controller =
      gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  gtk_event_controller_set_propagation_phase (win_scroll_event_controller,
                                              GTK_PHASE_CAPTURE);
  g_signal_connect (win_scroll_event_controller,
                    "scroll-begin",
                    G_CALLBACK (on_scroll_begin),
                    self);
  g_signal_connect (win_scroll_event_controller,
                    "scroll",
                    G_CALLBACK (on_scroll),
                    self);
  g_signal_connect (win_scroll_event_controller,
                    "scroll-end",
                    G_CALLBACK (on_scroll_end),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self), win_scroll_event_controller);

  // SIGNALS
  // Connect buttons to the callbacks
  g_signal_connect (self->auto_discard_button,
                    "toggled",
                    G_CALLBACK (on_auto_discard_button_toggled),
                    self);
  g_signal_connect (self->lock_button,
                    "toggled",
                    G_CALLBACK (on_lock_button_toggled),
                    self);

  // Listen to events
  g_signal_connect (GTK_WINDOW (self),
                    "close-request",
                    G_CALLBACK (on_close_request),
                    self);
}

/* [1] Note:
 * The PopOver of a menu button steals the focus of a motion event controller
 * (when it's under the pointer)
 *
 * This situation in unwanted
 */
