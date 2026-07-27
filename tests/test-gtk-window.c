/* test-gtk-window.c
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

static void
dispatch_pending_sources (void)
{
  while (g_main_context_iteration (NULL, FALSE))
    ;
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
  gint final_width;
  gint final_height;
  guint changes = 0;

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

  previous_zoom = kasasa_window_get_zoom_factor (window);
  for (guint i = 0; i < 256; i++)
    {
      gdouble zoom;
      gboolean changed;

      changed = kasasa_window_apply_zoom_delta (window,
                                                1.0,
                                                KASASA_ZOOM_INPUT_WHEEL);
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
      window, 1.0, KASASA_ZOOM_INPUT_WHEEL));

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
  g_test_add_func ("/gtk/window/continuous-zoom-shrink",
                   test_continuous_zoom_shrink);
  g_test_add_func ("/gtk/window/switch-resize-modes",
                   test_switch_resize_modes);

  return g_test_run ();
}
