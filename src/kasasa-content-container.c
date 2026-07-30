/* kasasa-content-container.c
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

#include <libportal-gtk4/portal-gtk4.h>
#include <glib/gi18n.h>
#include <math.h>

#include "kasasa-content-container.h"

#include "kasasa-window.h"
#include "kasasa-screenshot.h"
#include "kasasa-screencast.h"
#include "kasasa-source.h"
#include "kasasa-window-query.h"

#define DELAYED_SCREENSHOT_NOTIFICATION_ID "delayed-screenshot"
#define SCREENCAST_CREATE_TIMEOUT_MSEC 10000

struct _KasasaContentContainer
{
  AdwBreakpointBin         parent_instance;

  /* Template widgets */
  AdwToastOverlay         *toast_overlay;
  AdwCarousel             *carousel;
  GtkButton               *retake_screenshot_button;
  GtkButton               *add_screenshot_button;
  GtkButton               *add_delayed_screenshot_button;
  GtkButton               *add_screencast_button;
  GtkButton               *add_hyprland_monitor_screencast_button;
  GtkButton               *remove_content_button;
  GtkButton               *stop_screencast_button;
  GtkButton               *copy_screenshot_button;
  GtkMenuButton           *more_actions_button;
  GtkRevealer             *revealer_end_buttons;
  GtkRevealer             *revealer_start_buttons;
  GtkOverlay              *toolbar_overlay;
  GtkPopover              *more_actions_popover;

  /* Instance variables */
  XdpPortal               *portal;
  XdpParent               *parent;
  GSettings               *settings;
  GCancellable            *portal_cancellable;
  GCancellable            *screencast_cancellable;
  AdwToast                *screencast_request_toast;
  KasasaSource             delayed_screenshot_source;
  KasasaSource             screencast_create_timeout_source;
  guint                    carousel_interaction_locks;
  guint                    current_page_index;
  guint                    screencast_request_id;
  guint                    screencast_create_timeout_ms;
  gboolean                 screencast_request_pending;
  gboolean                 screencast_first_capture;
  KasasaScreenshotPortalOps  screenshot_portal_ops;
  KasasaScreencastPortalOps  screencast_portal_ops;
};

typedef struct
{
  GWeakRef container;
  KasasaPortalTakeScreenshotFinishFunc take_screenshot_finish;
  gboolean first_capture;
} PortalRequestData;

typedef struct
{
  GWeakRef container;
  KasasaPortalCreateScreencastSessionFinishFunc create_session_finish;
  KasasaPortalStartScreencastSessionFinishFunc start_session_finish;
  guint request_id;
  gboolean first_capture;
} ScreencastRequestData;

static const KasasaScreenshotPortalOps default_screenshot_portal_ops = {
  .take_screenshot = xdp_portal_take_screenshot,
  .take_screenshot_finish = xdp_portal_take_screenshot_finish,
};

static const KasasaScreencastPortalOps default_screencast_portal_ops = {
  .create_session = xdp_portal_create_screencast_session,
  .create_session_finish = xdp_portal_create_screencast_session_finish,
  .start_session = xdp_session_start,
  .start_session_finish = xdp_session_start_finish,
};

G_DEFINE_FINAL_TYPE (KasasaContentContainer, kasasa_content_container, ADW_TYPE_BREAKPOINT_BIN)

static GtkWidget * get_current_content (KasasaContentContainer *self);
static void start_screencast_session (KasasaContentContainer *self,
                                      gboolean                first_capture);
static gboolean append_hyprland_monitor_screencast (
  KasasaContentContainer *self,
  const KasasaMonitor    *monitor,
  GError                **error);
static void show_operation_error (KasasaContentContainer *self,
                                  const gchar            *fallback_message,
                                  const GError           *error);

static gboolean
has_active_screencast (KasasaContentContainer *self)
{
  guint n_pages = adw_carousel_get_n_pages (self->carousel);

  for (guint i = 0; i < n_pages; i++)
    {
      GtkWidget *content = adw_carousel_get_nth_page (self->carousel, i);

      if (KASASA_IS_SCREENCAST (content)
          && kasasa_screencast_is_active (KASASA_SCREENCAST (content)))
        return TRUE;
    }

  return FALSE;
}

static gboolean
request_was_cancelled (const GError *error)
{
  return error != NULL
         && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
create_hyprland_monitor_screencast (GtkButton *button,
                                    gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  g_autoptr (KasasaMonitor) monitor = NULL;
  g_autoptr (GError) error = NULL;

  gtk_popover_popdown (self->more_actions_popover);

  if (has_active_screencast (self))
    {
      AdwToast *toast = adw_toast_new (_("A screencast is already active"));

      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      return;
    }

  monitor = kasasa_monitor_query_resolve_live ("active", &error);
  if (monitor == NULL)
    {
      show_operation_error (self, _("Couldn't find the active monitor"), error);
      return;
    }

  if (!append_hyprland_monitor_screencast (self, monitor, &error))
    show_operation_error (self, _("Couldn't display the monitor"), error);
}

static void
show_operation_error (KasasaContentContainer *self,
                      const gchar            *fallback_message,
                      const GError           *error)
{
  const gchar *message = error != NULL ? error->message : fallback_message;
  AdwToast *toast;

  toast = adw_toast_new_format (_("Error: %s"), message);
  adw_toast_set_action_target_value (toast, g_variant_new_string (message));
  adw_toast_set_button_label (toast, _("Copy"));
  adw_toast_set_action_name (toast, "toast.copy_error");
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
  g_warning ("%s", message);
}

static void
on_screencast_cpu_fallback (KasasaScreencast     *screencast,
                            KasasaContentContainer *self)
{
  AdwToast *toast;

  toast = adw_toast_new (_("GPU screencast unavailable. Switched to CPU."));
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
withdraw_delayed_screenshot_notification (void)
{
  GApplication *application = g_application_get_default ();

  if (application != NULL)
    g_application_withdraw_notification (application,
                                         DELAYED_SCREENSHOT_NOTIFICATION_ID);
}

static void
send_delayed_screenshot_notification (guint interval)
{
  GApplication *application = g_application_get_default ();
  g_autoptr (GNotification) notification = NULL;
  g_autofree gchar *body = NULL;

  if (application == NULL)
    return;

  notification = g_notification_new (_("Screenshot scheduled"));
  body = g_strdup_printf (ngettext ("Screenshot in %u second",
                                    "Screenshot in %u seconds",
                                    interval),
                          interval);
  g_notification_set_body (notification, body);
  g_notification_add_button (notification,
                             _("Cancel"),
                             "app.cancel-delayed-screenshot");
  g_application_send_notification (application,
                                   DELAYED_SCREENSHOT_NOTIFICATION_ID,
                                   notification);
}

static PortalRequestData *
portal_request_data_new_full (KasasaContentContainer *self,
                              gboolean                first_capture)
{
  PortalRequestData *data = g_new0 (PortalRequestData, 1);

  g_weak_ref_init (&data->container, self);
  data->take_screenshot_finish =
    self->screenshot_portal_ops.take_screenshot_finish;
  data->first_capture = first_capture;
  return data;
}

static PortalRequestData *
portal_request_data_new (KasasaContentContainer *self)
{
  return portal_request_data_new_full (self, FALSE);
}

static KasasaContentContainer *
portal_request_data_take_container (gpointer user_data)
{
  PortalRequestData *data = user_data;
  KasasaContentContainer *self = g_weak_ref_get (&data->container);

  g_weak_ref_clear (&data->container);
  g_free (data);
  return self;
}

static gchar *
portal_request_data_finish_screenshot (PortalRequestData *data,
                                       XdpPortal         *portal,
                                       GAsyncResult      *result,
                                       GError           **error)
{
  return data->take_screenshot_finish (portal, result, error);
}

static ScreencastRequestData *
screencast_request_data_new (KasasaContentContainer *self,
                             gboolean                first_capture,
                             guint                   request_id)
{
  ScreencastRequestData *data = g_new0 (ScreencastRequestData, 1);

  g_weak_ref_init (&data->container, self);
  data->create_session_finish =
    self->screencast_portal_ops.create_session_finish;
  data->start_session_finish =
    self->screencast_portal_ops.start_session_finish;
  data->request_id = request_id;
  data->first_capture = first_capture;
  return data;
}

static KasasaContentContainer *
screencast_request_data_take_container (ScreencastRequestData *data)
{
  KasasaContentContainer *self = g_weak_ref_get (&data->container);

  g_weak_ref_clear (&data->container);
  g_free (data);
  return self;
}

static gboolean
screencast_request_is_current (KasasaContentContainer *self,
                               guint                   request_id)
{
  return self->screencast_request_pending
         && self->screencast_request_id == request_id;
}

static KasasaWindow *
get_root_window (KasasaContentContainer *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  return KASASA_IS_WINDOW (root) ? KASASA_WINDOW (root) : NULL;
}

gboolean
kasasa_content_container_controls_active (KasasaContentContainer *self)
{
  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  return gtk_menu_button_get_active (self->more_actions_button);
}

void
kasasa_content_container_reveal_controls (KasasaContentContainer *self,
                                          gboolean                reveal_child)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  gtk_revealer_set_reveal_child (self->revealer_start_buttons, reveal_child);
  gtk_revealer_set_reveal_child (self->revealer_end_buttons, reveal_child);
}

static gboolean
request_window_resize (KasasaContentContainer *self,
                       gboolean                for_zoom,
                       gboolean                continuous)
{
  KasasaWindow *window = NULL;
  GtkWidget *content = NULL;
  gint new_height, new_width;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  content = get_current_content (self);
  if (!KASASA_IS_WINDOW (window) || !KASASA_IS_CONTENT (content))
    return FALSE;

  kasasa_content_get_dimensions (KASASA_CONTENT (content),
                                 &new_height,
                                 &new_width);

  if (for_zoom)
    return kasasa_window_resize_window_scaling_for_zoom (
      window,
      (gdouble) new_height,
      (gdouble) new_width,
      continuous);

  return kasasa_window_resize_window_scaling (
    window, (gdouble) new_height, (gdouble) new_width);
}

gboolean
kasasa_content_container_request_window_resize (KasasaContentContainer *self)
{
  return request_window_resize (self, FALSE, FALSE);
}

gboolean
kasasa_content_container_request_zoom_resize (KasasaContentContainer *self,
                                              gboolean                continuous)
{
  return request_window_resize (self, TRUE, continuous);
}

void
kasasa_content_container_wipe_content (KasasaContentContainer *self)
{
  guint n_pages = 0;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  n_pages = adw_carousel_get_n_pages (self->carousel);

  adw_carousel_set_interactive (self->carousel, FALSE);

  // Request finishing content from the last to the first page of the carousel.
  // Pictures are only deleted if the trash_button is toggled
  for (gint i = n_pages-1; i >= 0; i--)
    {
      GtkWidget *content = adw_carousel_get_nth_page (self->carousel, i);

      // Finish the content (KasasaSCreenshot needs a reference to the parent
      // window)...
      kasasa_content_finish (KASASA_CONTENT (content));
      // ...then remove it from the carousel
      adw_carousel_remove (self->carousel, content);
    }
}

void
kasasa_content_container_carousel_set_interactive (KasasaContentContainer *self,
                                                   gboolean interactive)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  if (!interactive)
    {
      self->carousel_interaction_locks++;
      adw_carousel_set_interactive (self->carousel, FALSE);
      return;
    }

  if (self->carousel_interaction_locks > 0)
    self->carousel_interaction_locks--;

  if (self->carousel_interaction_locks == 0)
    adw_carousel_set_interactive (self->carousel, TRUE);
}

static void
kasasa_content_container_update_toolbar_sensibility (KasasaContentContainer *self)
{
  GtkWidget *current_content = NULL;
  gboolean is_screenshot;
  gboolean is_active_screencast;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  // Restore the whole overlay sesibility...
  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), TRUE);

  // ...and treat the cases indiviually
  if (adw_carousel_get_n_pages (self->carousel) < MAX_N_CONTENTS)
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->add_screenshot_button),
                                TRUE);
      gtk_widget_set_sensitive (GTK_WIDGET (self->more_actions_button),
                                TRUE);
    }
  else
    {
      gtk_widget_set_sensitive (GTK_WIDGET (self->add_screenshot_button),
                                FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (self->more_actions_button),
                                FALSE);
    }

  /* Keep this action clickable while a screencast is active so the handler can
   * explain why a second concurrent Portal session is not started. */
  gtk_widget_set_sensitive (GTK_WIDGET (self->add_screencast_button),
                            adw_carousel_get_n_pages (self->carousel)
                              < MAX_N_CONTENTS);

  if (adw_carousel_get_n_pages (self->carousel) > 1)
    gtk_widget_set_sensitive (GTK_WIDGET (self->remove_content_button),
                              TRUE);
  else
    gtk_widget_set_sensitive (GTK_WIDGET (self->remove_content_button),
                              FALSE);

  current_content = get_current_content (self);
  is_screenshot = KASASA_IS_SCREENSHOT (current_content);
  is_active_screencast =
    KASASA_IS_SCREENCAST (current_content)
    && kasasa_screencast_is_active (KASASA_SCREENCAST (current_content));
  gtk_widget_set_sensitive (GTK_WIDGET (self->retake_screenshot_button),
                            is_screenshot);
  gtk_widget_set_sensitive (GTK_WIDGET (self->copy_screenshot_button),
                            is_screenshot);
  gtk_widget_set_visible (GTK_WIDGET (self->stop_screencast_button),
                          is_active_screencast);
  gtk_widget_set_sensitive (GTK_WIDGET (self->stop_screencast_button),
                            is_active_screencast);
}

static void
get_parent (KasasaContentContainer *self)
{
  GtkWindow *window = NULL;

  window = GTK_WINDOW (kasasa_window_get_window_reference (GTK_WIDGET (self)));

  self->parent = xdp_parent_new_gtk (window);
}

// Load the screenshot to the GtkPicture widget
gboolean
kasasa_content_container_append_screenshot (KasasaContentContainer *self,
                                            const gchar            *uri,
                                            GError                **error)
{
  KasasaScreenshot *new_screenshot = NULL;
  guint n_pages;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);
  g_return_val_if_fail (uri != NULL, FALSE);

  n_pages = adw_carousel_get_n_pages (self->carousel);
  g_debug ("Carousel number of pages: %u", n_pages);

  if (n_pages >= MAX_N_CONTENTS)
    {
      g_warning ("Max number of contents reached");
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_NO_SPACE,
                           "Max number of contents reached");
      return FALSE;
    }

  new_screenshot = kasasa_screenshot_new ();
  adw_carousel_append (self->carousel, GTK_WIDGET (new_screenshot));
  if (!kasasa_screenshot_load_screenshot (new_screenshot, uri, error))
    {
      adw_carousel_remove (self->carousel, GTK_WIDGET (new_screenshot));
      return FALSE;
    }

  adw_carousel_scroll_to (self->carousel, GTK_WIDGET (new_screenshot), TRUE);
  kasasa_content_container_update_toolbar_sensibility (self);
  return TRUE;
}

void
kasasa_content_container_set_screenshot_portal_ops (
  KasasaContentContainer          *self,
  const KasasaScreenshotPortalOps *ops)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (ops == NULL
                    || (ops->take_screenshot != NULL
                        && ops->take_screenshot_finish != NULL));

  self->screenshot_portal_ops = ops != NULL
                                ? *ops
                                : default_screenshot_portal_ops;
}

void
kasasa_content_container_set_screencast_portal_ops (
  KasasaContentContainer          *self,
  const KasasaScreencastPortalOps *ops,
  guint                            create_timeout_ms)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (!self->screencast_request_pending);
  g_return_if_fail (ops == NULL
                    || (ops->create_session != NULL
                        && ops->create_session_finish != NULL
                        && ops->start_session != NULL
                        && ops->start_session_finish != NULL));

  self->screencast_portal_ops = ops != NULL
                                ? *ops
                                : default_screencast_portal_ops;
  self->screencast_create_timeout_ms = create_timeout_ms != 0
                                       ? create_timeout_ms
                                       : SCREENCAST_CREATE_TIMEOUT_MSEC;
}

static void
handle_taken_screenshot (KasasaContentContainer *self,
                         const gchar            *uri,
                         GError                 *portal_error,
                         gboolean                retaking_screenshot)
{
  KasasaWindow *window = NULL;
  g_autoptr (GError) error = NULL;

  window = get_root_window (self);
  if (window == NULL)
    return;

  kasasa_window_hide_window (window, FALSE,
                             NULL, NULL);

  if (request_was_cancelled (portal_error)
      || (portal_error == NULL && uri == NULL))
    return;

  if (portal_error != NULL)
    {
      AdwToast *toast =  adw_toast_new_format (_("Error: %s"), portal_error->message);
      adw_toast_set_action_target_value (toast, g_variant_new_string (portal_error->message));
      adw_toast_set_button_label (toast, _("Copy"));
      adw_toast_set_action_name (toast, "toast.copy_error");
      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      g_warning ("%s", portal_error->message);
      return;
    }

  if (uri == NULL)
    {
      const gchar *error_message = _("Couldn't load the screenshot");
      AdwToast *toast = adw_toast_new (error_message);
      adw_toast_set_action_target_value (toast, g_variant_new_string (error_message));
      adw_toast_set_button_label (toast, _("Copy"));
      adw_toast_set_action_name (toast, "toast.copy_error");
      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      g_warning ("%s", error_message);
      return;
    }

  if (retaking_screenshot)
    {
      // Replace with new screenshot
      GtkWidget *current_content = get_current_content (self);
      KasasaScreenshot *screenshot;

      if (!KASASA_IS_SCREENSHOT (current_content))
        {
          const gchar *message = _("Current content is not a screenshot");
          AdwToast *toast = adw_toast_new_format (_("Error: %s"), message);

          adw_toast_overlay_add_toast (self->toast_overlay, toast);
          g_warning ("Couldn't retake screenshot: %s", message);
          return;
        }

      screenshot = KASASA_SCREENSHOT (current_content);

      if (!kasasa_screenshot_load_screenshot (screenshot, uri, &error))
        {
          const gchar *message = error != NULL
                                 ? error->message
                                 : _("Couldn't load the screenshot");
          AdwToast *toast = adw_toast_new_format (_("Error: %s"), message);
          adw_toast_overlay_add_toast (self->toast_overlay, toast);
          g_warning ("Couldn't load screenshot: %s", message);
          return;
        }

      kasasa_content_container_request_window_resize (self);
    }
  else
    {
      // Add new screenshot
      if (!kasasa_content_container_append_screenshot (self, uri, &error))
        {
          AdwToast *toast = adw_toast_new_format (_("Error: %s"), error->message);
          adw_toast_overlay_add_toast (self->toast_overlay, toast);
          g_warning ("Couldn't load screenshot: %s", error->message);
          return;
        }
    }

  // Set the focus to the retake_screenshot_button
  gtk_window_set_focus (GTK_WINDOW (window), GTK_WIDGET (self->retake_screenshot_button));
}

static void
on_screencast_new_dimension (KasasaScreencast *screencast,
                             gint              new_width,
                             gint              new_height,
                             gpointer          user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  GtkWidget *current_content = get_current_content (self);
  gboolean miniaturized = kasasa_window_is_miniaturized (window);


  if (current_content == GTK_WIDGET (screencast)
      && !miniaturized)
    kasasa_window_resize_window_scaling (window,
                                         (gdouble) new_height,
                                         (gdouble) new_width);
}

static guint
find_content_index (KasasaContentContainer *self,
                    GtkWidget              *content)
{
  guint n_pages = adw_carousel_get_n_pages (self->carousel);

  if (content == NULL)
    return GTK_INVALID_LIST_POSITION;

  for (guint i = 0; i < n_pages; i++)
    {
      if (adw_carousel_get_nth_page (self->carousel, i) == content)
        return i;
    }

  return GTK_INVALID_LIST_POSITION;
}

static void
finish_screencast_content (KasasaContentContainer *self,
                           KasasaScreencast        *screencast)
{
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  GtkWidget *current_content = get_current_content (self);
  gboolean miniaturized = kasasa_window_is_miniaturized (window);
  guint n_pages = adw_carousel_get_n_pages (self->carousel);
  guint screencast_idx = find_content_index (self, GTK_WIDGET (screencast));

  kasasa_content_container_carousel_set_interactive (self, FALSE);
  kasasa_content_finish (KASASA_CONTENT (screencast));

  if (n_pages == 1 && !miniaturized)
    {
      // resize the window to a no content view
      kasasa_content_container_request_window_resize (self);
    }
  else if (n_pages == 1 && miniaturized)
    {
      // do nothing
      // the window will present a no content view after unminiaturized
    }
  else if (n_pages >= 2
           && current_content == GTK_WIDGET (screencast))
    {
      // scroll to the neighbor content
      GtkWidget *neighbor_content = NULL;
      guint neighbor_idx;

      kasasa_window_miniaturize_window (window, FALSE);

      if (screencast_idx == GTK_INVALID_LIST_POSITION)
        {
          g_warning ("Finished screencast is not in the carousel");
        }
      else
        {
          neighbor_idx = screencast_idx == 0
                         ? screencast_idx + 1
                         : screencast_idx - 1;
          neighbor_content = adw_carousel_get_nth_page (self->carousel,
                                                        neighbor_idx);

          adw_carousel_remove (self->carousel, GTK_WIDGET (screencast));
          self->current_page_index = find_content_index (self,
                                                         neighbor_content);
          adw_carousel_scroll_to (self->carousel, neighbor_content, TRUE);

          if (miniaturized)
            kasasa_window_miniaturize_window (window, TRUE);
        }
    }
  else
    {
      // silently remove the content
      if (screencast_idx == GTK_INVALID_LIST_POSITION)
        {
          g_warning ("Finished screencast is not in the carousel");
        }
      else
        {
          adw_carousel_remove (self->carousel, GTK_WIDGET (screencast));
          self->current_page_index = find_content_index (self,
                                                         current_content);
        }
    }

  kasasa_content_container_carousel_set_interactive (self, TRUE);
  kasasa_content_container_update_toolbar_sensibility (self);
}

static void
on_screencast_eos (KasasaScreencast *screencast,
                   gpointer          user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  finish_screencast_content (self, screencast);
}

static void
on_stop_screencast_clicked (GtkButton *button,
                            gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWidget *current_content = get_current_content (self);

  if (!KASASA_IS_SCREENCAST (current_content)
      || !kasasa_screencast_is_active (
        KASASA_SCREENCAST (current_content)))
    {
      kasasa_content_container_update_toolbar_sensibility (self);
      return;
    }

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  finish_screencast_content (self, KASASA_SCREENCAST (current_content));
}

/*************************** TAKE FIRST SCREENSHOT ***************************/
// take_first_screenshot -> on_first_screenshot_taken
static void
on_first_screenshot_taken (GObject      *object,
                           GAsyncResult *res,
                           gpointer      user_data)
{
  g_autoptr (KasasaContentContainer) self = NULL;
  KasasaWindow *window = NULL;
  g_autoptr (GSettings) settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uri = NULL;
  g_autofree gchar *error_message = NULL;
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;

  uri = portal_request_data_finish_screenshot (user_data,
                                               XDP_PORTAL (object),
                                               res,
                                               &error);

  self = portal_request_data_take_container (user_data);
  if (self == NULL || (window = get_root_window (self)) == NULL)
    return;

  if (request_was_cancelled (error)
      || (error == NULL && uri == NULL))
    {
      gtk_window_close (GTK_WINDOW (window));
      return;
    }

  if (error != NULL)
    {
      g_warning ("First screenshot failed: %s", error->message);
      // translators: reason which the screenshot failed
      error_message = g_strconcat (_("Reason: "), error->message, NULL);
      goto FAIL;
    }

  if (uri == NULL)
    {
      // translators: reason which the screenshot failed
      error_message = g_strconcat (_("Reason: "), _("Couldn't load the screenshot"), NULL);
      g_warning ("%s", error_message);
      goto FAIL;
    }

  // Fresh pin session: start from the auto-fitted size (zoom = 100%)
  kasasa_window_reset_zoom (window);

  if (!kasasa_content_container_append_screenshot (self, uri, &error))
    {
      g_warning ("Couldn't load first screenshot: %s", error->message);
      error_message = g_strconcat (_("Reason: "), error->message, NULL);
      goto FAIL;
    }

  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);

  // Resize was likely deferred while the window was hidden for the portal
  // dialog; apply the fitted size now that we are visible/mapped.
  kasasa_content_container_request_window_resize (self);

  // Enable auto discard window timer
  if (g_settings_get_boolean (settings, "auto-discard-window"))
    kasasa_window_auto_discard_window (window);

  kasasa_window_miniaturize_window (window, TRUE);
  return;

FAIL:
  icon = g_themed_icon_new ("dialog-warning-symbolic");
  notification = g_notification_new (_("Screenshot failed"));
  g_notification_set_icon (notification, icon);
  if (error_message != NULL)
    g_notification_set_body (notification, error_message);
  g_application_send_notification (g_application_get_default (),
                                   "io.github.kelvinnovais.Kasasa",
                                   notification);

  gtk_window_close (GTK_WINDOW (window));
}

static void
take_first_screenshot (KasasaContentContainer *self)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  self->screenshot_portal_ops.take_screenshot (
    self->portal,
    NULL,
    XDP_SCREENSHOT_FLAG_INTERACTIVE,
    self->portal_cancellable,
    on_first_screenshot_taken,
    portal_request_data_new (self)
  );
}
/******************************************************************************/




/******************************* ADD SCREENSHOT *******************************/
// take_screenshot -> take_screenshot_cb -> on_screenshot_taken
static void
on_screenshot_taken (GObject      *object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  g_autoptr (KasasaContentContainer) self = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uri = NULL;
  KasasaWindow *window = NULL;

  uri = portal_request_data_finish_screenshot (user_data,
                                               XDP_PORTAL (object),
                                               res,
                                               &error);
  self = portal_request_data_take_container (user_data);
  if (self == NULL || (window = get_root_window (self)) == NULL)
    return;

  handle_taken_screenshot (self, uri, error, FALSE);

  kasasa_content_container_update_toolbar_sensibility (self);

  kasasa_window_block_miniaturization (window, FALSE);
}

static void
take_screenshot_cb (gpointer user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  withdraw_delayed_screenshot_notification ();

  self->screenshot_portal_ops.take_screenshot (
    self->portal,
    NULL,
    XDP_SCREENSHOT_FLAG_INTERACTIVE,
    self->portal_cancellable,
    on_screenshot_taken,
    portal_request_data_new (self)
  );
}

static void
take_screenshot (GtkButton *button,
                 gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));

  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);

  kasasa_window_block_miniaturization (window, TRUE);

  kasasa_window_hide_window (window, TRUE,
                             take_screenshot_cb, G_OBJECT (self));
}

static void
take_delayed_screenshot (GtkButton *button,
                         gpointer user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  guint interval = g_settings_get_uint (self->settings,
                                        "screenshot-delay");

  gtk_popover_popdown (self->more_actions_popover);
  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);

  kasasa_window_block_miniaturization (window, TRUE);
  kasasa_window_hide_window (window, TRUE,
                             NULL, NULL);

  kasasa_source_set_timeout_seconds_once (&self->delayed_screenshot_source,
                                          interval,
                                          take_screenshot_cb,
                                          self);
  send_delayed_screenshot_notification (interval);
}
/******************************************************************************/




/***************************** RETAKE SCREENSHOT ******************************/
// retake_screenshot -> retake_screenshot_cb -> on_screenshot_retaken
static void
on_screenshot_retaken (GObject      *object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (KasasaContentContainer) self = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uri = NULL;
  KasasaWindow *window = NULL;

  uri = portal_request_data_finish_screenshot (user_data,
                                               XDP_PORTAL (object),
                                               res,
                                               &error);
  self = portal_request_data_take_container (user_data);
  if (self == NULL || (window = get_root_window (self)) == NULL)
    return;

  kasasa_window_block_miniaturization (window, FALSE);

  handle_taken_screenshot (self, uri, error, TRUE);

  kasasa_content_container_update_toolbar_sensibility (self);

  // Enable carousel again
  kasasa_content_container_carousel_set_interactive (self, TRUE);
}

static void
retake_screenshot_cb (gpointer user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  // Avoid changing the carousel page
  kasasa_content_container_carousel_set_interactive (self, FALSE);

  self->screenshot_portal_ops.take_screenshot (
    self->portal,
    NULL,
    XDP_SCREENSHOT_FLAG_INTERACTIVE,
    self->portal_cancellable,
    on_screenshot_retaken,
    portal_request_data_new (self)
  );
}

static void
retake_screenshot (GtkButton *button, gpointer user_data)
{
  KasasaContentContainer *self = NULL;
  KasasaWindow *window = NULL;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (user_data));

  self = KASASA_CONTENT_CONTAINER (user_data);
  window = kasasa_window_get_window_reference (GTK_WIDGET (self));

  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);

  kasasa_window_block_miniaturization (window, TRUE);

  kasasa_window_hide_window (window, TRUE,
                             retake_screenshot_cb, G_OBJECT (self));
}
/******************************************************************************/




/********************************* SCREENCAST *********************************/
// create_screencast_session -> create_screencast_session_cb -> on_screencast_session_started
static void
on_first_screencast_ready (KasasaContentContainer *self,
                           KasasaWindow           *window)
{
  g_autoptr (GSettings) settings = g_settings_new ("io.github.kelvinnovais.Kasasa");

  // Fresh pin session: start from the auto-fitted size (zoom = 100%)
  kasasa_window_reset_zoom (window);

  // First screencast keeps the surface mapped (opacity 0) so GStreamer can
  // attach; restore full opacity now that the pin is ready.
  gtk_widget_set_opacity (GTK_WIDGET (window), 1.0);

  // Apply the fitted size now that stream dimensions are known.
  kasasa_content_container_request_window_resize (self);

  // Enable auto discard window timer
  if (g_settings_get_boolean (settings, "auto-discard-window"))
    kasasa_window_auto_discard_window (window);

  kasasa_window_miniaturize_window (window, TRUE);
}

static void
fail_first_screencast (KasasaContentContainer *self,
                       KasasaWindow           *window,
                       const gchar            *fallback_message,
                       const GError           *error)
{
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;
  g_autofree gchar *error_message = NULL;
  const gchar *detail = error != NULL ? error->message : fallback_message;

  if (detail != NULL)
    {
      // translators: reason which the screencast failed
      error_message = g_strconcat (_("Reason: "), detail, NULL);
      g_warning ("First screencast failed: %s", detail);
    }

  icon = g_themed_icon_new ("dialog-warning-symbolic");
  notification = g_notification_new (_("Screencast failed"));
  g_notification_set_icon (notification, icon);
  if (error_message != NULL)
    g_notification_set_body (notification, error_message);
  g_application_send_notification (g_application_get_default (),
                                   "io.github.kelvinnovais.Kasasa",
                                   notification);

  gtk_window_close (GTK_WINDOW (window));
}

static void
dismiss_screencast_request_toast (KasasaContentContainer *self)
{
  if (self->screencast_request_toast == NULL)
    return;

  adw_toast_dismiss (self->screencast_request_toast);
  g_clear_object (&self->screencast_request_toast);
}

static void
finish_screencast_request (KasasaContentContainer *self)
{
  KasasaWindow *window;

  self->screencast_request_pending = FALSE;
  self->screencast_first_capture = FALSE;
  kasasa_source_clear (&self->screencast_create_timeout_source);
  dismiss_screencast_request_toast (self);
  g_clear_object (&self->screencast_cancellable);

  window = get_root_window (self);
  if (window != NULL)
    kasasa_window_block_miniaturization (window, FALSE);

  kasasa_content_container_update_toolbar_sensibility (self);
}

static gboolean
cancel_screencast_request_internal (KasasaContentContainer *self)
{
  g_autoptr (GCancellable) cancellable = NULL;

  if (!self->screencast_request_pending)
    return FALSE;

  if (self->screencast_cancellable != NULL)
    cancellable = g_object_ref (self->screencast_cancellable);

  finish_screencast_request (self);
  if (cancellable != NULL)
    g_cancellable_cancel (cancellable);

  return TRUE;
}

gboolean
kasasa_content_container_cancel_screencast_request (
  KasasaContentContainer *self)
{
  KasasaWindow *window;
  gboolean first_capture;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  first_capture = self->screencast_first_capture;
  window = get_root_window (self);
  if (!cancel_screencast_request_internal (self))
    return FALSE;

  if (first_capture && window != NULL)
    gtk_window_close (GTK_WINDOW (window));

  return TRUE;
}

static void
on_screencast_create_timeout (gpointer user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = get_root_window (self);
  gboolean first_capture = self->screencast_first_capture;
  const gchar *message = _("The screencast service did not respond");

  if (!cancel_screencast_request_internal (self))
    return;

  if (first_capture && window != NULL)
    fail_first_screencast (self, window, message, NULL);
  else
    show_operation_error (self, message, NULL);
}

static void
cancel_screencast_request_action (GtkWidget  *sender,
                                  const char *action,
                                  GVariant   *param)
{
  kasasa_content_container_cancel_screencast_request (
    KASASA_CONTENT_CONTAINER (sender));
}

static void
on_screencast_session_started (GObject      *source_object,
                               GAsyncResult *res,
                               gpointer      data)
{
  gint fd;
  gint stream_width = 0;
  gint stream_height = 0;
  guint node_id;

  g_autoptr (GError) error = NULL;
  g_autoptr (GError) pipeline_error = NULL;
  g_autoptr (GVariant) streams = NULL;
  g_autoptr (GVariant) stream = NULL;
  g_autoptr (GVariant) stream_properties = NULL;
  g_autoptr (KasasaContentContainer) self = NULL;
  KasasaScreencast *screencast = NULL;
  KasasaWindow *window = NULL;
  XdpSession *session = NULL;
  gboolean success;
  gboolean first_capture;
  gboolean cancelled;
  guint request_id;
  KasasaPortalStartScreencastSessionFinishFunc start_session_finish;
  ScreencastRequestData *request_data = data;

  first_capture = request_data->first_capture;
  request_id = request_data->request_id;
  start_session_finish = request_data->start_session_finish;
  session = XDP_SESSION (source_object);
  success = start_session_finish (session, res, &error);

  self = screencast_request_data_take_container (data);
  if (self == NULL || !screencast_request_is_current (self, request_id))
    {
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  window = get_root_window (self);
  if (window == NULL)
    {
      finish_screencast_request (self);
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  if (error != NULL || !success)
    {
      cancelled = request_was_cancelled (error)
                  || (error == NULL && !success);
      finish_screencast_request (self);
      if (cancelled)
        {
          if (first_capture)
            gtk_window_close (GTK_WINDOW (window));
        }
      else if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("Couldn't start the screencast"),
                                 error);
        }
      else
        {
          show_operation_error (self,
                                _("Couldn't start the screencast"),
                                error);
        }
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  streams = xdp_session_get_streams (session);
  if (streams == NULL || g_variant_n_children (streams) < 1)
    {
      finish_screencast_request (self);
      if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("The screencast did not provide a video stream"),
                                 NULL);
        }
      else
        {
          show_operation_error (self,
                                _("The screencast did not provide a video stream"),
                                NULL);
        }
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  stream = g_variant_get_child_value (streams, 0);
  if (!g_variant_is_of_type (stream, G_VARIANT_TYPE ("(ua{sv})")))
    {
      finish_screencast_request (self);
      if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("The screencast provided invalid stream information"),
                                 NULL);
        }
      else
        {
          show_operation_error (self,
                                _("The screencast provided invalid stream information"),
                                NULL);
        }
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  g_variant_get (stream,
                 "(u@a{sv})", &node_id, &stream_properties);
  g_variant_lookup (stream_properties,
                    "size",
                    "(ii)",
                    &stream_width,
                    &stream_height);

  fd = xdp_session_open_pipewire_remote (session);
  if (fd < 0)
    {
      finish_screencast_request (self);
      if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("Couldn't connect to the screencast service"),
                                 NULL);
        }
      else
        {
          show_operation_error (self,
                                _("Couldn't connect to the screencast service"),
                                NULL);
        }
      xdp_session_close (session);
      g_object_unref (session);
      return;
    }

  {
    g_autofree gchar *streams_description = g_variant_print (streams, TRUE);
    g_debug ("Streams: %s", streams_description);
  }

  screencast = kasasa_screencast_new ();

  g_signal_connect (screencast, "new-dimension",
                    G_CALLBACK (on_screencast_new_dimension), self);
  g_signal_connect (screencast, "eos",
                    G_CALLBACK (on_screencast_eos), self);
  g_signal_connect (screencast, "cpu-fallback",
                    G_CALLBACK (on_screencast_cpu_fallback), self);

  if (!kasasa_screencast_show (screencast,
                               session,
                               fd,
                               node_id,
                               stream_width,
                               stream_height,
                               &pipeline_error))
    {
      g_object_unref (screencast);
      finish_screencast_request (self);
      if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("Couldn't display the screencast"),
                                 pipeline_error);
        }
      else
        {
          show_operation_error (self,
                                _("Couldn't display the screencast"),
                                pipeline_error);
        }
      return;
    }
  adw_carousel_append (self->carousel, GTK_WIDGET (screencast));
  adw_carousel_scroll_to (self->carousel, GTK_WIDGET (screencast), TRUE);
  finish_screencast_request (self);

  if (first_capture)
    on_first_screencast_ready (self, window);
}

static void
create_screencast_session_cb (GObject      *source_object,
                              GAsyncResult *res,
                              gpointer      data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (KasasaContentContainer) self = NULL;
  KasasaWindow *window = NULL;
  XdpSession *session = NULL;
  gboolean first_capture;
  gboolean cancelled;
  guint request_id;
  KasasaPortalCreateScreencastSessionFinishFunc create_session_finish;
  ScreencastRequestData *request_data = data;

  first_capture = request_data->first_capture;
  request_id = request_data->request_id;
  create_session_finish = request_data->create_session_finish;
  session = create_session_finish (XDP_PORTAL (source_object), res, &error);

  self = screencast_request_data_take_container (data);
  if (self == NULL || !screencast_request_is_current (self, request_id))
    {
      if (session != NULL)
        {
          xdp_session_close (session);
          g_object_unref (session);
        }
      return;
    }

  window = get_root_window (self);
  if (window == NULL)
    {
      finish_screencast_request (self);
      if (session != NULL)
        {
          xdp_session_close (session);
          g_object_unref (session);
        }
      return;
    }

  if (error != NULL || session == NULL)
    {
      cancelled = request_was_cancelled (error)
                  || (error == NULL && session == NULL);
      finish_screencast_request (self);
      if (cancelled)
        {
          if (first_capture)
            gtk_window_close (GTK_WINDOW (window));
        }
      else if (first_capture)
        {
          fail_first_screencast (self,
                                 window,
                                 _("Couldn't create the screencast"),
                                 error);
        }
      else
        {
          show_operation_error (self,
                                _("Couldn't create the screencast"),
                                error);
        }
      if (session != NULL)
        {
          xdp_session_close (session);
          g_object_unref (session);
        }
      return;
    }

  /* Creating the Portal session succeeded.  From this point onward the user
   * may legitimately spend an arbitrary amount of time in the source chooser. */
  kasasa_source_clear (&self->screencast_create_timeout_source);

  if (!self->parent)
    get_parent (self);

  self->screencast_portal_ops.start_session (
    session,
    self->parent,
    self->screencast_cancellable,
    on_screencast_session_started,
    screencast_request_data_new (self, first_capture, request_id));
}

static void
start_screencast_session (KasasaContentContainer *self,
                          gboolean                first_capture)
{
  KasasaWindow *window = get_root_window (self);

  g_return_if_fail (window != NULL);

  if (self->screencast_request_pending)
    return;

  self->screencast_request_id++;
  if (self->screencast_request_id == 0)
    self->screencast_request_id++;
  self->screencast_request_pending = TRUE;
  self->screencast_first_capture = first_capture;
  self->screencast_cancellable = g_cancellable_new ();

  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);
  kasasa_window_block_miniaturization (window, TRUE);

  self->screencast_request_toast =
    adw_toast_new (_("Waiting for screen selection…"));
  adw_toast_set_timeout (self->screencast_request_toast, 0);
  adw_toast_set_priority (self->screencast_request_toast,
                          ADW_TOAST_PRIORITY_HIGH);
  adw_toast_set_button_label (self->screencast_request_toast, _("Cancel"));
  adw_toast_set_action_name (self->screencast_request_toast,
                             "screencast.cancel");
  adw_toast_overlay_add_toast (self->toast_overlay,
                               g_object_ref (self->screencast_request_toast));

  kasasa_source_set_timeout_once (&self->screencast_create_timeout_source,
                                  self->screencast_create_timeout_ms,
                                  on_screencast_create_timeout,
                                  self);

  self->screencast_portal_ops.create_session (
    self->portal,
    XDP_OUTPUT_WINDOW,
    XDP_SCREENCAST_FLAG_NONE,
    XDP_CURSOR_MODE_HIDDEN,
    XDP_PERSIST_MODE_TRANSIENT,
    NULL,
    self->screencast_cancellable,
    create_screencast_session_cb,
    screencast_request_data_new (self,
                                 first_capture,
                                 self->screencast_request_id));
}

static void
create_screencast_session (GtkButton *button,
                           gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  gtk_popover_popdown (self->more_actions_popover);

  if (has_active_screencast (self))
    {
      AdwToast *toast = adw_toast_new (
        _("Finish the current screencast before starting another one."));

      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      return;
    }

  start_screencast_session (self, FALSE);
}
/******************************************************************************/




void
kasasa_content_container_request_first_screenshot (KasasaContentContainer *self)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  take_first_screenshot (self);
}

void
kasasa_content_container_load_first_screenshot_uri (KasasaContentContainer *self,
                                                    const gchar            *uri)
{
  KasasaWindow *window = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *error_message = NULL;
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (uri != NULL);

  window = get_root_window (self);
  if (window == NULL)
    return;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");

  if (uri[0] == '\0')
    {
      error_message = g_strconcat (_("Reason: "),
                                   _("Couldn't load the screenshot"),
                                   NULL);
      goto FAIL;
    }

  kasasa_window_reset_zoom (window);

  if (!kasasa_content_container_append_screenshot (self, uri, &error))
    {
      g_warning ("Couldn't load first screenshot: %s", error->message);
      error_message = g_strconcat (_("Reason: "), error->message, NULL);
      goto FAIL;
    }

  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  kasasa_window_finish_initial_reveal (window);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    kasasa_window_auto_discard_window (window);

  kasasa_window_miniaturize_window (window, TRUE);
  return;

FAIL:
  icon = g_themed_icon_new ("dialog-warning-symbolic");
  notification = g_notification_new (_("Screenshot failed"));
  g_notification_set_icon (notification, icon);
  if (error_message != NULL)
    g_notification_set_body (notification, error_message);
  g_application_send_notification (g_application_get_default (),
                                   "io.github.kelvinnovais.Kasasa",
                                   notification);
  gtk_window_close (GTK_WINDOW (window));
}

void
kasasa_content_container_request_first_screencast (KasasaContentContainer *self)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  start_screencast_session (self, TRUE);
}

void
kasasa_content_container_request_screencast (KasasaContentContainer *self)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  start_screencast_session (self, FALSE);
}

static gboolean
append_hyprland_monitor_screencast (KasasaContentContainer *self,
                                    const KasasaMonitor    *monitor,
                                    GError                **error)
{
  KasasaScreencast *screencast;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);
  g_return_val_if_fail (monitor != NULL, FALSE);

  screencast = kasasa_screencast_new ();
  g_signal_connect (screencast, "new-dimension",
                    G_CALLBACK (on_screencast_new_dimension), self);
  g_signal_connect (screencast, "eos",
                    G_CALLBACK (on_screencast_eos), self);

  if (!kasasa_screencast_show_hyprland_output (screencast,
                                               monitor->name,
                                               monitor->width,
                                               monitor->height,
                                               error))
    {
      g_object_unref (screencast);
      return FALSE;
    }

  adw_carousel_append (self->carousel, GTK_WIDGET (screencast));
  adw_carousel_scroll_to (self->carousel, GTK_WIDGET (screencast), TRUE);
  kasasa_content_container_update_toolbar_sensibility (self);
  return TRUE;
}

void
kasasa_content_container_load_first_hyprland_monitor_screencast (
  KasasaContentContainer *self,
  const gchar            *monitor_name,
  gint                    width,
  gint                    height)
{
  KasasaWindow *window;
  KasasaMonitor monitor = { 0 };
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (monitor_name != NULL && *monitor_name != '\0');

  window = get_root_window (self);
  if (window == NULL)
    return;

  monitor.name = (gchar *) monitor_name;
  monitor.width = width;
  monitor.height = height;

  if (!append_hyprland_monitor_screencast (self, &monitor, &error))
    {
      g_warning ("Couldn't start Hyprland monitor screencast: %s",
                 error != NULL ? error->message : "unknown");
      icon = g_themed_icon_new ("dialog-warning-symbolic");
      notification = g_notification_new (_("Screencast failed"));
      g_notification_set_icon (notification, icon);
      g_notification_set_body (notification,
                               error != NULL
                               ? error->message
                               : _("Couldn't display the monitor"));
      g_application_send_notification (g_application_get_default (),
                                       "io.github.kelvinnovais.Kasasa",
                                       notification);
      gtk_window_close (GTK_WINDOW (window));
      return;
    }

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  kasasa_window_reset_zoom (window);
  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  kasasa_window_finish_initial_reveal (window);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    kasasa_window_auto_discard_window (window);

  kasasa_window_miniaturize_window (window, TRUE);
}

void
kasasa_content_container_load_first_hyprland_screencast (KasasaContentContainer *self,
                                                         guint32                 window_handle,
                                                         gint                    width,
                                                         gint                    height)
{
  KasasaWindow *window = NULL;
  KasasaScreencast *screencast = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *error_message = NULL;
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  window = get_root_window (self);
  if (window == NULL)
    return;

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  screencast = kasasa_screencast_new ();

  g_signal_connect (screencast, "new-dimension",
                    G_CALLBACK (on_screencast_new_dimension), self);
  g_signal_connect (screencast, "eos",
                    G_CALLBACK (on_screencast_eos), self);

  if (!kasasa_screencast_show_hyprland (screencast,
                                        window_handle,
                                        width,
                                        height,
                                        &error))
    {
      g_warning ("Couldn't start Hyprland screencast: %s",
                 error != NULL ? error->message : "unknown");
      error_message = g_strconcat (_("Reason: "),
                                   error != NULL
                                   ? error->message
                                   : _("Couldn't display the screencast"),
                                   NULL);
      g_object_unref (screencast);
      goto FAIL;
    }

  adw_carousel_append (self->carousel, GTK_WIDGET (screencast));
  adw_carousel_scroll_to (self->carousel, GTK_WIDGET (screencast), TRUE);
  kasasa_content_container_update_toolbar_sensibility (self);

  kasasa_window_reset_zoom (window);
  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  kasasa_window_finish_initial_reveal (window);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    kasasa_window_auto_discard_window (window);

  kasasa_window_miniaturize_window (window, TRUE);
  return;

FAIL:
  icon = g_themed_icon_new ("dialog-warning-symbolic");
  notification = g_notification_new (_("Screencast failed"));
  g_notification_set_icon (notification, icon);
  if (error_message != NULL)
    g_notification_set_body (notification, error_message);
  g_application_send_notification (g_application_get_default (),
                                   "io.github.kelvinnovais.Kasasa",
                                   notification);
  gtk_window_close (GTK_WINDOW (window));
}

gboolean
kasasa_content_container_cancel_delayed_screenshot (KasasaContentContainer *self)
{
  KasasaWindow *window;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  if (self->delayed_screenshot_source.id == 0)
    return FALSE;

  kasasa_source_clear (&self->delayed_screenshot_source);
  withdraw_delayed_screenshot_notification ();

  window = get_root_window (self);
  if (window != NULL)
    {
      kasasa_window_hide_window (window, FALSE, NULL, NULL);
      kasasa_window_block_miniaturization (window, FALSE);
    }

  kasasa_content_container_update_toolbar_sensibility (self);
  return TRUE;
}

static GtkWidget *
get_current_content (KasasaContentContainer *self)
{
  guint n_pages = adw_carousel_get_n_pages (self->carousel);
  gint position;

  if (n_pages == 0)
    return NULL;

  if (self->current_page_index < n_pages)
    return adw_carousel_get_nth_page (self->carousel,
                                      self->current_page_index);

  // Fallback for the first page, before AdwCarousel emits page-changed.
  position = (gint) round (adw_carousel_get_position (self->carousel));
  position = CLAMP (position, 0, (gint) n_pages - 1);

  g_debug ("Carousel current position: %d", position);

  return adw_carousel_get_nth_page (self->carousel, (guint) position);
}

gboolean
kasasa_content_container_switch_page (KasasaContentContainer *self,
                                      gint                    offset)
{
  GtkWidget *target_page = NULL;
  guint n_pages;
  gint current_index;
  gint target_index;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  n_pages = adw_carousel_get_n_pages (self->carousel);
  if (offset == 0 || n_pages < 2 || self->carousel_interaction_locks > 0)
    return FALSE;

  current_index = self->current_page_index < n_pages
                  ? (gint) self->current_page_index
                  : (gint) round (adw_carousel_get_position (self->carousel));
  current_index = CLAMP (current_index, 0, (gint) n_pages - 1);
  target_index = CLAMP (current_index + offset, 0, (gint) n_pages - 1);

  if (target_index == current_index)
    return FALSE;

  target_page = adw_carousel_get_nth_page (self->carousel,
                                           (guint) target_index);
  if (target_page == NULL)
    return FALSE;

  adw_carousel_scroll_to (self->carousel, target_page, TRUE);
  return TRUE;
}

static void
on_mouse_enter_controls (GtkEventControllerMotion *event_controller_motion,
                        gdouble                   x,
                        gdouble                   y,
                        gpointer                  user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));

  kasasa_window_change_opacity (window, OPACITY_INCREASE);
}

static void
on_mouse_leave_controls (GtkEventControllerMotion *event_controller_motion,
                        gdouble                   x,
                        gdouble                   y,
                        gpointer                  user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));

  kasasa_window_change_opacity (window, OPACITY_DECREASE);
}

static void
on_page_changed (AdwCarousel *carousel,
                 guint        index,
                 gpointer     user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  GtkWidget *content = NULL;
  guint n_pages = adw_carousel_get_n_pages (carousel);
  gboolean initial_reveal;
  gint new_height, new_width;

  g_debug ("Page changed");
  // If the carousel is empty, return
  if (index == GTK_INVALID_LIST_POSITION || index >= n_pages)
    return;

  initial_reveal = kasasa_window_is_initial_reveal_pending (window)
                   && self->current_page_index == GTK_INVALID_LIST_POSITION
                   && n_pages == 1;
  self->current_page_index = index;

  // Ensure that the window is visible
  kasasa_window_change_opacity (window, OPACITY_INCREASE);

  g_debug ("Resizing window for content at index %u due to page change", index);
  content = adw_carousel_get_nth_page (carousel, index);
  if (!KASASA_IS_CONTENT (content))
    return;

  kasasa_content_container_update_toolbar_sensibility (self);

  /* The first CLI capture submits one authoritative resize after its content
   * is loaded. Resizing here as well exposes intermediate CSD geometry. */
  if (initial_reveal)
    {
      g_debug ("Deferring first-page resize to initial reveal");
      return;
    }

  kasasa_content_get_dimensions (KASASA_CONTENT (content),
                                 &new_height,
                                 &new_width);

  kasasa_window_resize_for_content_switch (
    window,
    (gdouble) new_height,
    (gdouble) new_width,
    (KasasaSwitchResizeMode) g_settings_get_uint (
      self->settings, "image-switch-resize-mode"));
}

static void
on_remove_content_clicked (GtkButton *button,
                           gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  guint current_index = GTK_INVALID_LIST_POSITION;
  guint n_pages = adw_carousel_get_n_pages (self->carousel);
  GtkWidget *current_content = get_current_content (self);
  GtkWidget *neighbor_content = NULL;

  kasasa_content_container_carousel_set_interactive (self, FALSE);

  for (guint i = 0; i < n_pages; i++)
    {
      if (adw_carousel_get_nth_page (self->carousel, i) == current_content)
        {
          current_index = i;
          break;
        }
    }

  if (current_index == GTK_INVALID_LIST_POSITION)
    {
      kasasa_content_container_carousel_set_interactive (self, TRUE);
      return;
    }

  // Use the finish implementation for each class
  kasasa_content_finish (KASASA_CONTENT (current_content));

  /*
   * After calling 'adw_carousel_remove ()' a 'page-changed' signal is not emitted,
   * so the window don't get resized; also, calling 'resize_window ()', it access
   * a wrong carousel index due to a race condition.
   *
   * Workaround: get the neighbor content and forcibly scroll to it; to delete
   * a content, the window must hold at least 2 contents
   */
  if (current_index == 0)
    {
      // If the deleted content is at index 0, get the next one...
      neighbor_content = adw_carousel_get_nth_page (self->carousel,
                                                    current_index + 1);
    }
  else
    {
      // ...otherwise, always get the previous one.
      neighbor_content = adw_carousel_get_nth_page (self->carousel,
                                                    current_index - 1);
    }

  adw_carousel_remove (self->carousel, current_content);

  self->current_page_index = current_index == 0 ? 0 : current_index - 1;

  adw_carousel_scroll_to (self->carousel, neighbor_content, TRUE);

  kasasa_content_container_update_toolbar_sensibility (self);

  kasasa_content_container_carousel_set_interactive (self, TRUE);
}

// Copy the image to the clipboard
static void
on_copy_screenshot_button_clicked (GtkButton *button,
                                   gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  g_autoptr (GError) error = NULL;
  GdkClipboard *clipboard = NULL;
  g_autoptr (GdkTexture) texture = NULL;
  AdwToast *toast = NULL;
  GtkWidget *content = NULL;

  content = get_current_content (self);

  g_return_if_fail (KASASA_IS_SCREENSHOT (content));

  clipboard = gdk_display_get_clipboard (gdk_display_get_default ());

  texture =
    gdk_texture_new_from_file (kasasa_screenshot_get_file (KASASA_SCREENSHOT (content)),
                               &error);

  if (error != NULL)
    {
      toast = adw_toast_new_format (_("Error: %s"), error->message);
      adw_toast_set_action_target_value (toast, g_variant_new_string (error->message));
      adw_toast_set_button_label (toast, _("Copy"));
      adw_toast_set_action_name (toast, "toast.copy_error");
      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      g_warning ("%s", error->message);

      // Make the copy button insensitive on failure
      gtk_widget_set_sensitive (GTK_WIDGET (self->copy_screenshot_button), FALSE);

      return;
    }

  gdk_clipboard_set_texture (clipboard, texture);
  toast = adw_toast_new (_("Copied to the clipboard"));
  adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
on_menu_button_active (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaWindow *window = kasasa_window_get_window_reference (GTK_WIDGET (self));
  gboolean active;

  active = gtk_menu_button_get_active (self->more_actions_button);
  kasasa_window_set_controls_popup_active (window, active);
}


static void
copy_error_cb (GtkWidget  *sender,
               const char *action,
               GVariant   *param)
{
  GdkClipboard *clipboard = NULL;
  clipboard = gdk_display_get_clipboard (gdk_display_get_default ());

  // Copy the error message to the clipboard
  gdk_clipboard_set_text (clipboard, g_variant_get_string (param, NULL));
}

static void
kasasa_content_container_dispose (GObject *object)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (object);

  withdraw_delayed_screenshot_notification ();
  kasasa_source_clear (&self->delayed_screenshot_source);
  kasasa_source_clear (&self->screencast_create_timeout_source);
  self->screencast_request_pending = FALSE;
  self->screencast_first_capture = FALSE;
  dismiss_screencast_request_toast (self);
  if (self->screencast_cancellable != NULL)
    g_cancellable_cancel (self->screencast_cancellable);
  g_clear_object (&self->screencast_cancellable);
  if (self->portal_cancellable != NULL)
    g_cancellable_cancel (self->portal_cancellable);
  g_clear_object (&self->portal_cancellable);
  g_clear_object (&self->portal);
  g_clear_object (&self->settings);
  if (self->parent)
    {
      xdp_parent_free (self->parent);
      self->parent = NULL;
    }

  /* Finishing a carousel animation during template disposal can emit
   * page-changed after the container has already been finalized. */
  if (self->carousel != NULL)
    g_signal_handlers_disconnect_by_data (self->carousel, self);

  gtk_widget_dispose_template (GTK_WIDGET (object), KASASA_TYPE_CONTENT_CONTAINER);

  G_OBJECT_CLASS (kasasa_content_container_parent_class)->dispose (object);
}

static void
kasasa_content_container_class_init (KasasaContentContainerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kasasa_content_container_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/io/github/kelvinnovais/Kasasa/kasasa-content-container.ui");

  gtk_widget_class_install_action (widget_class, "toast.copy_error", "s", copy_error_cb);
  gtk_widget_class_install_action (widget_class,
                                   "screencast.cancel",
                                   NULL,
                                   cancel_screencast_request_action);

  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, carousel);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, retake_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_delayed_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_hyprland_monitor_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, remove_content_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, stop_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, copy_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, more_actions_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, revealer_start_buttons);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, revealer_end_buttons);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, toolbar_overlay);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, more_actions_popover);
}

static void
kasasa_content_container_init (KasasaContentContainer *self)
{
  GtkEventController *toolbar_motion_event_controller = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  self->portal = xdp_portal_new ();
  self->parent = NULL;
  self->settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  self->portal_cancellable = g_cancellable_new ();
  self->screencast_cancellable = NULL;
  self->screencast_request_toast = NULL;
  self->delayed_screenshot_source.id = 0;
  self->screencast_create_timeout_source.id = 0;
  self->screenshot_portal_ops = default_screenshot_portal_ops;
  self->screencast_portal_ops = default_screencast_portal_ops;
  self->carousel_interaction_locks = 0;
  self->current_page_index = GTK_INVALID_LIST_POSITION;
  self->screencast_request_id = 0;
  self->screencast_create_timeout_ms = SCREENCAST_CREATE_TIMEOUT_MSEC;
  self->screencast_request_pending = FALSE;
  self->screencast_first_capture = FALSE;

  // Signals
  g_signal_connect (self->carousel,
                    "page-changed",
                    G_CALLBACK (on_page_changed),
                    self);
  g_signal_connect (self->retake_screenshot_button,
                    "clicked",
                    G_CALLBACK (retake_screenshot),
                    self);
  g_signal_connect (self->add_screenshot_button,
                    "clicked",
                    G_CALLBACK (take_screenshot),
                    self);
  g_signal_connect (self->add_delayed_screenshot_button,
                    "clicked",
                    G_CALLBACK (take_delayed_screenshot),
                    self);
  g_signal_connect (self->add_screencast_button,
                    "clicked",
                    G_CALLBACK (create_screencast_session),
                    self);
  g_signal_connect (self->add_hyprland_monitor_screencast_button,
                    "clicked",
                    G_CALLBACK (create_hyprland_monitor_screencast),
                    self);
  g_signal_connect (self->remove_content_button,
                    "clicked",
                    G_CALLBACK (on_remove_content_clicked),
                    self);
  g_signal_connect (self->stop_screencast_button,
                    "clicked",
                    G_CALLBACK (on_stop_screencast_clicked),
                    self);
  g_signal_connect (self->copy_screenshot_button,
                    "clicked",
                    G_CALLBACK (on_copy_screenshot_button_clicked),
                    self);
  g_signal_connect (self->more_actions_button,
                    "notify::active",
                    G_CALLBACK (on_menu_button_active),
                    self);

  // Event controllers
  toolbar_motion_event_controller = gtk_event_controller_motion_new ();
  g_signal_connect (toolbar_motion_event_controller,
                    "enter",
                    G_CALLBACK (on_mouse_enter_controls),
                    self);
  g_signal_connect (toolbar_motion_event_controller,
                    "leave",
                    G_CALLBACK (on_mouse_leave_controls),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self->toolbar_overlay),
                             toolbar_motion_event_controller);
}

KasasaContentContainer *
kasasa_content_container_new (void)
{
  return KASASA_CONTENT_CONTAINER (g_object_new (KASASA_TYPE_CONTENT_CONTAINER,
                                                 NULL));
}
