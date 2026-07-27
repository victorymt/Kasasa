/* kasasa-screenshot.c
 *
 * Copyright 2024-2025 Kelvin Novais
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

#include "kasasa-screenshot.h"
#include "kasasa-image.h"
#include "kasasa-window.h"

struct _KasasaScreenshot
{
  AdwBin                  parent_instance;

  /* Instance variables */
  GFile                  *file;
  GtkPicture             *picture;
  gint                    image_height;
  gint                    image_width;
};

static void kasasa_screenshot_content_interface_init (KasasaContentInterface *iface);

G_DEFINE_TYPE_WITH_CODE (KasasaScreenshot, kasasa_screenshot, ADW_TYPE_BIN,
                         G_IMPLEMENT_INTERFACE (KASASA_TYPE_CONTENT,
                                                kasasa_screenshot_content_interface_init))

GFile *
kasasa_screenshot_get_file (KasasaScreenshot *self)
{
  g_return_val_if_fail (KASASA_IS_SCREENSHOT (self), NULL);
  return self->file;
}

static void
report_trash_failure (const gchar *message)
{
  GApplication *application = g_application_get_default ();

  g_warning ("Error while deleting screenshot: %s", message);

  if (application != NULL)
    {
      g_autoptr (GNotification) notification = NULL;

      notification = g_notification_new (_("Couldn't move screenshot to Trash"));
      g_notification_set_body (notification, message);
      g_application_send_notification (application,
                                       "screenshot-trash-failed",
                                       notification);
    }
}

static void
kasasa_screenshot_get_dimensions (KasasaContent *content,
                                  gint          *height,
                                  gint          *width)
{
  KasasaScreenshot *self = NULL;

  g_return_if_fail (KASASA_IS_SCREENSHOT (content));

  self = KASASA_SCREENSHOT (content);

  *height = self->image_height;
  *width = self->image_width;
}

static void
kasasa_screenshot_finish (KasasaContent *content)
{
  KasasaWindow *window = NULL;
  g_autoptr (GError) error = NULL;
  KasasaScreenshot *self = NULL;

  g_return_if_fail (KASASA_IS_SCREENSHOT (content));

  self = KASASA_SCREENSHOT (content);

  window = kasasa_window_get_window_reference (GTK_WIDGET (self));

  // Return if auto trashing screenshot is not enabled
  if (kasasa_window_get_trash_button_active (window) == FALSE)
    return;

  g_debug ("Auto trashing screenshot...");

  if (self->file == NULL)
    {
      report_trash_failure (_("No screenshot file is available"));
      return;
    }

  if (!g_file_trash (self->file, NULL, &error))
    {
      report_trash_failure (error->message);
      return;
    }

  gtk_picture_set_file (self->picture, NULL);
}

// Load the screenshot to the GtkPicture widget
gboolean
kasasa_screenshot_load_screenshot (KasasaScreenshot *self,
                                   const gchar      *uri,
                                   GError          **error)
{
  g_autoptr (GFile) new_file = NULL;
  g_autoptr (GdkTexture) texture = NULL;

  g_return_val_if_fail (KASASA_IS_SCREENSHOT (self), FALSE);
  g_return_val_if_fail (uri != NULL, FALSE);

  if (!kasasa_image_load_uri (uri, &new_file, &texture, error))
    return FALSE;

  // Only finish/trash the old image after its replacement has been validated.
  if (self->file != NULL && !g_file_equal (self->file, new_file))
    kasasa_screenshot_finish (KASASA_CONTENT (self));

  g_set_object (&self->file, new_file);

  self->image_height = gdk_texture_get_height (texture);
  self->image_width = gdk_texture_get_width (texture);

  // Explicitly unset the previous image: for some reason the old image doesn't get
  // replaced if the new image have the same size
  gtk_picture_set_file (self->picture, NULL);
  gtk_picture_set_file (self->picture, self->file);

  return TRUE;
}

static void
kasasa_screenshot_dispose (GObject *object)
{
  KasasaScreenshot *self = KASASA_SCREENSHOT (object);

  g_clear_object (&self->file);

  G_OBJECT_CLASS (kasasa_screenshot_parent_class)->dispose (object);
}

static void
kasasa_screenshot_content_interface_init (KasasaContentInterface *iface)
{
  iface->get_dimensions = kasasa_screenshot_get_dimensions;
  iface->finish = kasasa_screenshot_finish;
}

/*
 * Window default-size is the only size source. We deliberately report 0×0 so
 * GtkPicture's texture pixels and AdwCarousel cannot race the window frame
 * (that race showed up as a gap on the right that then jumped shut).
 */
static void
kasasa_screenshot_measure (GtkWidget      *widget,
                           GtkOrientation  orientation,
                           int             for_size,
                           int            *minimum,
                           int            *natural,
                           int            *minimum_baseline,
                           int            *natural_baseline)
{
  *minimum = 0;
  *natural = 0;
  *minimum_baseline = -1;
  *natural_baseline = -1;
}

static void
kasasa_screenshot_size_allocate (GtkWidget *widget,
                                 int        width,
                                 int        height,
                                 int        baseline)
{
  KasasaScreenshot *self = KASASA_SCREENSHOT (widget);
  GtkWidget *child = adw_bin_get_child (ADW_BIN (widget));
  GtkContentFit fit;

  fit = kasasa_content_should_fill_allocation (self->image_height,
                                               self->image_width,
                                               height,
                                               width)
        ? GTK_CONTENT_FIT_FILL
        : GTK_CONTENT_FIT_CONTAIN;
  if (gtk_picture_get_content_fit (self->picture) != fit)
    gtk_picture_set_content_fit (self->picture, fit);

  if (child != NULL)
    gtk_widget_allocate (child, width, height, baseline, NULL);
}

static void
kasasa_screenshot_class_init (KasasaScreenshotClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = kasasa_screenshot_dispose;

  widget_class->measure = kasasa_screenshot_measure;
  widget_class->size_allocate = kasasa_screenshot_size_allocate;
}

static void
kasasa_screenshot_init (KasasaScreenshot *self)
{
  self->picture = GTK_PICTURE (gtk_picture_new ());

  /* Allocation switches to FILL when only pixel rounding differs. */
  gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_picture_set_can_shrink (self->picture, TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->picture), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->picture), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->picture), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self->picture), GTK_ALIGN_FILL);

  adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (self->picture));

  /* Parent construction may install a bin layout manager — remove it. */
  gtk_widget_set_layout_manager (GTK_WIDGET (self), NULL);

  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_FILL);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_FILL);
}

KasasaScreenshot *
kasasa_screenshot_new (void)
{
  return KASASA_SCREENSHOT (g_object_new (KASASA_TYPE_SCREENSHOT, NULL));
}
