/* kasasa-capture-controller.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib/gi18n.h>

#include "kasasa-capture-controller.h"

#include "kasasa-source.h"
#include "kasasa-window-query.h"

struct _KasasaCaptureController
{
  GObject parent_instance;

  GWeakRef owner;
  GSettings *settings;
  KasasaCaptureControllerCallbacks callbacks;
  gpointer user_data;
  KasasaNativeCaptureOps native_ops;
  KasasaRegionCaptureOps region_ops;

  GCancellable *native_cancellable;
  GCancellable *region_cancellable;
  KasasaSource delayed_source;
  KasasaWindowClient *delayed_client;
  KasasaNativeCaptureKind native_kind;
  KasasaRegionCaptureKind region_kind;
  gboolean native_pending;
  gboolean region_pending;
  gboolean delayed_pending;
};

typedef struct
{
  KasasaCaptureController *controller;
  KasasaNativeCaptureKind kind;
} NativeCaptureRequest;

typedef struct
{
  KasasaCaptureController *controller;
  KasasaNativeCaptureKind kind;
  gchar *(*finish) (GAsyncResult *result,
                    GError      **error);
} NativeScreenshotRequest;

typedef struct
{
  KasasaCaptureController *controller;
  KasasaRegionCaptureKind kind;
  gchar *(*finish) (GAsyncResult *result,
                    GError      **error);
} RegionCaptureRequest;

G_DEFINE_FINAL_TYPE (KasasaCaptureController,
                     kasasa_capture_controller,
                     G_TYPE_OBJECT)

static gboolean
native_capture_is_first (KasasaNativeCaptureKind kind)
{
  return kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENSHOT
         || kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST;
}

static gboolean
native_capture_is_screencast (KasasaNativeCaptureKind kind)
{
  return kind == KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST
         || kind == KASASA_NATIVE_CAPTURE_ADD_SCREENCAST;
}

static GtkWindow *
get_window (KasasaCaptureController *self)
{
  GObject *owner = g_weak_ref_get (&self->owner);
  GtkWindow *window = NULL;

  if (owner != NULL)
    {
      if (self->callbacks.get_window != NULL)
        window = self->callbacks.get_window (self->user_data);
      g_object_unref (owner);
    }

  return window;
}

static GObject *
get_owner (KasasaCaptureController *self)
{
  return g_weak_ref_get (&self->owner);
}

static void
finish_native (KasasaCaptureController *self)
{
  KasasaNativeCaptureKind kind = self->native_kind;
  g_autoptr (GObject) owner = get_owner (self);

  self->native_pending = FALSE;
  g_clear_object (&self->native_cancellable);
  if (kind == KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT)
    self->delayed_pending = FALSE;

  if (owner != NULL && self->callbacks.capture_finished != NULL)
    self->callbacks.capture_finished (self->user_data, kind);
}

static void
finish_region (KasasaCaptureController *self)
{
  KasasaRegionCaptureKind kind = self->region_kind;
  g_autoptr (GObject) owner = get_owner (self);

  self->region_pending = FALSE;
  g_clear_object (&self->region_cancellable);

  if (owner != NULL && self->callbacks.region_capture_finished != NULL)
    self->callbacks.region_capture_finished (self->user_data, kind);
}

static NativeCaptureRequest *
native_capture_request_new (KasasaCaptureController *self,
                            KasasaNativeCaptureKind  kind)
{
  NativeCaptureRequest *request = g_new0 (NativeCaptureRequest, 1);

  request->controller = g_object_ref (self);
  request->kind = kind;
  return request;
}

static void
native_capture_request_free (NativeCaptureRequest *request)
{
  g_clear_object (&request->controller);
  g_free (request);
}

static NativeScreenshotRequest *
native_screenshot_request_new (KasasaCaptureController *self,
                               KasasaNativeCaptureKind  kind)
{
  NativeScreenshotRequest *request = g_new0 (NativeScreenshotRequest, 1);

  request->controller = g_object_ref (self);
  request->kind = kind;
  request->finish = self->native_ops.capture_screenshot_finish;
  return request;
}

static void
native_screenshot_request_free (NativeScreenshotRequest *request)
{
  g_clear_object (&request->controller);
  g_free (request);
}

static RegionCaptureRequest *
region_capture_request_new (KasasaCaptureController *self,
                            KasasaRegionCaptureKind  kind)
{
  RegionCaptureRequest *request = g_new0 (RegionCaptureRequest, 1);

  request->controller = g_object_ref (self);
  request->kind = kind;
  request->finish = self->region_ops.capture_finish;
  return request;
}

static void
region_capture_request_free (RegionCaptureRequest *request)
{
  g_clear_object (&request->controller);
  g_free (request);
}

static void
on_native_screenshot_captured (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  NativeScreenshotRequest *request = user_data;
  KasasaCaptureController *self = request->controller;
  g_autoptr (GObject) owner = get_owner (self);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uri = NULL;

  uri = request->finish (result, &error);
  if (owner != NULL && self->callbacks.native_screenshot_captured != NULL)
    self->callbacks.native_screenshot_captured (self->user_data,
                                                request->kind,
                                                uri,
                                                error);
  finish_native (self);
  native_screenshot_request_free (request);
}

static void
on_region_screenshot_captured (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  RegionCaptureRequest *request = user_data;
  KasasaCaptureController *self = request->controller;
  g_autoptr (GObject) owner = get_owner (self);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *uri = NULL;

  uri = request->finish (result, &error);
  if (owner != NULL && self->callbacks.region_screenshot_captured != NULL)
    self->callbacks.region_screenshot_captured (self->user_data,
                                                request->kind,
                                                uri,
                                                error);
  finish_region (self);
  region_capture_request_free (request);
}

static void
capture_selected_screenshot (KasasaCaptureController *self,
                             const KasasaWindowClient *client,
                             KasasaNativeCaptureKind  kind)
{
  NativeScreenshotRequest *request;

  g_clear_object (&self->native_cancellable);
  self->native_cancellable = g_cancellable_new ();
  request = native_screenshot_request_new (self, kind);
  self->native_ops.capture_screenshot_async (client,
                                             self->native_cancellable,
                                             on_native_screenshot_captured,
                                             request);
}

static void
capture_delayed_screenshot_cb (gpointer user_data)
{
  KasasaCaptureController *self = KASASA_CAPTURE_CONTROLLER (user_data);
  g_autoptr (GObject) owner = get_owner (self);

  if (owner != NULL && self->callbacks.delayed_screenshot_started != NULL)
    self->callbacks.delayed_screenshot_started (self->user_data);

  if (self->delayed_client != NULL)
    capture_selected_screenshot (self,
                                 self->delayed_client,
                                 KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT);
  else
    finish_native (self);

  g_clear_pointer (&self->delayed_client, kasasa_window_client_free);
}

static void
on_region_window_hidden (gpointer user_data)
{
  KasasaCaptureController *self = KASASA_CAPTURE_CONTROLLER (user_data);
  g_autoptr (GObject) owner = get_owner (self);

  if (owner != NULL
      && self->region_pending
      && self->region_ops.capture_async != NULL)
    self->region_ops.capture_async (self->region_cancellable,
                                    on_region_screenshot_captured,
                                    region_capture_request_new (
                                      self, self->region_kind));

  g_object_unref (self);
}

static void
on_native_window_selected (const KasasaWindowClient *client,
                           gpointer                  user_data)
{
  NativeCaptureRequest *request = user_data;
  KasasaCaptureController *self = request->controller;
  g_autoptr (GObject) owner = get_owner (self);
  GtkWindow *window;

  if (owner == NULL)
    return;

  window = get_window (self);
  if (window == NULL)
    {
      finish_native (self);
      return;
    }

  if (client == NULL)
    {
      gboolean first_capture = native_capture_is_first (request->kind);

      finish_native (self);
      if (self->callbacks.capture_cancelled != NULL)
        self->callbacks.capture_cancelled (self->user_data,
                                           request->kind,
                                           first_capture);
      return;
    }

  if (request->kind == KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT)
    {
      guint interval = g_settings_get_uint (self->settings,
                                            "screenshot-delay");

      g_clear_pointer (&self->delayed_client, kasasa_window_client_free);
      self->delayed_client = kasasa_window_client_copy (client);
      self->delayed_pending = TRUE;
      kasasa_source_set_timeout_seconds_once (&self->delayed_source,
                                              interval,
                                              capture_delayed_screenshot_cb,
                                              self);
      if (self->callbacks.delayed_screenshot_scheduled != NULL)
        self->callbacks.delayed_screenshot_scheduled (self->user_data,
                                                      interval);
      return;
    }

  if (native_capture_is_screencast (request->kind))
    {
      g_autoptr (GError) error = NULL;
      guint32 handle = 0;

      if (!self->native_ops.window_handle_from_address (client->address,
                                                        &handle,
                                                        &error))
        handle = 0;

      if (self->callbacks.native_screencast_selected != NULL)
        self->callbacks.native_screencast_selected (self->user_data,
                                                    request->kind,
                                                    handle,
                                                    client->width,
                                                    client->height,
                                                    error);
      finish_native (self);
      return;
    }

  capture_selected_screenshot (self, client, request->kind);
}

static const gchar *
native_capture_picker_title (KasasaNativeCaptureKind kind)
{
  return native_capture_is_screencast (kind)
         ? _ ("Select a window to preview live")
         : _ ("Select a window to capture");
}

KasasaCaptureController *
kasasa_capture_controller_new (GObject                                *owner,
                                GSettings                              *settings,
                                const KasasaCaptureControllerCallbacks *callbacks,
                                gpointer                                user_data)
{
  KasasaCaptureController *self;

  g_return_val_if_fail (G_IS_OBJECT (owner), NULL);
  g_return_val_if_fail (G_IS_SETTINGS (settings), NULL);
  g_return_val_if_fail (callbacks != NULL, NULL);

  self = g_object_new (KASASA_TYPE_CAPTURE_CONTROLLER, NULL);
  g_weak_ref_set (&self->owner, owner);
  self->settings = g_object_ref (settings);
  self->callbacks = *callbacks;
  self->user_data = user_data;
  return self;
}

void
kasasa_capture_controller_set_native_ops (KasasaCaptureController      *self,
                                           const KasasaNativeCaptureOps *ops)
{
  g_return_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self));
  g_return_if_fail (ops != NULL);
  self->native_ops = *ops;
}

void
kasasa_capture_controller_set_region_ops (KasasaCaptureController      *self,
                                           const KasasaRegionCaptureOps *ops)
{
  g_return_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self));
  g_return_if_fail (ops != NULL);
  self->region_ops = *ops;
}

gboolean
kasasa_capture_controller_is_native_pending (KasasaCaptureController *self)
{
  g_return_val_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self), FALSE);
  return self->native_pending;
}

gboolean
kasasa_capture_controller_is_region_pending (KasasaCaptureController *self)
{
  g_return_val_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self), FALSE);
  return self->region_pending;
}

gboolean
kasasa_capture_controller_is_delayed_pending (KasasaCaptureController *self)
{
  g_return_val_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self), FALSE);
  return self->delayed_pending;
}

void
kasasa_capture_controller_begin_native (KasasaCaptureController *self,
                                         KasasaNativeCaptureKind  kind,
                                         GError                 **error)
{
  GtkWindow *window;
  NativeCaptureRequest *request;

  g_return_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self));
  g_return_if_fail (error == NULL || *error == NULL);

  window = get_window (self);
  if (window == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_CLOSED,
                           "The capture window is no longer available");
      return;
    }

  if (self->native_pending || self->region_pending || self->delayed_pending)
    return;

  if (native_capture_is_screencast (kind)
      && !self->native_ops.screencast_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _ ("Live window preview requires Hyprland Wayland"));
      return;
    }

  if (!native_capture_is_screencast (kind)
      && !self->native_ops.screenshot_available ())
    {
      g_set_error_literal (error,
                           KASASA_WINDOW_QUERY_ERROR,
                           KASASA_WINDOW_QUERY_ERROR_UNAVAILABLE,
                           _ ("Window capture requires Hyprland Wayland"));
      return;
    }

  self->native_pending = TRUE;
  self->native_kind = kind;
  request = native_capture_request_new (self, kind);
  if (self->native_ops.present_picker (window,
                                       native_capture_picker_title (kind),
                                       on_native_window_selected,
                                       request,
                                       (GDestroyNotify) native_capture_request_free,
                                       error))
    return;

  native_capture_request_free (request);
  self->native_pending = FALSE;
  g_clear_object (&self->native_cancellable);
}

void
kasasa_capture_controller_begin_region (KasasaCaptureController *self,
                                         KasasaRegionCaptureKind  kind,
                                         GError                  **error)
{
  GtkWindow *window;

  g_return_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self));
  g_return_if_fail (error == NULL || *error == NULL);

  window = get_window (self);
  if (window == NULL)
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_CLOSED,
                           "The capture window is no longer available");
      return;
    }

  if (self->native_pending || self->region_pending || self->delayed_pending)
    return;

  if (!self->region_ops.available ())
    {
      g_set_error_literal (error,
                           G_IO_ERROR,
                           G_IO_ERROR_NOT_FOUND,
                           _ ("Region capture requires slurp and grim"));
      return;
    }

  self->region_pending = TRUE;
  self->region_kind = kind;
  g_clear_object (&self->region_cancellable);
  self->region_cancellable = g_cancellable_new ();

  if (self->callbacks.hide_window != NULL)
    {
      g_object_ref (self);
      self->callbacks.hide_window (self->user_data,
                                   TRUE,
                                   on_region_window_hidden,
                                   G_OBJECT (self));
    }
  else
    on_region_window_hidden (g_object_ref (self));
}

gboolean
kasasa_capture_controller_cancel_delayed (KasasaCaptureController *self)
{
  g_return_val_if_fail (KASASA_IS_CAPTURE_CONTROLLER (self), FALSE);

  if (!self->delayed_pending)
    return FALSE;

  kasasa_source_clear (&self->delayed_source);
  g_clear_pointer (&self->delayed_client, kasasa_window_client_free);
  if (self->native_cancellable != NULL)
    {
      g_cancellable_cancel (self->native_cancellable);
      return TRUE;
    }

  finish_native (self);
  return TRUE;
}

static void
kasasa_capture_controller_dispose (GObject *object)
{
  KasasaCaptureController *self = KASASA_CAPTURE_CONTROLLER (object);

  kasasa_source_clear (&self->delayed_source);
  g_clear_pointer (&self->delayed_client, kasasa_window_client_free);
  g_clear_object (&self->native_cancellable);
  g_clear_object (&self->region_cancellable);
  g_clear_object (&self->settings);
  G_OBJECT_CLASS (kasasa_capture_controller_parent_class)->dispose (object);
}

static void
kasasa_capture_controller_finalize (GObject *object)
{
  KasasaCaptureController *self = KASASA_CAPTURE_CONTROLLER (object);

  g_weak_ref_clear (&self->owner);
  G_OBJECT_CLASS (kasasa_capture_controller_parent_class)->finalize (object);
}

static void
kasasa_capture_controller_class_init (KasasaCaptureControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kasasa_capture_controller_dispose;
  object_class->finalize = kasasa_capture_controller_finalize;
}

static void
kasasa_capture_controller_init (KasasaCaptureController *self)
{
  g_weak_ref_init (&self->owner, NULL);
  self->delayed_source.id = 0;
}
