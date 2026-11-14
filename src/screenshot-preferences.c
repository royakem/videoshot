/* screenshot-preferences.c - Preferences window
 *
 * Copyright (C) 2026 Roy-Åke Martinsson
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#include "config.h"

#include <glib/gi18n.h>
#include <handy.h>

#include "screenshot-config.h"
#include "screenshot-preferences.h"
#include "screenshot-video.h"

#define VIDEO_FORMAT_KEY         "video-format"
#define VIDEO_SAVE_DIRECTORY_KEY "video-save-directory"

static void
folder_set_cb (GtkFileChooserButton *button,
               gpointer              user_data)
{
  g_autofree gchar *folder = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (button));

  if (folder != NULL)
    g_settings_set_string (screenshot_config->settings, VIDEO_SAVE_DIRECTORY_KEY, folder);
}

void
screenshot_show_preferences (GtkWindow *parent)
{
  GtkWidget *dialog, *content, *listbox, *row, *combo, *folder;
  const ScreenshotVideoFormat *formats;
  g_autofree gchar *dir = NULL;
  guint n_formats, i;

  dialog = g_object_new (GTK_TYPE_DIALOG,
                         "title", _("Preferences"),
                         "transient-for", parent,
                         "modal", TRUE,
                         "resizable", FALSE,
                         "use-header-bar", TRUE,
                         NULL);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  gtk_container_set_border_width (GTK_CONTAINER (content), 24);

  listbox = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (listbox), GTK_SELECTION_NONE);
  gtk_widget_set_size_request (listbox, 420, -1);
  gtk_style_context_add_class (gtk_widget_get_style_context (listbox), "content");
  gtk_container_add (GTK_CONTAINER (content), listbox);

  /* output format */
  combo = gtk_combo_box_text_new ();
  gtk_widget_set_valign (combo, GTK_ALIGN_CENTER);
  formats = screenshot_video_get_formats (&n_formats);
  for (i = 0; i < n_formats; i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), formats[i].id, _(formats[i].name));
  g_settings_bind (screenshot_config->settings, VIDEO_FORMAT_KEY,
                   combo, "active-id", G_SETTINGS_BIND_DEFAULT);

  row = hdy_action_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (row), _("Videoshot Format"));
  hdy_action_row_set_activatable_widget (HDY_ACTION_ROW (row), combo);
  gtk_container_add (GTK_CONTAINER (row), combo);
  gtk_container_add (GTK_CONTAINER (listbox), row);

  /* output folder */
  folder = gtk_file_chooser_button_new (_("Videoshot Folder"),
                                        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  gtk_widget_set_valign (folder, GTK_ALIGN_CENTER);
  dir = screenshot_video_get_save_dir ();
  if (dir != NULL)
    gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (folder), dir);
  g_signal_connect (folder, "file-set", G_CALLBACK (folder_set_cb), NULL);

  row = hdy_action_row_new ();
  hdy_preferences_row_set_title (HDY_PREFERENCES_ROW (row), _("Videoshot Folder"));
  gtk_container_add (GTK_CONTAINER (row), folder);
  gtk_container_add (GTK_CONTAINER (listbox), row);

  g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);

  gtk_widget_show_all (dialog);
}
