/* test-gtk-content-container.c
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "kasasa-content-container.h"
#include "kasasa-screenshot.h"
#include "kasasa-window.h"

static const guint8 png_1x1[] = {
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
  0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c,
  0x02, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41,
  0x54, 0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00,
  0x01, 0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
  0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
  0xae, 0x42, 0x60, 0x82
};

typedef struct
{
  GtkWindow *window;
  KasasaContentContainer *container;
  AdwCarousel *carousel;
  GtkWidget *add_button;
  GtkWidget *more_actions_button;
  GtkWidget *remove_button;
  GtkWidget *retake_button;
  GtkWidget *delayed_button;
  GtkWidget *toolbar_overlay;
  gchar *image_path;
  gchar *image_uri;
} Fixture;

typedef enum
{
  FAKE_PORTAL_SUCCESS,
  FAKE_PORTAL_CANCEL,
  FAKE_PORTAL_ERROR,
  FAKE_PORTAL_PENDING,
} FakePortalResult;

static FakePortalResult fake_portal_result;
static const gchar *fake_portal_uri;
static GTask *fake_pending_task;
static guint fake_take_calls;
static guint fake_finish_calls;
static guint switch_resize_calls;
static KasasaSwitchResizeMode last_switch_resize_mode;

static void
fake_take_screenshot (XdpPortal           *portal,
                      XdpParent           *parent,
                      XdpScreenshotFlags   flags,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             data)
{
  GTask *task;

  fake_take_calls++;
  task = g_task_new (portal, cancellable, callback, data);

  switch (fake_portal_result)
    {
    case FAKE_PORTAL_SUCCESS:
      g_task_return_pointer (task, g_strdup (fake_portal_uri), g_free);
      g_object_unref (task);
      break;
    case FAKE_PORTAL_CANCEL:
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_CANCELLED,
                               "portal test cancelled");
      g_object_unref (task);
      break;
    case FAKE_PORTAL_ERROR:
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "portal test error");
      g_object_unref (task);
      break;
    case FAKE_PORTAL_PENDING:
      g_assert_null (fake_pending_task);
      fake_pending_task = task;
      break;
    default:
      g_assert_not_reached ();
    }
}

static gchar *
fake_take_screenshot_finish (XdpPortal    *portal,
                             GAsyncResult *result,
                             GError      **error)
{
  fake_finish_calls++;
  return g_task_propagate_pointer (G_TASK (result), error);
}

static const KasasaScreenshotPortalOps fake_portal_ops = {
  .take_screenshot = fake_take_screenshot,
  .take_screenshot_finish = fake_take_screenshot_finish,
};

/* The container state machine does not need real window animations. */
GType
kasasa_window_get_type (void)
{
  return GTK_TYPE_WINDOW;
}

KasasaWindow *
kasasa_window_get_window_reference (GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root (widget);

  return GTK_IS_WINDOW (root) ? KASASA_WINDOW (root) : NULL;
}

gboolean
kasasa_window_get_trash_button_active (KasasaWindow *window)
{
  return FALSE;
}

gboolean
kasasa_window_is_miniaturized (KasasaWindow *window)
{
  return FALSE;
}

void
kasasa_window_hide_window (KasasaWindow       *window,
                           gboolean            hide,
                           HideWindowCallback  callback,
                           GObject             *callback_data)
{
  if (callback != NULL)
    callback (callback_data);
}

void
kasasa_window_change_opacity (KasasaWindow *window,
                              Opacity       opacity_direction)
{
}

gboolean
kasasa_window_resize_window_scaling (KasasaWindow *window,
                                     gdouble       new_height,
                                     gdouble       new_width)
{
  return TRUE;
}

gboolean
kasasa_window_resize_window_scaling_for_zoom (KasasaWindow *window,
                                              gdouble       new_height,
                                              gdouble       new_width)
{
  return TRUE;
}

gboolean
kasasa_window_resize_for_content_switch (KasasaWindow          *window,
                                         gdouble                new_height,
                                         gdouble                new_width,
                                         KasasaSwitchResizeMode mode)
{
  switch_resize_calls++;
  last_switch_resize_mode = mode;
  return TRUE;
}

void
kasasa_window_reset_zoom (KasasaWindow *window)
{
}

void
kasasa_window_auto_discard_window (KasasaWindow *window)
{
}

void
kasasa_window_miniaturize_window (KasasaWindow *window,
                                  gboolean      miniaturize)
{
}

void
kasasa_window_block_miniaturization (KasasaWindow *window,
                                     gboolean      block)
{
}

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

static void
fixture_setup (Fixture *fixture,
               gconstpointer user_data)
{
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  gint fd;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_assert_true (g_settings_set_uint (settings,
                                      "image-switch-resize-mode",
                                      KASASA_SWITCH_RESIZE_FIT));
  switch_resize_calls = 0;
  last_switch_resize_mode = KASASA_SWITCH_RESIZE_FIT;

  fd = g_file_open_tmp ("kasasa-container-test-XXXXXX.png",
                        &fixture->image_path,
                        &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);
  g_assert_true (g_file_set_contents (fixture->image_path,
                                      (const gchar *) png_1x1,
                                      sizeof png_1x1,
                                      &error));
  g_assert_no_error (error);
  fixture->image_uri = g_filename_to_uri (fixture->image_path, NULL, &error);
  g_assert_no_error (error);

  fixture->window = GTK_WINDOW (gtk_window_new ());
  fixture->container = kasasa_content_container_new ();
  gtk_window_set_child (fixture->window, GTK_WIDGET (fixture->container));

  fixture->carousel = ADW_CAROUSEL (
    find_widget_by_id (GTK_WIDGET (fixture->container), "carousel"));
  fixture->add_button = find_widget_by_id (GTK_WIDGET (fixture->container),
                                           "add_screenshot_button");
  fixture->more_actions_button = find_widget_by_id (
    GTK_WIDGET (fixture->container), "more_actions_button");
  fixture->remove_button = find_widget_by_id (GTK_WIDGET (fixture->container),
                                              "remove_content_button");
  fixture->retake_button = find_widget_by_id (GTK_WIDGET (fixture->container),
                                              "retake_screenshot_button");
  fixture->delayed_button = find_widget_by_id (GTK_WIDGET (fixture->container),
                                               "add_delayed_screenshot_button");
  fixture->toolbar_overlay = find_widget_by_id (GTK_WIDGET (fixture->container),
                                                "toolbar_overlay");
  g_assert_nonnull (fixture->carousel);
  g_assert_nonnull (fixture->add_button);
  g_assert_nonnull (fixture->more_actions_button);
  g_assert_nonnull (fixture->remove_button);
  g_assert_nonnull (fixture->retake_button);
  g_assert_nonnull (fixture->delayed_button);
  g_assert_nonnull (fixture->toolbar_overlay);

  fake_portal_result = FAKE_PORTAL_SUCCESS;
  fake_portal_uri = fixture->image_uri;
  fake_take_calls = 0;
  fake_finish_calls = 0;
  g_assert_null (fake_pending_task);
  kasasa_content_container_set_screenshot_portal_ops (fixture->container,
                                                      &fake_portal_ops);

  gtk_window_present (fixture->window);
  dispatch_pending_sources ();
}

static void
fixture_teardown (Fixture *fixture,
                  gconstpointer user_data)
{
  if (fixture->window != NULL)
    {
      gtk_window_destroy (fixture->window);
      dispatch_pending_sources ();
    }

  g_assert_cmpint (g_remove (fixture->image_path), ==, 0);
  g_clear_pointer (&fixture->image_path, g_free);
  g_clear_pointer (&fixture->image_uri, g_free);
}

static GtkWidget *
append_screenshot (Fixture *fixture)
{
  g_autoptr (GError) error = NULL;
  guint index = adw_carousel_get_n_pages (fixture->carousel);

  g_assert_true (kasasa_content_container_append_screenshot (
    fixture->container, fixture->image_uri, &error));
  g_assert_no_error (error);
  dispatch_pending_sources ();

  return adw_carousel_get_nth_page (fixture->carousel, index);
}

static void
select_page (Fixture   *fixture,
             GtkWidget *page)
{
  guint n_pages = adw_carousel_get_n_pages (fixture->carousel);

  adw_carousel_scroll_to (fixture->carousel, page, FALSE);
  for (guint i = 0; i < n_pages; i++)
    {
      if (adw_carousel_get_nth_page (fixture->carousel, i) == page)
        {
          g_signal_emit_by_name (fixture->carousel, "page-changed", i);
          break;
        }
    }
}

static void
test_content_limit_and_toolbar (Fixture *fixture,
                                gconstpointer user_data)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (gtk_widget_get_sensitive (fixture->remove_button));

  for (guint i = 0; i < MAX_N_CONTENTS; i++)
    append_screenshot (fixture);

  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel),
                    ==,
                    MAX_N_CONTENTS);
  g_assert_false (gtk_widget_get_sensitive (fixture->add_button));
  g_assert_false (gtk_widget_get_sensitive (fixture->more_actions_button));
  g_assert_true (gtk_widget_get_sensitive (fixture->remove_button));

  g_test_expect_message (NULL,
                         G_LOG_LEVEL_WARNING,
                         "*Max number of contents reached*");
  g_assert_false (kasasa_content_container_append_screenshot (
    fixture->container, fixture->image_uri, &error));
  g_test_assert_expected_messages ();
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel),
                    ==,
                    MAX_N_CONTENTS);
}

static void
test_failed_append_preserves_carousel (Fixture *fixture,
                                       gconstpointer user_data)
{
  static const guint8 invalid_image[] = "not an image";
  g_autofree gchar *invalid_path = NULL;
  g_autofree gchar *invalid_uri = NULL;
  g_autoptr (GError) error = NULL;
  gint fd;

  append_screenshot (fixture);
  fd = g_file_open_tmp ("kasasa-container-invalid-XXXXXX.png",
                        &invalid_path,
                        &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);
  g_assert_true (g_file_set_contents (invalid_path,
                                      (const gchar *) invalid_image,
                                      sizeof invalid_image - 1,
                                      &error));
  g_assert_no_error (error);
  invalid_uri = g_filename_to_uri (invalid_path, NULL, &error);
  g_assert_no_error (error);

  g_assert_false (kasasa_content_container_append_screenshot (
    fixture->container, invalid_uri, &error));
  g_assert_nonnull (error);
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 1);
  g_assert_cmpint (g_remove (invalid_path), ==, 0);
}

static void
test_removes_first_middle_and_last (Fixture *fixture,
                                    gconstpointer user_data)
{
  GtkWidget *first = append_screenshot (fixture);
  GtkWidget *middle = append_screenshot (fixture);
  GtkWidget *third = append_screenshot (fixture);
  GtkWidget *last = append_screenshot (fixture);

  select_page (fixture, middle);
  g_signal_emit_by_name (fixture->remove_button, "clicked");
  dispatch_pending_sources ();
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 3);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 0) == first);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 1) == third);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 2) == last);

  select_page (fixture, last);
  g_signal_emit_by_name (fixture->remove_button, "clicked");
  dispatch_pending_sources ();
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 2);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 0) == first);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 1) == third);

  select_page (fixture, first);
  g_signal_emit_by_name (fixture->remove_button, "clicked");
  dispatch_pending_sources ();
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 1);
  g_assert_true (adw_carousel_get_nth_page (fixture->carousel, 0) == third);
  g_assert_false (gtk_widget_get_sensitive (fixture->remove_button));
}

static void
test_page_switching_and_nested_locks (Fixture *fixture,
                                      gconstpointer user_data)
{
  GtkWidget *first = append_screenshot (fixture);
  GtkWidget *second = append_screenshot (fixture);

  select_page (fixture, first);
  g_assert_false (kasasa_content_container_switch_page (fixture->container,
                                                        0));
  g_assert_false (kasasa_content_container_switch_page (fixture->container,
                                                        -1));
  g_assert_true (kasasa_content_container_switch_page (fixture->container,
                                                       1));

  select_page (fixture, second);
  g_assert_false (kasasa_content_container_switch_page (fixture->container,
                                                        1));

  kasasa_content_container_carousel_set_interactive (fixture->container,
                                                      FALSE);
  kasasa_content_container_carousel_set_interactive (fixture->container,
                                                      FALSE);
  g_assert_false (adw_carousel_get_interactive (fixture->carousel));
  g_assert_false (kasasa_content_container_switch_page (fixture->container,
                                                        -1));

  kasasa_content_container_carousel_set_interactive (fixture->container, TRUE);
  g_assert_false (adw_carousel_get_interactive (fixture->carousel));
  kasasa_content_container_carousel_set_interactive (fixture->container, TRUE);
  g_assert_true (adw_carousel_get_interactive (fixture->carousel));
}

static void
test_page_switch_resize_mode (Fixture *fixture,
                              gconstpointer user_data)
{
  g_autoptr (GSettings) settings =
    g_settings_new ("io.github.kelvinnovais.Kasasa");
  GtkWidget *first = append_screenshot (fixture);
  GtkWidget *second = append_screenshot (fixture);

  g_assert_true (g_settings_set_uint (settings,
                                      "image-switch-resize-mode",
                                      KASASA_SWITCH_RESIZE_KEEP_WIDTH));
  switch_resize_calls = 0;
  select_page (fixture, first);
  g_assert_cmpuint (switch_resize_calls, >, 0);
  g_assert_cmpint (last_switch_resize_mode,
                   ==,
                   KASASA_SWITCH_RESIZE_KEEP_WIDTH);

  g_assert_true (g_settings_set_uint (settings,
                                      "image-switch-resize-mode",
                                      KASASA_SWITCH_RESIZE_KEEP_HEIGHT));
  switch_resize_calls = 0;
  select_page (fixture, second);
  g_assert_cmpuint (switch_resize_calls, >, 0);
  g_assert_cmpint (last_switch_resize_mode,
                   ==,
                   KASASA_SWITCH_RESIZE_KEEP_HEIGHT);
}

static void
test_wipe_removes_every_page (Fixture *fixture,
                              gconstpointer user_data)
{
  append_screenshot (fixture);
  append_screenshot (fixture);
  append_screenshot (fixture);

  kasasa_content_container_wipe_content (fixture->container);
  dispatch_pending_sources ();

  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 0);
  g_assert_false (adw_carousel_get_interactive (fixture->carousel));
}

static gboolean
prevent_window_close (GtkWindow *window,
                      gpointer   user_data)
{
  guint *close_requests = user_data;

  (*close_requests)++;
  return TRUE;
}

static void
mark_finalized (gpointer data,
                GObject *where_the_object_was)
{
  gboolean *finalized = data;

  *finalized = TRUE;
}

static void
test_portal_first_screenshot_success (Fixture *fixture,
                                      gconstpointer user_data)
{
  gtk_widget_set_visible (GTK_WIDGET (fixture->window), FALSE);
  kasasa_content_container_request_first_screenshot (fixture->container);
  dispatch_pending_sources ();

  g_assert_cmpuint (fake_take_calls, ==, 1);
  g_assert_cmpuint (fake_finish_calls, ==, 1);
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 1);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (fixture->window)));
}

static void
test_portal_first_screenshot_cancel (Fixture *fixture,
                                     gconstpointer user_data)
{
  guint close_requests = 0;

  fake_portal_result = FAKE_PORTAL_CANCEL;
  g_signal_connect (fixture->window,
                    "close-request",
                    G_CALLBACK (prevent_window_close),
                    &close_requests);

  kasasa_content_container_request_first_screenshot (fixture->container);
  dispatch_pending_sources ();

  g_assert_cmpuint (fake_take_calls, ==, 1);
  g_assert_cmpuint (fake_finish_calls, ==, 1);
  g_assert_cmpuint (close_requests, ==, 1);
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 0);
}

static void
test_portal_add_screenshot_error_restores_toolbar (Fixture *fixture,
                                                   gconstpointer user_data)
{
  fake_portal_result = FAKE_PORTAL_ERROR;
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*portal test error*");

  g_signal_emit_by_name (fixture->add_button, "clicked");
  dispatch_pending_sources ();

  g_test_assert_expected_messages ();
  g_assert_cmpuint (fake_take_calls, ==, 1);
  g_assert_cmpuint (fake_finish_calls, ==, 1);
  g_assert_cmpuint (adw_carousel_get_n_pages (fixture->carousel), ==, 0);
  g_assert_true (gtk_widget_get_sensitive (fixture->toolbar_overlay));
}

static void
test_portal_retake_success_then_cancel (Fixture *fixture,
                                        gconstpointer user_data)
{
  g_autofree gchar *replacement_path = NULL;
  g_autofree gchar *replacement_uri = NULL;
  g_autoptr (GFile) replacement_file = NULL;
  g_autoptr (GError) error = NULL;
  KasasaScreenshot *screenshot;
  gint fd;

  screenshot = KASASA_SCREENSHOT (append_screenshot (fixture));
  fd = g_file_open_tmp ("kasasa-container-retake-XXXXXX.png",
                        &replacement_path,
                        &error);
  g_assert_no_error (error);
  g_assert_cmpint (fd, >=, 0);
  g_assert_cmpint (close (fd), ==, 0);
  g_assert_true (g_file_set_contents (replacement_path,
                                      (const gchar *) png_1x1,
                                      sizeof png_1x1,
                                      &error));
  g_assert_no_error (error);
  replacement_uri = g_filename_to_uri (replacement_path, NULL, &error);
  g_assert_no_error (error);
  replacement_file = g_file_new_for_uri (replacement_uri);

  fake_portal_uri = replacement_uri;
  g_signal_emit_by_name (fixture->retake_button, "clicked");
  dispatch_pending_sources ();

  g_assert_cmpuint (fake_take_calls, ==, 1);
  g_assert_true (g_file_equal (replacement_file,
                               kasasa_screenshot_get_file (screenshot)));
  g_assert_true (adw_carousel_get_interactive (fixture->carousel));

  fake_portal_result = FAKE_PORTAL_CANCEL;
  g_signal_emit_by_name (fixture->retake_button, "clicked");
  dispatch_pending_sources ();

  g_assert_cmpuint (fake_take_calls, ==, 2);
  g_assert_true (g_file_equal (replacement_file,
                               kasasa_screenshot_get_file (screenshot)));
  g_assert_true (adw_carousel_get_interactive (fixture->carousel));
  g_assert_cmpint (g_remove (replacement_path), ==, 0);
}

static void
test_portal_callback_after_dispose (Fixture *fixture,
                                    gconstpointer user_data)
{
  gboolean container_finalized = FALSE;
  GTask *task;

  fake_portal_result = FAKE_PORTAL_PENDING;
  g_object_weak_ref (G_OBJECT (fixture->container),
                     mark_finalized,
                     &container_finalized);
  kasasa_content_container_request_first_screenshot (fixture->container);
  g_assert_nonnull (fake_pending_task);

  gtk_window_destroy (fixture->window);
  fixture->window = NULL;
  dispatch_pending_sources ();
  g_assert_true (container_finalized);

  task = g_steal_pointer (&fake_pending_task);
  g_task_return_pointer (task, g_strdup (fixture->image_uri), g_free);
  g_object_unref (task);
  dispatch_pending_sources ();

  g_assert_cmpuint (fake_take_calls, ==, 1);
  g_assert_cmpuint (fake_finish_calls, ==, 1);
}

static void
test_delayed_screenshot_cancel_restores_state (Fixture *fixture,
                                               gconstpointer user_data)
{
  g_autoptr (GSettings) settings =
    g_settings_new ("io.github.kelvinnovais.Kasasa");

  g_assert_true (g_settings_set_uint (settings, "screenshot-delay", 1));
  g_signal_emit_by_name (fixture->delayed_button, "clicked");

  g_assert_false (gtk_widget_get_sensitive (fixture->toolbar_overlay));
  g_assert_true (kasasa_content_container_cancel_delayed_screenshot (
    fixture->container));
  g_assert_false (kasasa_content_container_cancel_delayed_screenshot (
    fixture->container));
  g_assert_true (gtk_widget_get_sensitive (fixture->toolbar_overlay));
  g_assert_cmpuint (fake_take_calls, ==, 0);
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
  g_test_add ("/gtk/content-container/content-limit-and-toolbar",
              Fixture, NULL, fixture_setup,
              test_content_limit_and_toolbar, fixture_teardown);
  g_test_add ("/gtk/content-container/failed-append-preserves-carousel",
              Fixture, NULL, fixture_setup,
              test_failed_append_preserves_carousel, fixture_teardown);
  g_test_add ("/gtk/content-container/remove-first-middle-last",
              Fixture, NULL, fixture_setup,
              test_removes_first_middle_and_last, fixture_teardown);
  g_test_add ("/gtk/content-container/page-switching-and-locks",
              Fixture, NULL, fixture_setup,
              test_page_switching_and_nested_locks, fixture_teardown);
  g_test_add ("/gtk/content-container/page-switch-resize-mode",
              Fixture, NULL, fixture_setup,
              test_page_switch_resize_mode, fixture_teardown);
  g_test_add ("/gtk/content-container/wipe",
              Fixture, NULL, fixture_setup,
              test_wipe_removes_every_page, fixture_teardown);
  g_test_add ("/gtk/content-container/portal-first-success",
              Fixture, NULL, fixture_setup,
              test_portal_first_screenshot_success, fixture_teardown);
  g_test_add ("/gtk/content-container/portal-first-cancel",
              Fixture, NULL, fixture_setup,
              test_portal_first_screenshot_cancel, fixture_teardown);
  g_test_add ("/gtk/content-container/portal-add-error-restores-toolbar",
              Fixture, NULL, fixture_setup,
              test_portal_add_screenshot_error_restores_toolbar,
              fixture_teardown);
  g_test_add ("/gtk/content-container/portal-retake-success-cancel",
              Fixture, NULL, fixture_setup,
              test_portal_retake_success_then_cancel, fixture_teardown);
  g_test_add ("/gtk/content-container/portal-callback-after-dispose",
              Fixture, NULL, fixture_setup,
              test_portal_callback_after_dispose, fixture_teardown);
  g_test_add ("/gtk/content-container/delayed-cancel-restores-state",
              Fixture, NULL, fixture_setup,
              test_delayed_screenshot_cancel_restores_state,
              fixture_teardown);

  return g_test_run ();
}
