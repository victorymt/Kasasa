/* kasasa-window-picker.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib/gi18n.h>

#include "kasasa-window-picker.h"
#include "kasasa-window-picker-logic.h"

typedef struct
{
  GPtrArray                  *clients;
  GtkWindow                  *window;
  GtkSearchEntry             *search_entry;
  GtkListBox                 *list;
  GtkStack                   *stack;
  AdwStatusPage              *empty_page;
  KasasaWindowPickerCallback  callback;
  gpointer                    user_data;
  GDestroyNotify              destroy;
  gboolean                    completed;
} KasasaWindowPickerData;

static void
picker_data_free (KasasaWindowPickerData *data)
{
  if (data->destroy != NULL)
    data->destroy (data->user_data);

  g_clear_pointer (&data->clients, g_ptr_array_unref);
  g_free (data);
}

static gboolean
filter_window_row (GtkListBoxRow *row,
                   gpointer       user_data)
{
  KasasaWindowPickerData *data = user_data;
  KasasaWindowClient *client = g_object_get_data (G_OBJECT (row),
                                                   "kasasa-window-client");
  const gchar *query = gtk_editable_get_text (GTK_EDITABLE (data->search_entry));

  return kasasa_window_picker_matches_search (client->title, query)
         || kasasa_window_picker_matches_search (client->class_name, query)
         || kasasa_window_picker_matches_search (client->workspace_name, query);
}

static GtkListBoxRow *
first_visible_row (KasasaWindowPickerData *data)
{
  GtkListBoxRow *row;

  for (gint i = 0; (row = gtk_list_box_get_row_at_index (data->list, i)) != NULL; i++)
    {
      if (gtk_widget_get_child_visible (GTK_WIDGET (row)))
        return row;
    }

  return NULL;
}

static void
update_empty_state (KasasaWindowPickerData *data)
{
  GtkListBoxRow *row = first_visible_row (data);

  if (row != NULL)
    {
      gtk_stack_set_visible_child_name (data->stack, "windows");
      gtk_list_box_select_row (data->list, row);
      return;
    }

  adw_status_page_set_icon_name (data->empty_page,
                                 data->clients->len == 0
                                   ? "window-new-symbolic"
                                   : "system-search-symbolic");
  adw_status_page_set_title (data->empty_page,
                             data->clients->len == 0
                               ? _("No capturable windows")
                               : _("No matching windows"));
  adw_status_page_set_description (data->empty_page, NULL);
  gtk_stack_set_visible_child_name (data->stack, "empty");
}

static void
on_search_changed (GtkSearchEntry          *entry,
                   KasasaWindowPickerData *data)
{
  gtk_list_box_invalidate_filter (data->list);
  update_empty_state (data);
}

static void
activate_selected_row (KasasaWindowPickerData *data)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (data->list);

  if (row == NULL || !gtk_widget_get_child_visible (GTK_WIDGET (row)))
    row = first_visible_row (data);
  if (row != NULL)
    g_signal_emit_by_name (data->list, "row-activated", row);
}

static void
on_search_activate (GtkSearchEntry          *entry,
                    KasasaWindowPickerData *data)
{
  activate_selected_row (data);
}

static gboolean
on_search_key_pressed (GtkEventControllerKey  *controller,
                       guint                   keyval,
                       guint                   keycode,
                       GdkModifierType         state,
                       KasasaWindowPickerData *data)
{
  GtkListBoxRow *selected;
  GtkListBoxRow *candidate;
  gint index;
  gint direction;

  if (keyval != GDK_KEY_Down && keyval != GDK_KEY_Up)
    return GDK_EVENT_PROPAGATE;

  direction = keyval == GDK_KEY_Down ? 1 : -1;
  selected = gtk_list_box_get_selected_row (data->list);
  index = selected != NULL ? gtk_list_box_row_get_index (selected) : -direction;

  for (index += direction; index >= 0; index += direction)
    {
      candidate = gtk_list_box_get_row_at_index (data->list, index);
      if (candidate == NULL)
        break;
      if (gtk_widget_get_child_visible (GTK_WIDGET (candidate)))
        {
          gtk_list_box_select_row (data->list, candidate);
          return GDK_EVENT_STOP;
        }
    }

  return GDK_EVENT_STOP;
}

static void
on_window_row_activated (GtkListBox             *list,
                         GtkListBoxRow          *row,
                         KasasaWindowPickerData *data)
{
  GtkRoot *root;
  g_autoptr (KasasaWindowClient) client = NULL;

  if (data->completed)
    return;

  client = kasasa_window_client_copy (
    g_object_get_data (G_OBJECT (row), "kasasa-window-client"));
  root = gtk_widget_get_root (GTK_WIDGET (list));
  if (root != NULL)
    g_object_ref (root);
  data->completed = TRUE;
  data->callback (client, data->user_data);

  if (GTK_IS_WINDOW (root))
    gtk_window_destroy (GTK_WINDOW (root));
  g_clear_object (&root);
}

static gboolean
on_picker_close_request (GtkWindow              *window,
                         KasasaWindowPickerData *data)
{
  if (!data->completed)
    {
      data->completed = TRUE;
      data->callback (NULL, data->user_data);
    }

  return FALSE;
}

static GtkWidget *
create_window_row (KasasaWindowClient *client)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *text_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *icon = gtk_image_new_from_icon_name ("window-new-symbolic");
  GtkWidget *title = gtk_label_new (NULL);
  GtkWidget *details = gtk_label_new (NULL);
  g_autofree gchar *detail_text = NULL;
  const gchar *display_title;

  display_title = client->title != NULL && *client->title != '\0'
                  ? client->title
                  : client->class_name;
  if (display_title == NULL || *display_title == '\0')
    display_title = _("Untitled window");

  detail_text = g_strdup_printf ("%s  |  %s  |  %dx%d",
                                 client->class_name != NULL
                                   ? client->class_name : _("Unknown app"),
                                 client->workspace_name != NULL
                                   ? client->workspace_name : _("Unknown workspace"),
                                 client->width,
                                 client->height);

  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 24);

  gtk_label_set_text (GTK_LABEL (title), display_title);
  gtk_widget_set_tooltip_text (title, display_title);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (title, "heading");

  gtk_label_set_text (GTK_LABEL (details), detail_text);
  gtk_widget_set_tooltip_text (details, detail_text);
  gtk_label_set_xalign (GTK_LABEL (details), 0.0f);
  gtk_label_set_ellipsize (GTK_LABEL (details), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (details, "dim-label");

  gtk_widget_set_hexpand (text_box, TRUE);
  gtk_box_append (GTK_BOX (text_box), title);
  gtk_box_append (GTK_BOX (text_box), details);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_box_append (GTK_BOX (box), text_box);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "kasasa-window-client", client);

  return row;
}

static void
replace_clients (KasasaWindowPickerData *data,
                 GPtrArray              *clients)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (data->list))) != NULL)
    gtk_list_box_remove (data->list, child);

  g_clear_pointer (&data->clients, g_ptr_array_unref);
  data->clients = clients;

  for (guint i = 0; i < data->clients->len; i++)
    gtk_list_box_append (data->list,
                         create_window_row (g_ptr_array_index (data->clients, i)));

  gtk_window_set_default_size (data->window,
                               520,
                               kasasa_window_picker_height_for_count (data->clients->len));
  gtk_list_box_invalidate_filter (data->list);
  update_empty_state (data);
}

static void
on_refresh_clicked (GtkButton               *button,
                    KasasaWindowPickerData *data)
{
  g_autoptr (GError) error = NULL;
  GPtrArray *clients;

  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  clients = kasasa_window_query_list_clients (&error);
  gtk_widget_set_sensitive (GTK_WIDGET (button), TRUE);

  if (clients == NULL)
    {
      adw_status_page_set_icon_name (data->empty_page, "dialog-warning-symbolic");
      adw_status_page_set_title (data->empty_page, _("Couldn't refresh windows"));
      adw_status_page_set_description (data->empty_page,
                                       error != NULL ? error->message : NULL);
      gtk_stack_set_visible_child_name (data->stack, "empty");
      return;
    }

  replace_clients (data, clients);
}

gboolean
kasasa_window_picker_present (GtkWindow                  *parent,
                              const gchar                *title,
                              KasasaWindowPickerCallback  callback,
                              gpointer                    user_data,
                              GDestroyNotify              destroy,
                              GError                    **error)
{
  g_autoptr (GPtrArray) clients = NULL;
  KasasaWindowPickerData *data;
  GtkApplication *application;
  GtkWidget *window;
  GtkWidget *header;
  GtkWidget *content;
  GtkWidget *search;
  GtkEventController *search_keys;
  GtkWidget *refresh;
  GtkWidget *scroller;
  GtkWidget *list;
  GtkWidget *stack;
  GtkWidget *empty_page;

  g_return_val_if_fail (GTK_IS_WINDOW (parent), FALSE);
  g_return_val_if_fail (callback != NULL, FALSE);

  clients = kasasa_window_query_list_clients (error);
  if (clients == NULL)
    return FALSE;

  data = g_new0 (KasasaWindowPickerData, 1);
  data->callback = callback;
  data->user_data = user_data;
  data->destroy = destroy;

  window = adw_window_new ();
  application = gtk_window_get_application (parent);
  if (application != NULL)
    gtk_window_set_application (GTK_WINDOW (window), application);
  gtk_window_set_title (GTK_WINDOW (window), title);
  gtk_window_set_modal (GTK_WINDOW (window), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (window), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (window), TRUE);

  header = adw_header_bar_new ();
  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  search = gtk_search_entry_new ();
  refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  scroller = gtk_scrolled_window_new ();
  list = gtk_list_box_new ();
  stack = gtk_stack_new ();
  empty_page = adw_status_page_new ();

  gtk_widget_set_tooltip_text (refresh, _("Refresh windows"));
  gtk_widget_add_css_class (refresh, "flat");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), refresh);

  gtk_widget_set_margin_start (search, 12);
  gtk_widget_set_margin_end (search, 12);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search),
                                         _("Search windows"));
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (stack, TRUE);
  gtk_widget_set_margin_start (stack, 12);
  gtk_widget_set_margin_end (stack, 12);
  gtk_widget_set_margin_bottom (stack, 12);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list), TRUE);
  gtk_widget_add_css_class (list, "boxed-list");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), list);
  gtk_stack_add_named (GTK_STACK (stack), scroller, "windows");
  gtk_stack_add_named (GTK_STACK (stack), empty_page, "empty");

  gtk_box_append (GTK_BOX (content), header);
  gtk_box_append (GTK_BOX (content), search);
  gtk_box_append (GTK_BOX (content), stack);
  adw_window_set_content (ADW_WINDOW (window), content);

  data->window = GTK_WINDOW (window);
  data->search_entry = GTK_SEARCH_ENTRY (search);
  data->list = GTK_LIST_BOX (list);
  data->stack = GTK_STACK (stack);
  data->empty_page = ADW_STATUS_PAGE (empty_page);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (list),
                                filter_window_row,
                                data,
                                NULL);
  replace_clients (data, g_steal_pointer (&clients));

  g_signal_connect (search, "search-changed", G_CALLBACK (on_search_changed), data);
  g_signal_connect (search, "activate", G_CALLBACK (on_search_activate), data);
  g_signal_connect (list, "row-activated", G_CALLBACK (on_window_row_activated), data);
  g_signal_connect (refresh, "clicked", G_CALLBACK (on_refresh_clicked), data);
  g_signal_connect (window, "close-request", G_CALLBACK (on_picker_close_request), data);
  search_keys = gtk_event_controller_key_new ();
  g_signal_connect (search_keys,
                    "key-pressed",
                    G_CALLBACK (on_search_key_pressed),
                    data);
  gtk_widget_add_controller (search, search_keys);
  g_object_set_data_full (G_OBJECT (window),
                          "kasasa-window-picker-data",
                          data,
                          (GDestroyNotify) picker_data_free);

  gtk_window_present (GTK_WINDOW (window));
  gtk_widget_grab_focus (search);
  return TRUE;
}
