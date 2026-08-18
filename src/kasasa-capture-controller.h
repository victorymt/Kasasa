/* kasasa-capture-controller.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "kasasa-content-host.h"
#include "kasasa-window-picker.h"

G_BEGIN_DECLS

typedef enum
{
  KASASA_NATIVE_CAPTURE_FIRST_SCREENSHOT,
  KASASA_NATIVE_CAPTURE_ADD_SCREENSHOT,
  KASASA_NATIVE_CAPTURE_DELAYED_SCREENSHOT,
  KASASA_NATIVE_CAPTURE_RETAKE_SCREENSHOT,
  KASASA_NATIVE_CAPTURE_FIRST_SCREENCAST,
  KASASA_NATIVE_CAPTURE_ADD_SCREENCAST,
} KasasaNativeCaptureKind;

typedef enum
{
  KASASA_REGION_CAPTURE_FIRST,
  KASASA_REGION_CAPTURE_ADD,
  KASASA_REGION_CAPTURE_RETAKE,
} KasasaRegionCaptureKind;

typedef struct
{
  gboolean (*screenshot_available) (void);
  gboolean (*screencast_available) (void);
  gboolean (*present_picker) (GtkWindow                  *parent,
                              const gchar                *title,
                              KasasaWindowPickerCallback  callback,
                              gpointer                    user_data,
                              GDestroyNotify              destroy,
                              GError                    **error);
  void (*capture_screenshot_async) (const KasasaWindowClient *client,
                                    GCancellable             *cancellable,
                                    GAsyncReadyCallback       callback,
                                    gpointer                  user_data);
  gchar *(*capture_screenshot_finish) (GAsyncResult *result,
                                       GError      **error);
  gboolean (*window_handle_from_address) (const gchar *address,
                                          guint32     *handle,
                                          GError     **error);
} KasasaNativeCaptureOps;

typedef struct
{
  gboolean (*available) (void);
  void (*capture_async) (GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data);
  gchar *(*capture_finish) (GAsyncResult *result,
                            GError      **error);
} KasasaRegionCaptureOps;

typedef struct
{
  GtkWindow *(*get_window) (gpointer user_data);
  void (*hide_window) (gpointer            user_data,
                       gboolean            hide,
                       HideWindowCallback  callback,
                       GObject            *callback_data);
  void (*native_screenshot_captured) (gpointer                 user_data,
                                     KasasaNativeCaptureKind  kind,
                                     const gchar              *uri,
                                     const GError             *error);
  void (*region_screenshot_captured) (gpointer                 user_data,
                                      KasasaRegionCaptureKind  kind,
                                      const gchar              *uri,
                                      const GError             *error);
  void (*native_screencast_selected) (gpointer                 user_data,
                                      KasasaNativeCaptureKind  kind,
                                      guint32                  handle,
                                      gint                     width,
                                      gint                     height,
                                      const GError             *error);
  void (*capture_finished) (gpointer                 user_data,
                            KasasaNativeCaptureKind  kind);
  void (*region_capture_finished) (gpointer                  user_data,
                                   KasasaRegionCaptureKind   kind);
  void (*capture_cancelled) (gpointer                 user_data,
                             KasasaNativeCaptureKind  kind,
                             gboolean                 first_capture);
  void (*region_capture_cancelled) (gpointer                  user_data,
                                    KasasaRegionCaptureKind   kind,
                                    gboolean                  first_capture);
  void (*delayed_screenshot_scheduled) (gpointer user_data,
                                        guint    interval);
  void (*delayed_screenshot_started) (gpointer user_data);
} KasasaCaptureControllerCallbacks;

#define KASASA_TYPE_CAPTURE_CONTROLLER (kasasa_capture_controller_get_type ())
G_DECLARE_FINAL_TYPE (KasasaCaptureController,
                      kasasa_capture_controller,
                      KASASA,
                      CAPTURE_CONTROLLER,
                      GObject)

KasasaCaptureController *kasasa_capture_controller_new (
  GObject                                *owner,
  GSettings                              *settings,
  const KasasaCaptureControllerCallbacks *callbacks,
  gpointer                                user_data);
void kasasa_capture_controller_set_native_ops (
  KasasaCaptureController       *self,
  const KasasaNativeCaptureOps  *ops);
void kasasa_capture_controller_set_region_ops (
  KasasaCaptureController       *self,
  const KasasaRegionCaptureOps  *ops);
gboolean kasasa_capture_controller_is_native_pending (
  KasasaCaptureController *self);
gboolean kasasa_capture_controller_is_region_pending (
  KasasaCaptureController *self);
gboolean kasasa_capture_controller_is_delayed_pending (
  KasasaCaptureController *self);
void kasasa_capture_controller_begin_native (
  KasasaCaptureController *self,
  KasasaNativeCaptureKind  kind,
  GError                 **error);
void kasasa_capture_controller_begin_region (
  KasasaCaptureController *self,
  KasasaRegionCaptureKind  kind,
  GError                  **error);
gboolean kasasa_capture_controller_cancel_delayed (
  KasasaCaptureController *self);

G_END_DECLS
