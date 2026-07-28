/* test-gtk-window.c
 *
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

#include <adwaita.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <math.h>
#include <unistd.h>

#include "kasasa-application.h"
#include "kasasa-content-container.h"
#include "kasasa-window.h"

typedef struct
{
  AdwCarousel *carousel;
  gboolean close_observed;
  gboolean content_was_empty;
} CloseData;

typedef struct
{
  gint from_width;
  gint from_height;
  gint to_width;
  gint to_height;
  gint previous_width;
  gint previous_height;
  gint previous_allocated_width;
  gint previous_allocated_height;
  gdouble max_progress_delta;
  gdouble max_allocation_progress_delta;
  guint samples;
  guint allocation_samples;
  gboolean reversed;
  gboolean allocation_reversed;
} ResizeTrace;

static GtkWidget *
find_widget_by_id (GtkWidget  *widget,
                   const char *id)
{
  GtkWidget *child;
  const char *widget_id;

  widget_id = gtk_buildable_get_buildable_id (GTK_BUILDABLE (widget));
  if (g_strcmp0 (widget_id, id) == 0)
    return widget;

  for (child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *match = find_widget_by_id (child, id);

      if (match != NULL)
        return match;
    }

  return NULL;
}

static GtkEventController *
find_motion_controller (GtkWidget *widget)
{
  g_autoptr (GListModel) controllers = NULL;
  guint n_controllers;

  controllers = gtk_widget_observe_controllers (widget);
  n_controllers = g_list_model_get_n_items (controllers);
  for (guint i = 0; i < n_controllers; i++)
    {
      GtkEventController *controller = g_list_model_get_item (controllers, i);

      if (GTK_IS_EVENT_CONTROLLER_MOTION (controller))
        return controller;

      g_object_unref (controller);
    }

  return NULL;
}

static void
dispatch_pending_sources (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
}

static void
dispatch_sources_for (guint duration_ms)
{
  gint64 deadline = g_get_monotonic_time ()
                   + (gint64) duration_ms * G_TIME_SPAN_MILLISECOND;

  do
    {
      dispatch_pending_sources ();
      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);
}

static void
count_property_notification (GObject    *object,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
  guint *count = user_data;

  (*count)++;
}

static gboolean
after_close_request (GtkWindow *window,
                     gpointer   user_data)
{
  CloseData *data = user_data;

  data->close_observed = TRUE;
  data->content_was_empty =
    adw_carousel_get_n_pages (data->carousel) == 0;
  return FALSE;
}

static gchar *
create_temp_image_uri (gchar **path_out,
                       gint    width,
                       gint    height)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  gchar *uri;
  gint fd;

  fd = g_file_open_tmp ("kasasa-window-test-XXXXXX.png", path_out, &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);

  pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  g_assert_nonnull (pixbuf);
  gdk_pixbuf_fill (pixbuf, 0x287a5fff);
  g_assert_true (gdk_pixbuf_save (pixbuf,
                                  *path_out,
                                  "png",
                                  &error,
                                  NULL));
  g_assert_no_error (error);
  uri = g_filename_to_uri (*path_out, NULL, &error);
  g_assert_no_error (error);

  return uri;
}

static void
test_quit_wipes_real_window_content (void)
{
  g_autofree gchar *application_id = NULL;
  g_autofree gchar *image_path = NULL;
  g_autofree gchar *image_uri = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  KasasaWindow *window;
  KasasaContentContainer *container;
  CloseData close_data = { 0 };

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_boolean (settings, "auto-hide-menu", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "auto-discard-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings, "auto-trash-image", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "miniaturize-window",
                                         FALSE));

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Window%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", application,
                         NULL);
  container = KASASA_CONTENT_CONTAINER (
    find_widget_by_id (GTK_WIDGET (window), "content_container"));
  close_data.carousel = ADW_CAROUSEL (
    find_widget_by_id (GTK_WIDGET (window), "carousel"));
  g_assert_nonnull (container);
  g_assert_nonnull (close_data.carousel);

  image_uri = create_temp_image_uri (&image_path, 1, 1);
  g_assert_true (kasasa_content_container_append_screenshot (container,
                                                             image_uri,
                                                             &error));
  g_assert_no_error (error);
  g_assert_cmpuint (adw_carousel_get_n_pages (close_data.carousel), ==, 1);

  g_signal_connect_after (window,
                          "close-request",
                          G_CALLBACK (after_close_request),
                          &close_data);
  gtk_window_present (GTK_WINDOW (window));
  dispatch_pending_sources ();

  g_action_group_activate_action (G_ACTION_GROUP (application), "quit", NULL);
  dispatch_pending_sources ();

  g_assert_true (close_data.close_observed);
  g_assert_true (close_data.content_was_empty);
  g_assert_cmpint (g_remove (image_path), ==, 0);
}

static void
test_internal_motion_keeps_controls_visible (void)
{
  g_autofree gchar *application_id = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  g_autoptr (GtkEventController) content_motion = NULL;
  g_autoptr (GtkEventController) window_motion = NULL;
  GtkWidget *content_container;
  GtkRevealer *header_bar_revealer;
  KasasaWindow *window;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_boolean (settings, "auto-hide-menu", TRUE));
  g_assert_true (g_settings_set_double (settings, "controls-timeout", 0.1));
  g_assert_true (g_settings_set_boolean (settings,
                                         "miniaturize-window",
                                         FALSE));

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Motion%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", application,
                         NULL);
  content_container = find_widget_by_id (GTK_WIDGET (window),
                                         "content_container");
  header_bar_revealer = GTK_REVEALER (
    find_widget_by_id (GTK_WIDGET (window), "header_bar_revealer"));
  g_assert_nonnull (content_container);
  g_assert_nonnull (header_bar_revealer);

  window_motion = find_motion_controller (GTK_WIDGET (window));
  content_motion = find_motion_controller (content_container);
  g_assert_nonnull (window_motion);
  g_assert_nonnull (content_motion);

  gtk_window_present (GTK_WINDOW (window));
  dispatch_pending_sources ();

  g_signal_emit_by_name (window_motion, "enter", 10.0, 10.0);
  g_signal_emit_by_name (content_motion, "enter", 10.0, 10.0);
  g_assert_true (gtk_revealer_get_reveal_child (header_bar_revealer));

  /* Moving onto an overlay child leaves the content widget, but not the pin. */
  g_signal_emit_by_name (content_motion, "leave");
  dispatch_sources_for (150);
  g_assert_true (gtk_revealer_get_reveal_child (header_bar_revealer));

  /* A real top-level leave must still hide the controls after the timeout. */
  g_signal_emit_by_name (content_motion, "enter", 10.0, 10.0);
  g_signal_emit_by_name (content_motion, "leave");
  g_signal_emit_by_name (window_motion, "leave");
  dispatch_sources_for (150);
  g_assert_false (gtk_revealer_get_reveal_child (header_bar_revealer));

  gtk_window_destroy (GTK_WINDOW (window));
  dispatch_pending_sources ();
}

static gboolean
wait_for_window_shrink (GtkWindow *window,
                        gint       initial_width,
                        gint      *width_out,
                        gint      *height_out)
{
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  do
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;

      gtk_window_get_default_size (window, width_out, height_out);
      if (*width_out < initial_width
          && *width_out >= WINDOW_MIN_WIDTH
          && *height_out >= WINDOW_MIN_HEIGHT
          && fabs ((gdouble) *width_out / *height_out - 1.5) <= 0.02)
        return TRUE;

      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);

  return FALSE;
}

static gboolean
wait_for_window_allocation (GtkWindow *window,
                            gint       expected_width,
                            gint       expected_height)
{
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  do
    {
      gint height;
      gint width;

      while (g_main_context_iteration (NULL, FALSE))
        ;

      width = gtk_widget_get_width (GTK_WIDGET (window));
      height = gtk_widget_get_height (GTK_WIDGET (window));
      if (ABS (width - expected_width) <= 1
          && ABS (height - expected_height) <= 1)
        return TRUE;

      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);

  return FALSE;
}

static void
trace_resize_progress (GtkWindow  *window,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  ResizeTrace *trace = user_data;
  gdouble height_progress;
  gdouble width_progress;
  gint height;
  gint width;

  gtk_window_get_default_size (window, &width, &height);
  if (width > trace->previous_width || height > trace->previous_height)
    trace->reversed = TRUE;

  width_progress = (gdouble) (trace->from_width - width)
                   / (trace->from_width - trace->to_width);
  height_progress = (gdouble) (trace->from_height - height)
                    / (trace->from_height - trace->to_height);
  trace->max_progress_delta = MAX (trace->max_progress_delta,
                                   ABS (width_progress - height_progress));
  trace->previous_width = width;
  trace->previous_height = height;
  trace->samples++;
}

static gboolean
wait_for_animated_shrink (GtkWindow  *window,
                          ResizeTrace *trace)
{
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  do
    {
      gdouble height_progress;
      gdouble width_progress;
      gint allocated_height;
      gint allocated_width;
      gint default_height;
      gint default_width;

      while (g_main_context_iteration (NULL, FALSE))
        ;

      allocated_width = gtk_widget_get_width (GTK_WIDGET (window));
      allocated_height = gtk_widget_get_height (GTK_WIDGET (window));
      if (allocated_width != trace->previous_allocated_width
          || allocated_height != trace->previous_allocated_height)
        {
          if (allocated_width > trace->previous_allocated_width + 1
              || allocated_height > trace->previous_allocated_height + 1)
            trace->allocation_reversed = TRUE;

          width_progress =
            (gdouble) (trace->from_width - allocated_width)
            / (trace->from_width - trace->to_width);
          height_progress =
            (gdouble) (trace->from_height - allocated_height)
            / (trace->from_height - trace->to_height);
          trace->max_allocation_progress_delta =
            MAX (trace->max_allocation_progress_delta,
                 ABS (width_progress - height_progress));
          trace->previous_allocated_width = allocated_width;
          trace->previous_allocated_height = allocated_height;
          trace->allocation_samples++;
        }

      gtk_window_get_default_size (window, &default_width, &default_height);
      if (default_width == trace->to_width
          && default_height == trace->to_height
          && ABS (allocated_width - trace->to_width) <= 1
          && ABS (allocated_height - trace->to_height) <= 1)
        return TRUE;

      g_usleep (1000);
    }
  while (g_get_monotonic_time () < deadline);

  return FALSE;
}

static void
test_animated_shrink_uses_one_timeline (void)
{
  g_autofree gchar *application_id = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  KasasaWindow *window;
  ResizeTrace trace = {
    .from_width = 600,
    .from_height = 450,
    .to_width = 75,
    .to_height = 75,
    .previous_width = 600,
    .previous_height = 450,
    .previous_allocated_width = 600,
    .previous_allocated_height = 450,
  };

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_boolean (settings,
                                         "miniaturize-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "auto-discard-window",
                                         FALSE));

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Shrink%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", application,
                         NULL);
  gtk_window_present (GTK_WINDOW (window));
  dispatch_pending_sources ();

  kasasa_window_resize_window (window,
                               trace.from_height,
                               trace.from_width);
  g_assert_true (wait_for_window_allocation (GTK_WINDOW (window),
                                             trace.from_width,
                                             trace.from_height));

  g_object_set (gtk_settings_get_default (),
                "gtk-enable-animations", TRUE,
                NULL);
  g_signal_connect (window,
                    "notify::default-width",
                    G_CALLBACK (trace_resize_progress),
                    &trace);
  g_signal_connect (window,
                    "notify::default-height",
                    G_CALLBACK (trace_resize_progress),
                    &trace);

  kasasa_window_resize_window (window, trace.to_height, trace.to_width);
  g_assert_true (wait_for_animated_shrink (GTK_WINDOW (window), &trace));

  g_signal_handlers_disconnect_by_data (window, &trace);
  g_object_set (gtk_settings_get_default (),
                "gtk-enable-animations", FALSE,
                NULL);
  g_assert_cmpuint (trace.samples, >, 2);
  g_assert_cmpuint (trace.allocation_samples, >, 2);
  g_assert_false (trace.reversed);
  g_assert_false (trace.allocation_reversed);
  g_assert_cmpfloat (trace.max_progress_delta, <=, 0.03);
  g_assert_cmpfloat (trace.max_allocation_progress_delta, <=, 0.05);

  gtk_window_destroy (GTK_WINDOW (window));
  dispatch_pending_sources ();
}

static void
test_switch_resize_modes (void)
{
  g_autofree gchar *application_id = NULL;
  g_autofree gchar *image_path = NULL;
  g_autofree gchar *image_uri = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  KasasaWindow *window;
  KasasaContentContainer *container;
  CloseData close_data = { 0 };
  gint initial_height;
  gint initial_width;
  gint resized_height;
  gint resized_width;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_boolean (settings, "auto-hide-menu", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "auto-discard-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings, "auto-trash-image", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "miniaturize-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings, "change-opacity", FALSE));
  g_assert_true (g_settings_set_int (settings, "occupy-screen", 15));

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Resize%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", application,
                         NULL);
  container = KASASA_CONTENT_CONTAINER (
    find_widget_by_id (GTK_WIDGET (window), "content_container"));
  close_data.carousel = ADW_CAROUSEL (
    find_widget_by_id (GTK_WIDGET (window), "carousel"));
  g_assert_nonnull (container);
  g_assert_nonnull (close_data.carousel);

  image_uri = create_temp_image_uri (&image_path, 1200, 800);
  g_assert_true (kasasa_content_container_append_screenshot (container,
                                                             image_uri,
                                                             &error));
  g_assert_no_error (error);
  gtk_window_present (GTK_WINDOW (window));
  dispatch_pending_sources ();
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &initial_width,
                               &initial_height);
  g_assert_true (wait_for_window_allocation (GTK_WINDOW (window),
                                             initial_width,
                                             initial_height));

  g_assert_true (kasasa_window_resize_for_content_switch (
    window, 700.0, 1000.0, KASASA_SWITCH_RESIZE_KEEP_WIDTH));
  dispatch_pending_sources ();
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &resized_width,
                               &resized_height);
  g_assert_cmpint (ABS (resized_width - initial_width), <=, 1);
  g_assert_cmpfloat_with_epsilon ((gdouble) resized_width / resized_height,
                                  1000.0 / 700.0,
                                  0.02);

  kasasa_window_reset_zoom (window);
  g_assert_true (kasasa_window_resize_window_scaling (window, 800.0, 1200.0));
  dispatch_pending_sources ();
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &initial_width,
                               &initial_height);
  g_assert_true (wait_for_window_allocation (GTK_WINDOW (window),
                                             initial_width,
                                             initial_height));

  g_assert_true (kasasa_window_resize_for_content_switch (
    window, 1000.0, 1400.0, KASASA_SWITCH_RESIZE_KEEP_HEIGHT));
  dispatch_pending_sources ();
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &resized_width,
                               &resized_height);
  g_assert_cmpint (ABS (resized_height - initial_height), <=, 1);
  g_assert_cmpfloat_with_epsilon ((gdouble) resized_width / resized_height,
                                  1400.0 / 1000.0,
                                  0.02);

  g_signal_connect_after (window,
                          "close-request",
                          G_CALLBACK (after_close_request),
                          &close_data);
  g_action_group_activate_action (G_ACTION_GROUP (application), "quit", NULL);
  dispatch_pending_sources ();

  g_assert_true (close_data.close_observed);
  g_assert_true (close_data.content_was_empty);
  g_assert_cmpint (g_remove (image_path), ==, 0);
}

static void
test_continuous_zoom_shrink (void)
{
  g_autofree gchar *application_id = NULL;
  g_autofree gchar *image_path = NULL;
  g_autofree gchar *image_uri = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaApplication) application = NULL;
  KasasaWindow *window;
  KasasaContentContainer *container;
  CloseData close_data = { 0 };
  gdouble previous_zoom;
  gint initial_width;
  gint initial_height;
  gint immediate_height;
  gint immediate_width;
  gint final_width;
  gint final_height;
  gint settled_height;
  gint settled_width;
  guint changes = 0;
  guint wheel_height_updates = 0;
  guint wheel_width_updates = 0;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_boolean (settings, "auto-hide-menu", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "auto-discard-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings, "auto-trash-image", FALSE));
  g_assert_true (g_settings_set_boolean (settings,
                                         "miniaturize-window",
                                         FALSE));
  g_assert_true (g_settings_set_boolean (settings, "change-opacity", FALSE));
  g_assert_true (g_settings_set_int (settings, "occupy-screen", 50));

  application_id = g_strdup_printf ("io.github.kelvinnovais.Kasasa.Zoom%u",
                                    (guint) getpid ());
  application = kasasa_application_new (application_id);
  g_assert_true (g_application_register (G_APPLICATION (application),
                                         NULL,
                                         &error));
  g_assert_no_error (error);

  window = g_object_new (KASASA_TYPE_WINDOW,
                         "application", application,
                         NULL);
  container = KASASA_CONTENT_CONTAINER (
    find_widget_by_id (GTK_WIDGET (window), "content_container"));
  close_data.carousel = ADW_CAROUSEL (
    find_widget_by_id (GTK_WIDGET (window), "carousel"));
  g_assert_nonnull (container);
  g_assert_nonnull (close_data.carousel);

  image_uri = create_temp_image_uri (&image_path, 1200, 800);
  g_assert_true (kasasa_content_container_append_screenshot (container,
                                                             image_uri,
                                                             &error));
  g_assert_no_error (error);
  gtk_window_present (GTK_WINDOW (window));
  dispatch_pending_sources ();
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &initial_width,
                               &initial_height);
  g_assert_cmpint (initial_width, >, WINDOW_MIN_WIDTH);
  g_assert_cmpint (initial_height, >, WINDOW_MIN_HEIGHT);

  g_signal_connect (window,
                    "notify::default-width",
                    G_CALLBACK (count_property_notification),
                    &wheel_width_updates);
  g_signal_connect (window,
                    "notify::default-height",
                    G_CALLBACK (count_property_notification),
                    &wheel_height_updates);

  g_assert_true (kasasa_window_apply_zoom_delta (
    window, 1.0, KASASA_ZOOM_INPUT_WHEEL));
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &immediate_width,
                               &immediate_height);
  dispatch_sources_for (500);
  gtk_window_get_default_size (GTK_WINDOW (window),
                               &settled_width,
                               &settled_height);
  g_signal_handlers_disconnect_by_data (window, &wheel_width_updates);
  g_signal_handlers_disconnect_by_data (window, &wheel_height_updates);

  g_assert_cmpint (immediate_width, <, initial_width);
  g_assert_cmpint (immediate_height, <, initial_height);
  g_assert_cmpint (settled_width, ==, immediate_width);
  g_assert_cmpint (settled_height, ==, immediate_height);
  g_assert_cmpuint (wheel_width_updates, <=, 1);
  g_assert_cmpuint (wheel_height_updates, <=, 1);

  previous_zoom = kasasa_window_get_zoom_factor (window);
  for (guint i = 0; i < 256; i++)
    {
      gdouble zoom;
      gboolean changed;

      changed = kasasa_window_apply_zoom_delta (window,
                                                KASASA_ZOOM_SURFACE_PIXELS_PER_STEP
                                                / 4.0,
                                                KASASA_ZOOM_INPUT_SURFACE);
      zoom = kasasa_window_get_zoom_factor (window);
      g_assert_true (isfinite (zoom));
      g_assert_cmpfloat (zoom, <=, previous_zoom);
      g_assert_cmpfloat (zoom, >=, WINDOW_ZOOM_MIN);
      if (changed)
        changes++;
      previous_zoom = zoom;
    }

  g_assert_cmpuint (changes, >, 0);
  g_assert_cmpfloat (previous_zoom, <, 1.0);
  g_assert_cmpfloat (previous_zoom, >=, WINDOW_ZOOM_MIN);
  for (guint i = 0; i < 32; i++)
    g_assert_false (kasasa_window_apply_zoom_delta (
      window,
      KASASA_ZOOM_SURFACE_PIXELS_PER_STEP / 4.0,
      KASASA_ZOOM_INPUT_SURFACE));

  g_assert_true (wait_for_window_shrink (GTK_WINDOW (window),
                                         initial_width,
                                         &final_width,
                                         &final_height));
  g_assert_cmpint (final_width, >=, WINDOW_MIN_WIDTH);
  g_assert_cmpint (final_height, >=, WINDOW_MIN_HEIGHT);
  g_assert_cmpfloat_with_epsilon ((gdouble) final_width / final_height,
                                  1.5,
                                  0.02);

  g_signal_connect_after (window,
                          "close-request",
                          G_CALLBACK (after_close_request),
                          &close_data);
  g_action_group_activate_action (G_ACTION_GROUP (application), "quit", NULL);
  dispatch_pending_sources ();

  g_assert_true (close_data.close_observed);
  g_assert_true (close_data.content_was_empty);
  g_assert_cmpint (g_remove (image_path), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ())
    {
      if (g_getenv ("KASASA_REQUIRE_DISPLAY") != NULL)
        g_error ("A display is required for GTK component tests");

      g_test_message ("No display available; skipping GTK component tests");
      return 77;
    }

  adw_init ();
  g_object_set (gtk_settings_get_default (),
                "gtk-enable-animations", FALSE,
                NULL);
  g_test_add_func ("/gtk/window/quit-wipes-content",
                   test_quit_wipes_real_window_content);
  g_test_add_func ("/gtk/window/internal-motion-keeps-controls-visible",
                   test_internal_motion_keeps_controls_visible);
  g_test_add_func ("/gtk/window/continuous-zoom-shrink",
                   test_continuous_zoom_shrink);
  g_test_add_func ("/gtk/window/switch-resize-modes",
                   test_switch_resize_modes);
  g_test_add_func ("/gtk/window/animated-shrink-one-timeline",
                   test_animated_shrink_uses_one_timeline);

  return g_test_run ();
}
