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

#include <glib/gi18n.h>
#include <math.h>

#include "kasasa-content-container.h"

#include "kasasa-hyprland-capture.h"
#include "kasasa-hyprland-stream.h"
#include "kasasa-region-capture.h"
#include "kasasa-screenshot.h"
#include "kasasa-screencast.h"
#include "kasasa-source.h"
#include "kasasa-window-picker.h"
#include "kasasa-window-query.h"

#define DELAYED_SCREENSHOT_NOTIFICATION_ID "delayed-screenshot"

struct _KasasaContentContainer
{
  AdwBreakpointBin         parent_instance;

  /* Template widgets */
  AdwToastOverlay         *toast_overlay;
  AdwCarousel             *carousel;
  GtkButton               *retake_screenshot_button;
  GtkButton               *add_screenshot_button;
  GtkButton               *add_window_screenshot_button;
  GtkButton               *add_delayed_screenshot_button;
  GtkButton               *add_screencast_button;
  GtkButton               *add_hyprland_monitor_screencast_button;
  GtkButton               *remove_content_button;
  GtkButton               *stop_screencast_button;
  GtkButton               *crop_screencast_button;
  GtkButton               *crop_reset_button;
  GtkButton               *crop_cancel_button;
  GtkButton               *crop_confirm_button;
  GtkButton               *copy_screenshot_button;
  GtkMenuButton           *more_actions_button;
  GtkRevealer             *revealer_end_buttons;
  GtkRevealer             *revealer_start_buttons;
  GtkOverlay              *toolbar_overlay;
  GtkPopover              *more_actions_popover;

  /* Instance variables */
  GSettings               *settings;
  KasasaCaptureController *capture_controller;
  AdwToast                *delayed_screenshot_toast;
  guint                    carousel_interaction_locks;
  guint                    current_page_index;
  KasasaContentHostOps       host_ops;
  gpointer                   host_data;
  GDestroyNotify             host_data_destroy;
};

static const KasasaNativeCaptureOps default_native_capture_ops = {
  .screenshot_available = kasasa_hyprland_capture_available,
  .screencast_available = kasasa_hyprland_stream_available,
  .present_picker = kasasa_window_picker_present,
  .capture_screenshot_async = kasasa_hyprland_capture_screenshot_async,
  .capture_screenshot_finish = kasasa_hyprland_capture_screenshot_finish,
  .window_handle_from_address = kasasa_hyprland_stream_handle_from_address,
};

static const KasasaRegionCaptureOps default_region_capture_ops = {
  .available = kasasa_region_capture_available,
  .capture_async = kasasa_region_capture_screenshot_async,
  .capture_finish = kasasa_region_capture_screenshot_finish,
};

G_DEFINE_FINAL_TYPE (KasasaContentContainer, kasasa_content_container, ADW_TYPE_BREAKPOINT_BIN)

static GtkWidget * get_current_content (KasasaContentContainer *self);
static gboolean append_hyprland_monitor_screencast (
  KasasaContentContainer *self,
  const KasasaMonitor    *monitor,
  GError                **error);
static gboolean append_hyprland_screencast (KasasaContentContainer *self,
                                             guint32                 window_handle,
                                             gint                    width,
                                             gint                    height,
                                             GError                **error);
static void show_operation_error (KasasaContentContainer *self,
                                  const gchar            *fallback_message,
                                  const GError           *error);
static void fail_first_screencast (KasasaContentContainer *self,
                                   GtkWindow              *window,
                                   const gchar            *fallback_message,
                                   const GError           *error);
static void begin_native_capture_request (KasasaContentContainer *self,
                                          KasasaNativeCaptureKind kind);
static void begin_region_capture_request (KasasaContentContainer *self,
                                          KasasaRegionCaptureKind kind);
static void load_first_screenshot_uri (KasasaContentContainer *self,
                                       const gchar            *uri,
                                       KasasaScreenshotSource  source);
static void on_native_screenshot_captured (gpointer                user_data,
                                           KasasaNativeCaptureKind kind,
                                           const gchar             *uri,
                                           const GError            *error);
static void on_region_screenshot_captured (gpointer                user_data,
                                           KasasaRegionCaptureKind kind,
                                           const gchar             *uri,
                                           const GError            *error);
static void on_native_screencast_selected (gpointer                user_data,
                                           KasasaNativeCaptureKind kind,
                                           guint32                  handle,
                                           gint                     width,
                                           gint                     height,
                                           const GError            *error);
static void on_capture_finished (gpointer                user_data,
                                 KasasaNativeCaptureKind kind);
static void on_region_capture_finished (gpointer                 user_data,
                                        KasasaRegionCaptureKind  kind);
static void on_capture_cancelled (gpointer                user_data,
                                  KasasaNativeCaptureKind kind,
                                  gboolean                 first_capture);
static void on_region_capture_cancelled (gpointer                 user_data,
                                         KasasaRegionCaptureKind kind,
                                         gboolean                 first_capture);
static void on_delayed_screenshot_scheduled (gpointer user_data,
                                             guint    interval);
static void on_delayed_screenshot_started (gpointer user_data);

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
on_screencast_dmabuf_fallback (KasasaScreencast     *screencast,
                               KasasaContentContainer *self)
{
  AdwToast *toast;

  toast = adw_toast_new (
    _("Hardware-accelerated preview unavailable. Switched to compatibility mode."));
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

static void
dismiss_delayed_screenshot_toast (KasasaContentContainer *self)
{
  if (self->delayed_screenshot_toast == NULL)
    return;

  adw_toast_dismiss (self->delayed_screenshot_toast);
  g_clear_object (&self->delayed_screenshot_toast);
}

static void
show_delayed_screenshot_toast (KasasaContentContainer *self,
                               guint                   interval)
{
  g_autofree gchar *title = NULL;

  dismiss_delayed_screenshot_toast (self);
  title = g_strdup_printf (ngettext ("Screenshot in %u second",
                                     "Screenshot in %u seconds",
                                     interval),
                           interval);
  self->delayed_screenshot_toast = adw_toast_new (title);
  adw_toast_set_timeout (self->delayed_screenshot_toast, 0);
  adw_toast_set_priority (self->delayed_screenshot_toast,
                          ADW_TOAST_PRIORITY_HIGH);
  adw_toast_set_button_label (self->delayed_screenshot_toast, _("Cancel"));
  adw_toast_set_action_name (self->delayed_screenshot_toast,
                             "app.cancel-delayed-screenshot");
  adw_toast_overlay_add_toast (self->toast_overlay,
                               g_object_ref (self->delayed_screenshot_toast));
}

static GtkWindow *
get_root_window (KasasaContentContainer *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  return GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
}

static gboolean
host_is_miniaturized (KasasaContentContainer *self)
{
  return self->host_ops.is_miniaturized != NULL
         && self->host_ops.is_miniaturized (self->host_data);
}

static void
host_hide_window (KasasaContentContainer *self,
                  gboolean                hide,
                  HideWindowCallback      callback,
                  GObject                *callback_data)
{
  if (self->host_ops.hide_window != NULL)
    self->host_ops.hide_window (self->host_data,
                                hide,
                                callback,
                                callback_data);
}

static void
host_change_opacity (KasasaContentContainer *self,
                     Opacity                direction)
{
  if (self->host_ops.change_opacity != NULL)
    self->host_ops.change_opacity (self->host_data, direction);
}

static void
host_reset_zoom (KasasaContentContainer *self)
{
  if (self->host_ops.reset_zoom != NULL)
    self->host_ops.reset_zoom (self->host_data);
}

static void
host_auto_discard_window (KasasaContentContainer *self)
{
  if (self->host_ops.auto_discard_window != NULL)
    self->host_ops.auto_discard_window (self->host_data);
}

static void
host_miniaturize_window (KasasaContentContainer *self,
                         gboolean                miniaturize)
{
  if (self->host_ops.miniaturize_window != NULL)
    self->host_ops.miniaturize_window (self->host_data, miniaturize);
}

static void
host_block_miniaturization (KasasaContentContainer *self,
                            gboolean                block)
{
  if (self->host_ops.block_miniaturization != NULL)
    self->host_ops.block_miniaturization (self->host_data, block);
}

static void
host_set_controls_popup_active (KasasaContentContainer *self,
                                gboolean                active)
{
  if (self->host_ops.set_controls_popup_active != NULL)
    self->host_ops.set_controls_popup_active (self->host_data, active);
}

static void
host_set_crop_mode (KasasaContentContainer *self,
                    gboolean                active)
{
  if (self->host_ops.set_crop_mode != NULL)
    self->host_ops.set_crop_mode (self->host_data, active);
}

static void
host_finish_initial_reveal (KasasaContentContainer *self)
{
  if (self->host_ops.finish_initial_reveal != NULL)
    self->host_ops.finish_initial_reveal (self->host_data);
}

static gboolean
host_is_initial_reveal_pending (KasasaContentContainer *self)
{
  return self->host_ops.is_initial_reveal_pending != NULL
         && self->host_ops.is_initial_reveal_pending (self->host_data);
}

static GtkWindow *
capture_controller_get_window (gpointer user_data)
{
  return get_root_window (KASASA_CONTENT_CONTAINER (user_data));
}

static void
capture_controller_hide_window (gpointer            user_data,
                                gboolean            hide,
                                HideWindowCallback  callback,
                                GObject            *callback_data)
{
  host_hide_window (KASASA_CONTENT_CONTAINER (user_data),
                    hide,
                    callback,
                    callback_data);
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
  GtkWidget *content = NULL;
  gint new_height, new_width;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  content = get_current_content (self);
  if (!KASASA_IS_CONTENT (content) || self->host_ops.resize == NULL)
    return FALSE;

  kasasa_content_get_dimensions (KASASA_CONTENT (content),
                                 &new_height,
                                 &new_width);

  return self->host_ops.resize (
    self->host_data,
    (gdouble) new_height,
    (gdouble) new_width,
    KASASA_SWITCH_RESIZE_FIT,
    for_zoom,
    continuous);
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
  gboolean crop_available;
  gboolean crop_mode;

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
   * explain why a second concurrent capture is not started. */
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
  crop_available =
    KASASA_IS_SCREENCAST (current_content)
    && kasasa_screencast_is_crop_available (KASASA_SCREENCAST (current_content));
  crop_mode =
    KASASA_IS_SCREENCAST (current_content)
    && kasasa_screencast_is_cropping (KASASA_SCREENCAST (current_content));
  gtk_widget_set_sensitive (GTK_WIDGET (self->retake_screenshot_button),
                            is_screenshot && !crop_mode);
  gtk_widget_set_sensitive (GTK_WIDGET (self->copy_screenshot_button),
                            is_screenshot && !crop_mode);
  gtk_widget_set_visible (GTK_WIDGET (self->stop_screencast_button),
                          is_active_screencast && !crop_mode);
  gtk_widget_set_sensitive (GTK_WIDGET (self->stop_screencast_button),
                            is_active_screencast && !crop_mode);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_screencast_button),
                          crop_available && !crop_mode);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_reset_button), crop_mode);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_cancel_button), crop_mode);
  gtk_widget_set_visible (GTK_WIDGET (self->crop_confirm_button), crop_mode);
  gtk_widget_set_halign (GTK_WIDGET (self->revealer_end_buttons),
                         crop_mode ? GTK_ALIGN_CENTER : GTK_ALIGN_END);

  gtk_widget_set_sensitive (GTK_WIDGET (self->add_screenshot_button),
                            !crop_mode
                            && adw_carousel_get_n_pages (self->carousel)
                                 < MAX_N_CONTENTS);
  gtk_widget_set_sensitive (GTK_WIDGET (self->add_screencast_button),
                            !crop_mode
                            && adw_carousel_get_n_pages (self->carousel)
                                 < MAX_N_CONTENTS);
  gtk_widget_set_sensitive (GTK_WIDGET (self->more_actions_button),
                            !crop_mode
                            && adw_carousel_get_n_pages (self->carousel)
                                 < MAX_N_CONTENTS);
  gtk_widget_set_sensitive (GTK_WIDGET (self->remove_content_button),
                            !crop_mode
                            && adw_carousel_get_n_pages (self->carousel) > 1);
}

static gboolean
append_screenshot (KasasaContentContainer *self,
                   const gchar            *uri,
                   KasasaScreenshotSource  source,
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
  kasasa_screenshot_set_source (new_screenshot, source);
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

// Load the screenshot to the GtkPicture widget
gboolean
kasasa_content_container_append_screenshot (KasasaContentContainer *self,
                                            const gchar            *uri,
                                            GError                **error)
{
  return append_screenshot (self,
                            uri,
                            KASASA_SCREENSHOT_SOURCE_REGION,
                            error);
}

void
kasasa_content_container_set_native_capture_ops (
  KasasaContentContainer       *self,
  const KasasaNativeCaptureOps *ops)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (!kasasa_capture_controller_is_native_pending (
                      self->capture_controller));
  g_return_if_fail (ops == NULL
                    || (ops->screenshot_available != NULL
                        && ops->screencast_available != NULL
                        && ops->present_picker != NULL
                        && ops->capture_screenshot_async != NULL
                        && ops->capture_screenshot_finish != NULL
                        && ops->window_handle_from_address != NULL));

  kasasa_capture_controller_set_native_ops (
    self->capture_controller,
    ops != NULL ? ops : &default_native_capture_ops);
}

void
kasasa_content_container_set_region_capture_ops (
  KasasaContentContainer       *self,
  const KasasaRegionCaptureOps *ops)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (!kasasa_capture_controller_is_region_pending (
                      self->capture_controller));
  g_return_if_fail (ops == NULL
                    || (ops->available != NULL
                        && ops->capture_async != NULL
                        && ops->capture_finish != NULL));

  kasasa_capture_controller_set_region_ops (
    self->capture_controller,
    ops != NULL ? ops : &default_region_capture_ops);
}

void
kasasa_content_container_set_host (
  KasasaContentContainer *self,
  const KasasaContentHostOps *ops,
  gpointer                user_data,
  GDestroyNotify          destroy)
{
  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));
  g_return_if_fail (ops != NULL || user_data == NULL);

  if (self->host_data_destroy != NULL)
    self->host_data_destroy (self->host_data);

  self->host_ops = ops != NULL ? *ops : (KasasaContentHostOps) { 0 };
  self->host_data = ops != NULL ? user_data : NULL;
  self->host_data_destroy = ops != NULL ? destroy : NULL;
}

static void
handle_taken_screenshot (KasasaContentContainer *self,
                         const gchar            *uri,
                         GError                 *capture_error,
                         gboolean                retaking_screenshot,
                         KasasaScreenshotSource  source)
{
  GtkWindow *window = NULL;
  g_autoptr (GError) error = NULL;

  window = get_root_window (self);
  if (window == NULL)
    return;

  host_hide_window (self, FALSE, NULL, NULL);

  if (request_was_cancelled (capture_error)
      || (capture_error == NULL && uri == NULL))
    return;

  if (capture_error != NULL)
    {
      AdwToast *toast =  adw_toast_new_format (_("Error: %s"), capture_error->message);
      adw_toast_set_action_target_value (toast, g_variant_new_string (capture_error->message));
      adw_toast_set_button_label (toast, _("Copy"));
      adw_toast_set_action_name (toast, "toast.copy_error");
      adw_toast_overlay_add_toast (self->toast_overlay, toast);
      g_warning ("%s", capture_error->message);
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

      kasasa_screenshot_set_source (screenshot, source);

      kasasa_content_container_request_window_resize (self);
    }
  else
    {
      // Add new screenshot
      if (!append_screenshot (self, uri, source, &error))
        {
          AdwToast *toast = adw_toast_new_format (_("Error: %s"), error->message);
          adw_toast_overlay_add_toast (self->toast_overlay, toast);
          g_warning ("Couldn't load screenshot: %s", error->message);
          return;
        }
    }

  // Set the focus to the retake_screenshot_button
  gtk_window_set_focus (window, GTK_WIDGET (self->retake_screenshot_button));
}

static void
fail_first_screenshot (GtkWindow    *window,
                       const GError *error)
{
  g_autoptr (GNotification) notification = NULL;
  g_autoptr (GIcon) icon = NULL;
  const gchar *detail = error != NULL
                        ? error->message
                        : _("Couldn't capture the screenshot");

  g_warning ("First screenshot failed: %s", detail);
  icon = g_themed_icon_new ("dialog-warning-symbolic");
  notification = g_notification_new (_("Screenshot failed"));
  g_notification_set_icon (notification, icon);
  g_notification_set_body (notification, detail);
  g_application_send_notification (g_application_get_default (),
                                   "io.github.kelvinnovais.Kasasa",
                                   notification);
  gtk_window_close (window);
}

static void
on_capture_finished (gpointer                user_data,
                     KasasaNativeCaptureKind kind)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  if (get_root_window (self) != NULL)
    host_block_miniaturization (self, FALSE);
  if (kind == KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT)
    kasasa_content_container_carousel_set_interactive (self, TRUE);
  if (kind == KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT)
    {
      withdraw_delayed_screenshot_notification ();
      dismiss_delayed_screenshot_toast (self);
    }
  kasasa_content_container_update_toolbar_sensibility (self);
}

static void
on_region_capture_finished (gpointer                 user_data,
                            KasasaRegionCaptureKind  kind)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  if (get_root_window (self) != NULL)
    host_block_miniaturization (self, FALSE);
  if (kind == KASASA_REGION_CAPTURE_RETAKE)
    kasasa_content_container_carousel_set_interactive (self, TRUE);
  kasasa_content_container_update_toolbar_sensibility (self);
}

static void
on_capture_cancelled (gpointer                user_data,
                      KasasaNativeCaptureKind kind,
                      gboolean                 first_capture)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWindow *window = get_root_window (self);

  if (first_capture && window != NULL)
    gtk_window_close (window);
}

static void
on_region_capture_cancelled (gpointer                 user_data,
                             KasasaRegionCaptureKind  kind,
                             gboolean                 first_capture)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWindow *window = get_root_window (self);

  if (first_capture && window != NULL)
    gtk_window_close (window);
}

static void
on_delayed_screenshot_scheduled (gpointer user_data,
                                 guint    interval)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  send_delayed_screenshot_notification (interval);
  show_delayed_screenshot_toast (self, interval);
}

static void
on_delayed_screenshot_started (gpointer user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  withdraw_delayed_screenshot_notification ();
  if (self->delayed_screenshot_toast != NULL)
    adw_toast_set_title (self->delayed_screenshot_toast,
                         _ ("Capturing window…"));
}

static void
on_native_screenshot_captured (gpointer                user_data,
                               KasasaNativeCaptureKind kind,
                               const gchar             *uri,
                               const GError            *error)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWindow *window = get_root_window (self);

  if (window == NULL)
    return;

  if (kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENSHOT)
    {
      if (uri == NULL)
        fail_first_screenshot (window, error);
      else
        load_first_screenshot_uri (self,
                                   uri,
                                   KASASA_SCREENSHOT_SOURCE_WINDOW);
      return;
    }

  handle_taken_screenshot (self,
                           uri,
                           (GError *) error,
                           kind == KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT,
                           KASASA_SCREENSHOT_SOURCE_WINDOW);
}

static void
on_region_screenshot_captured (gpointer                 user_data,
                               KasasaRegionCaptureKind  kind,
                               const gchar             *uri,
                               const GError            *error)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWindow *window = get_root_window (self);

  if (window == NULL)
    return;

  if (kind == KASASA_REGION_CAPTURE_FIRST)
    {
      if (uri == NULL)
        {
          if (!request_was_cancelled (error))
            fail_first_screenshot (window, error);
          else
            gtk_window_close (window);
        }
      else
        {
          host_hide_window (self, FALSE, NULL, NULL);
          load_first_screenshot_uri (self,
                                     uri,
                                     KASASA_SCREENSHOT_SOURCE_REGION);
        }
      return;
    }

  handle_taken_screenshot (self,
                           uri,
                           (GError *) error,
                           kind == KASASA_REGION_CAPTURE_RETAKE,
                           KASASA_SCREENSHOT_SOURCE_REGION);
}

static void
on_native_screencast_selected (gpointer                user_data,
                               KasasaNativeCaptureKind kind,
                               guint32                  handle,
                               gint                     width,
                               gint                     height,
                               const GError            *error)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWindow *window = get_root_window (self);
  g_autoptr (GError) append_error = NULL;

  if (window == NULL)
    return;

  if (error != NULL)
    {
      if (kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST)
        fail_first_screencast (self,
                               window,
                               _ ("Invalid Hyprland window address"),
                               error);
      else
        show_operation_error (self, _ ("Invalid Hyprland window address"), error);
      return;
    }

  if (kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST)
    {
      kasasa_content_container_load_first_hyprland_screencast (self,
                                                               handle,
                                                               width,
                                                               height);
      return;
    }

  if (!append_hyprland_screencast (self,
                                   handle,
                                   width,
                                   height,
                                   &append_error))
    show_operation_error (self, _ ("Couldn't display the screencast"), append_error);
}

static void
begin_native_capture_request (KasasaContentContainer *self,
                              KasasaNativeCaptureKind kind)
{
  g_autoptr (GError) error = NULL;
  GtkWindow *window;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  window = get_root_window (self);
  if (window == NULL
      || kasasa_capture_controller_is_native_pending (self->capture_controller)
      || kasasa_capture_controller_is_region_pending (self->capture_controller)
      || kasasa_capture_controller_is_delayed_pending (self->capture_controller))
    return;

  if (kind != KASASA_NATIVE_CAPTURE_FIRST_SCREENSHOT
      && kind != KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST
      && kind != KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT
      && adw_carousel_get_n_pages (self->carousel) >= MAX_N_CONTENTS)
    {
      show_operation_error (self, _("Maximum number of items reached"), NULL);
      return;
    }

  if ((kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST
       || kind == KASASA_NATIVE_CAPTURE_ADD_SCREENCAST)
      && has_active_screencast (self))
    {
      show_operation_error (
        self, _("Finish the current screencast before starting another one."), NULL);
      return;
    }

  gtk_popover_popdown (self->more_actions_popover);
  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);
  host_block_miniaturization (self, TRUE);
  if (kind == KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT)
    kasasa_content_container_carousel_set_interactive (self, FALSE);

  kasasa_capture_controller_begin_native (self->capture_controller,
                                           kind,
                                           &error);
  if (error == NULL)
    return;

  on_capture_finished (self, kind);

  if (kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST
      || kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENSHOT)
    {
      if (kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST)
        fail_first_screencast (self,
                               window,
                               _("Couldn't open the window selector"),
                               error);
      else
        fail_first_screenshot (window, error);
    }
  else
    show_operation_error (self, _("Couldn't open the window selector"), error);
}

static void
begin_region_capture_request (KasasaContentContainer *self,
                              KasasaRegionCaptureKind kind)
{
  g_autoptr (GError) error = NULL;
  GtkWindow *window;

  g_return_if_fail (KASASA_IS_CONTENT_CONTAINER (self));

  window = get_root_window (self);
  if (window == NULL
      || kasasa_capture_controller_is_region_pending (self->capture_controller)
      || kasasa_capture_controller_is_native_pending (self->capture_controller)
      || kasasa_capture_controller_is_delayed_pending (self->capture_controller))
    return;

  if (kind == KASASA_REGION_CAPTURE_RETAKE)
    {
      GtkWidget *content = get_current_content (self);

      if (!KASASA_IS_SCREENSHOT (content))
        return;
    }
  else if (kind != KASASA_REGION_CAPTURE_FIRST
           && adw_carousel_get_n_pages (self->carousel) >= MAX_N_CONTENTS)
    {
      show_operation_error (self, _("Maximum number of items reached"), NULL);
      return;
    }

  gtk_popover_popdown (self->more_actions_popover);
  gtk_widget_set_sensitive (GTK_WIDGET (self->toolbar_overlay), FALSE);
  host_block_miniaturization (self, TRUE);
  if (kind == KASASA_REGION_CAPTURE_RETAKE)
    kasasa_content_container_carousel_set_interactive (self, FALSE);

  kasasa_capture_controller_begin_region (self->capture_controller,
                                           kind,
                                           &error);
  if (error == NULL)
    return;

  on_region_capture_finished (self, kind);
  if (kind == KASASA_REGION_CAPTURE_FIRST)
    fail_first_screenshot (window, error);
  else
    show_operation_error (self, _ ("Couldn't start region capture"), error);
}

static void
select_region_screenshot (GtkButton *button,
                          gpointer   user_data)
{
  begin_region_capture_request (KASASA_CONTENT_CONTAINER (user_data),
                                KASASA_REGION_CAPTURE_ADD);
}

static void
select_native_screenshot (GtkButton *button,
                          gpointer   user_data)
{
  begin_native_capture_request (KASASA_CONTENT_CONTAINER (user_data),
                                KASASA_NATIVE_CAPTURE_ADD_SCREENSHOT);
}

static void
select_native_delayed_screenshot (GtkButton *button,
                                  gpointer   user_data)
{
  begin_native_capture_request (KASASA_CONTENT_CONTAINER (user_data),
                                KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT);
}

static void
select_native_retake_screenshot (GtkButton *button,
                                 gpointer   user_data)
{
  begin_native_capture_request (KASASA_CONTENT_CONTAINER (user_data),
                                KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT);
}

static void
retake_screenshot (GtkButton *button,
                   gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWidget *content = get_current_content (self);

  if (!KASASA_IS_SCREENSHOT (content))
    return;

  if (kasasa_screenshot_get_source (KASASA_SCREENSHOT (content))
      == KASASA_SCREENSHOT_SOURCE_WINDOW)
    select_native_retake_screenshot (button, user_data);
  else
    begin_region_capture_request (self, KASASA_REGION_CAPTURE_RETAKE);
}

static void
select_native_screencast (GtkButton *button,
                          gpointer   user_data)
{
  begin_native_capture_request (KASASA_CONTENT_CONTAINER (user_data),
                                KASASA_NATIVE_CAPTURE_ADD_SCREENCAST);
}

static void
on_screencast_new_dimension (KasasaScreencast *screencast,
                             gint              new_width,
                             gint              new_height,
                             gpointer          user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWidget *current_content = get_current_content (self);
  gboolean miniaturized = host_is_miniaturized (self);


  if (current_content == GTK_WIDGET (screencast)
      && !miniaturized && self->host_ops.resize != NULL)
    self->host_ops.resize (self->host_data,
                           (gdouble) new_height,
                           (gdouble) new_width,
                           KASASA_SWITCH_RESIZE_FIT,
                           FALSE,
                           FALSE);
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
  GtkWidget *current_content = get_current_content (self);
  gboolean miniaturized = host_is_miniaturized (self);
  guint n_pages = adw_carousel_get_n_pages (self->carousel);
  guint screencast_idx = find_content_index (self, GTK_WIDGET (screencast));

  if (current_content == GTK_WIDGET (screencast)
      && kasasa_screencast_is_cropping (screencast))
    {
      host_set_crop_mode (self, FALSE);
      kasasa_content_container_carousel_set_interactive (self, TRUE);
    }

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

      host_miniaturize_window (self, FALSE);

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
            host_miniaturize_window (self, TRUE);
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

static KasasaScreencast *
get_current_window_screencast (KasasaContentContainer *self)
{
  GtkWidget *content = get_current_content (self);

  if (!KASASA_IS_SCREENCAST (content))
    return NULL;

  return KASASA_SCREENCAST (content);
}

static void
on_crop_screencast_clicked (GtkButton *button,
                            gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaScreencast *screencast = get_current_window_screencast (self);

  if (screencast != NULL
      && kasasa_screencast_begin_crop (screencast))
    {
      kasasa_content_container_carousel_set_interactive (self, FALSE);
      host_set_crop_mode (self, TRUE);
      kasasa_content_container_update_toolbar_sensibility (self);
      kasasa_content_container_request_window_resize (self);
    }
}

static void
on_crop_reset_clicked (GtkButton *button,
                       gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaScreencast *screencast = get_current_window_screencast (self);

  if (screencast != NULL)
    kasasa_screencast_reset_crop (screencast);
}

static void
on_crop_cancel_clicked (GtkButton *button,
                        gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaScreencast *screencast = get_current_window_screencast (self);

  if (screencast != NULL)
    {
      kasasa_screencast_cancel_crop (screencast);
      kasasa_content_container_carousel_set_interactive (self, TRUE);
      host_set_crop_mode (self, FALSE);
      kasasa_content_container_update_toolbar_sensibility (self);
      kasasa_content_container_request_window_resize (self);
    }
}

static void
on_crop_confirm_clicked (GtkButton *button,
                         gpointer   user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  KasasaScreencast *screencast = get_current_window_screencast (self);

  if (screencast != NULL
      && kasasa_screencast_confirm_crop (screencast))
    {
      kasasa_content_container_carousel_set_interactive (self, TRUE);
      host_set_crop_mode (self, FALSE);
      kasasa_content_container_update_toolbar_sensibility (self);
      kasasa_content_container_request_window_resize (self);
    }
}

static void
fail_first_screencast (KasasaContentContainer *self,
                       GtkWindow              *window,
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

  gtk_window_close (window);
}


void
kasasa_content_container_request_first_screenshot (KasasaContentContainer *self)
{
  begin_region_capture_request (self, KASASA_REGION_CAPTURE_FIRST);
}

static void
load_first_screenshot_uri (KasasaContentContainer *self,
                           const gchar            *uri,
                           KasasaScreenshotSource  source)
{
  GtkWindow *window = NULL;
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

  host_reset_zoom (self);

  if (!append_screenshot (self, uri, source, &error))
    {
      g_warning ("Couldn't load first screenshot: %s", error->message);
      error_message = g_strconcat (_("Reason: "), error->message, NULL);
      goto FAIL;
    }

  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  host_finish_initial_reveal (self);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    host_auto_discard_window (self);

  host_miniaturize_window (self, TRUE);
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
  gtk_window_close (window);
}

void
kasasa_content_container_load_first_screenshot_uri (KasasaContentContainer *self,
                                                    const gchar            *uri)
{
  load_first_screenshot_uri (self,
                             uri,
                             KASASA_SCREENSHOT_SOURCE_WINDOW);
}

void
kasasa_content_container_request_first_hyprland_screencast (
  KasasaContentContainer *self)
{
  begin_native_capture_request (self,
                                KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST);
}

void
kasasa_content_container_request_hyprland_screencast (
  KasasaContentContainer *self)
{
  begin_native_capture_request (self,
                                KASASA_NATIVE_CAPTURE_ADD_SCREENCAST);
}

static gboolean
append_hyprland_screencast (KasasaContentContainer *self,
                            guint32                 window_handle,
                            gint                    width,
                            gint                    height,
                            GError                **error)
{
  KasasaScreencast *screencast;

  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  screencast = kasasa_screencast_new ();
  g_signal_connect (screencast, "new-dimension",
                    G_CALLBACK (on_screencast_new_dimension), self);
  g_signal_connect (screencast, "eos",
                    G_CALLBACK (on_screencast_eos), self);
  g_signal_connect (screencast, "dmabuf-fallback",
                    G_CALLBACK (on_screencast_dmabuf_fallback), self);

  if (!kasasa_screencast_show_hyprland (screencast,
                                        window_handle,
                                        width,
                                        height,
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
  GtkWindow *window;
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
      gtk_window_close (window);
      return;
    }

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  host_reset_zoom (self);
  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  host_finish_initial_reveal (self);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    host_auto_discard_window (self);

  host_miniaturize_window (self, TRUE);
}

void
kasasa_content_container_load_first_hyprland_screencast (KasasaContentContainer *self,
                                                         guint32                 window_handle,
                                                         gint                    width,
                                                         gint                    height)
{
  GtkWindow *window = NULL;
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
  if (!append_hyprland_screencast (self,
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
      goto FAIL;
    }

  host_reset_zoom (self);
  gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
  kasasa_content_container_request_window_resize (self);
  host_finish_initial_reveal (self);

  if (g_settings_get_boolean (settings, "auto-discard-window"))
    host_auto_discard_window (self);

  host_miniaturize_window (self, TRUE);
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
  gtk_window_close (window);
}

gboolean
kasasa_content_container_cancel_delayed_screenshot (KasasaContentContainer *self)
{
  g_return_val_if_fail (KASASA_IS_CONTENT_CONTAINER (self), FALSE);

  if (!kasasa_capture_controller_is_delayed_pending (self->capture_controller))
    return FALSE;

  withdraw_delayed_screenshot_notification ();
  if (self->delayed_screenshot_toast != NULL)
    adw_toast_set_title (self->delayed_screenshot_toast,
                         _("Cancelling capture…"));
  return kasasa_capture_controller_cancel_delayed (self->capture_controller);
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

  host_change_opacity (self, OPACITY_INCREASE);
}

static void
on_mouse_leave_controls (GtkEventControllerMotion *event_controller_motion,
                        gdouble                   x,
                        gdouble                   y,
                        gpointer                  user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);

  host_change_opacity (self, OPACITY_DECREASE);
}

static void
on_page_changed (AdwCarousel *carousel,
                 guint        index,
                 gpointer     user_data)
{
  KasasaContentContainer *self = KASASA_CONTENT_CONTAINER (user_data);
  GtkWidget *content = NULL;
  guint n_pages = adw_carousel_get_n_pages (carousel);
  gboolean initial_reveal;
  gint new_height, new_width;

  g_debug ("Page changed");
  // If the carousel is empty, return
  if (index == GTK_INVALID_LIST_POSITION || index >= n_pages)
    return;

  initial_reveal = host_is_initial_reveal_pending (self)
                   && self->current_page_index == GTK_INVALID_LIST_POSITION
                   && n_pages == 1;
  self->current_page_index = index;

  // Ensure that the window is visible
  host_change_opacity (self, OPACITY_INCREASE);

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

  if (self->host_ops.resize != NULL)
    self->host_ops.resize (
      self->host_data,
      (gdouble) new_height,
      (gdouble) new_width,
      (KasasaSwitchResizeMode) g_settings_get_uint (
        self->settings, "image-switch-resize-mode"),
      FALSE,
      FALSE);
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
  gboolean active;

  active = gtk_menu_button_get_active (self->more_actions_button);
  host_set_controls_popup_active (self, active);
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
  dismiss_delayed_screenshot_toast (self);
  g_clear_object (&self->capture_controller);
  g_clear_object (&self->settings);
  kasasa_content_container_set_host (self, NULL, NULL, NULL);

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

  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, carousel);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, retake_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_window_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_delayed_screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, add_hyprland_monitor_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, remove_content_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, stop_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, crop_screencast_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, crop_reset_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, crop_cancel_button);
  gtk_widget_class_bind_template_child (widget_class, KasasaContentContainer, crop_confirm_button);
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
  static const KasasaCaptureControllerCallbacks capture_callbacks = {
    .get_window = capture_controller_get_window,
    .hide_window = capture_controller_hide_window,
    .native_screenshot_captured = on_native_screenshot_captured,
    .region_screenshot_captured = on_region_screenshot_captured,
    .native_screencast_selected = on_native_screencast_selected,
    .capture_finished = on_capture_finished,
    .region_capture_finished = on_region_capture_finished,
    .capture_cancelled = on_capture_cancelled,
    .region_capture_cancelled = on_region_capture_cancelled,
    .delayed_screenshot_scheduled = on_delayed_screenshot_scheduled,
    .delayed_screenshot_started = on_delayed_screenshot_started,
  };

  gtk_widget_init_template (GTK_WIDGET (self));

  self->settings = g_settings_new ("io.github.kelvinnovais.Kasasa");
  self->capture_controller = kasasa_capture_controller_new (
    G_OBJECT (self), self->settings, &capture_callbacks, self);
  kasasa_capture_controller_set_native_ops (self->capture_controller,
                                            &default_native_capture_ops);
  kasasa_capture_controller_set_region_ops (self->capture_controller,
                                            &default_region_capture_ops);
  self->delayed_screenshot_toast = NULL;
  self->host_ops = (KasasaContentHostOps) { 0 };
  self->host_data = NULL;
  self->host_data_destroy = NULL;
  self->carousel_interaction_locks = 0;
  self->current_page_index = GTK_INVALID_LIST_POSITION;

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
                    G_CALLBACK (select_region_screenshot),
                    self);
  g_signal_connect (self->add_window_screenshot_button,
                    "clicked",
                    G_CALLBACK (select_native_screenshot),
                    self);
  g_signal_connect (self->add_delayed_screenshot_button,
                    "clicked",
                    G_CALLBACK (select_native_delayed_screenshot),
                    self);
  g_signal_connect (self->add_screencast_button,
                    "clicked",
                    G_CALLBACK (select_native_screencast),
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
  g_signal_connect (self->crop_screencast_button,
                    "clicked",
                    G_CALLBACK (on_crop_screencast_clicked),
                    self);
  g_signal_connect (self->crop_reset_button,
                    "clicked",
                    G_CALLBACK (on_crop_reset_clicked),
                    self);
  g_signal_connect (self->crop_cancel_button,
                    "clicked",
                    G_CALLBACK (on_crop_cancel_clicked),
                    self);
  g_signal_connect (self->crop_confirm_button,
                    "clicked",
                    G_CALLBACK (on_crop_confirm_clicked),
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
