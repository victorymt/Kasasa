/* test-gtk-utils.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

static GtkWidget *
test_find_widget_by_id (GtkWidget  *root,
                        const char *id)
{
  GQueue pending = G_QUEUE_INIT;

  g_queue_push_tail (&pending, root);
  while (!g_queue_is_empty (&pending))
    {
      GtkWidget *widget = g_queue_pop_head (&pending);
      GtkWidget *child;
      const char *widget_id;

      widget_id = gtk_buildable_get_buildable_id (GTK_BUILDABLE (widget));
      if (g_strcmp0 (widget_id, id) == 0)
        return widget;

      for (child = gtk_widget_get_first_child (widget);
           child != NULL;
           child = gtk_widget_get_next_sibling (child))
        g_queue_push_tail (&pending, child);
    }

  return NULL;
}
