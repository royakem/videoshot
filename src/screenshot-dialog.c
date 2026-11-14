/* screenshot-dialog.c - main GNOME Screenshot dialog
 *
 * Copyright (C) 2001-2006  Jonathan Blandford <jrb@alum.mit.edu>
 * Copyright (C) 2026 Roy-Åke Martinsson (Videoshot changes)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 */

#include "config.h"

#include "screenshot-config.h"
#include "screenshot-dialog.h"
#include "screenshot-utils.h"
#include "screenshot-video.h"
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <string.h>
#include <stdlib.h>

struct _ScreenshotDialog
{
  HdyApplicationWindow parent_instance;

  GdkPixbuf *screenshot;
  GdkPixbuf *preview_image;

  GtkWidget *save_widget;
  GtkWidget *filename_entry;
  GtkWidget *preview_darea;
  GtkWidget *format_label;
  GtkWidget *format_combo;

  gint drag_x;
  gint drag_y;

  /* a videoshot can be played where its still is shown */
  gchar *video_path;
  struct _Player *player;
  cairo_surface_t *frame;
  gint image_width;
  gint image_height;
  gdouble icon_alpha;
  gdouble icon_target;
  guint fade_id;
};

/* Plays the videoshot: ffmpeg decodes it at its own pace, scaled to the
 * preview, and writes plain frames that are painted as they arrive.
 */
typedef struct _Player
{
  ScreenshotDialog *dialog;   /* NULL once the dialog has let go of it */
  GSubprocess *process;
  GInputStream *stream;
  GCancellable *cancellable;
  guchar *buffer;
  gsize size;
  gint width;
  gint height;
} Player;

#define FADE_INTERVAL 16 /* milliseconds */
#define FADE_STEP     0.12

G_DEFINE_TYPE (ScreenshotDialog, screenshot_dialog, HDY_TYPE_APPLICATION_WINDOW)

enum {
  SIGNAL_SAVE,
  SIGNAL_COPY,
  SIGNAL_BACK,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

enum {
  TYPE_IMAGE_PNG,
  LAST_TYPE
};

static GtkTargetEntry drag_types[] =
{
  { "image/png", 0, TYPE_IMAGE_PNG },
};

static void
player_free (Player *player)
{
  g_clear_object (&player->process);
  g_clear_object (&player->cancellable);
  g_free (player->buffer);
  g_free (player);
}

static gboolean
fade_cb (gpointer user_data)
{
  ScreenshotDialog *self = user_data;

  if (self->icon_alpha < self->icon_target)
    self->icon_alpha = MIN (self->icon_alpha + FADE_STEP, self->icon_target);
  else
    self->icon_alpha = MAX (self->icon_alpha - FADE_STEP, self->icon_target);

  gtk_widget_queue_draw (self->preview_darea);

  if (self->icon_alpha != self->icon_target)
    return G_SOURCE_CONTINUE;

  self->fade_id = 0;

  return G_SOURCE_REMOVE;
}

static void
fade_icon (ScreenshotDialog *self,
           gdouble           target)
{
  self->icon_target = target;

  if (self->fade_id == 0 && self->icon_alpha != target)
    self->fade_id = g_timeout_add (FADE_INTERVAL, fade_cb, self);
}

/* Back to the still with the play icon over it. */
static void
player_stop (ScreenshotDialog *self)
{
  if (self->player != NULL)
    {
      /* the pending read ends with an error and frees the player */
      self->player->dialog = NULL;
      g_cancellable_cancel (self->player->cancellable);
      g_subprocess_force_exit (self->player->process);
      self->player = NULL;
    }

  g_clear_pointer (&self->frame, cairo_surface_destroy);
  fade_icon (self, 1.0);
  gtk_widget_queue_draw (self->preview_darea);
}

static void
player_read_cb (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  Player *player = user_data;
  ScreenshotDialog *self = player->dialog;
  gsize size = 0;
  gint row, stride;
  guchar *pixels;

  g_input_stream_read_all_finish (G_INPUT_STREAM (source), result, &size, NULL);

  if (self == NULL)
    {
      player_free (player);
      return;
    }

  if (size < player->size)
    {
      /* the end of the clip */
      player_stop (self);
      player_free (player);
      return;
    }

  if (self->frame == NULL)
    self->frame = cairo_image_surface_create (CAIRO_FORMAT_RGB24, player->width, player->height);

  cairo_surface_flush (self->frame);
  pixels = cairo_image_surface_get_data (self->frame);
  stride = cairo_image_surface_get_stride (self->frame);
  for (row = 0; row < player->height; row++)
    memcpy (pixels + row * stride, player->buffer + row * player->width * 4, player->width * 4);
  cairo_surface_mark_dirty (self->frame);
  gtk_widget_queue_draw (self->preview_darea);

  g_input_stream_read_all_async (player->stream, player->buffer, player->size,
                                 G_PRIORITY_DEFAULT, player->cancellable,
                                 player_read_cb, player);
}

static void
player_start (ScreenshotDialog *self)
{
  g_autofree gchar *filter = NULL;
  Player *player;

  if (self->player != NULL || self->image_width < 2 || self->image_height < 2)
    return;

  player = g_new0 (Player, 1);
  player->width = self->image_width;
  player->height = self->image_height;
  player->size = (gsize) player->width * player->height * 4;

  filter = g_strdup_printf ("scale=%d:%d", player->width, player->height);
  /* "-re" makes ffmpeg hand out the frames at the speed of the clip */
  player->process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                      NULL,
                                      "ffmpeg", "-nostdin", "-loglevel", "error",
                                      "-re", "-i", self->video_path,
                                      "-vf", filter, "-an",
                                      "-f", "rawvideo", "-pix_fmt", "bgra", "-",
                                      NULL);
  if (player->process == NULL)
    {
      player_free (player);
      return;
    }

  player->dialog = self;
  player->stream = g_subprocess_get_stdout_pipe (player->process);
  player->cancellable = g_cancellable_new ();
  player->buffer = g_malloc (player->size);
  self->player = player;

  fade_icon (self, 0.0);
  g_input_stream_read_all_async (player->stream, player->buffer, player->size,
                                 G_PRIORITY_DEFAULT, player->cancellable,
                                 player_read_cb, player);
}

/* A round button with a triangle, in the middle of the still. */
static void
draw_play_icon (cairo_t *cr,
                gdouble  x,
                gdouble  y,
                gdouble  radius,
                gdouble  alpha)
{
  cairo_arc (cr, x, y, radius, 0, 2 * G_PI);
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.6 * alpha);
  cairo_fill_preserve (cr);
  cairo_set_line_width (cr, radius / 14);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.9 * alpha);
  cairo_stroke (cr);

  cairo_move_to (cr, x - radius * 0.28, y - radius * 0.45);
  cairo_line_to (cr, x + radius * 0.48, y);
  cairo_line_to (cr, x - radius * 0.28, y + radius * 0.45);
  cairo_close_path (cr);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
  cairo_fill (cr);
}

static void
preview_draw_cb (GtkWidget        *drawing_area,
                 cairo_t          *cr,
                 ScreenshotDialog *self)
{
  gint scale, width, height, image_width, image_height, x, y;

  scale = gtk_widget_get_scale_factor (drawing_area);
  width = gtk_widget_get_allocated_width (drawing_area) * scale;
  height = gtk_widget_get_allocated_height (drawing_area) * scale;

  image_width = gdk_pixbuf_get_width (self->screenshot);
  image_height = gdk_pixbuf_get_height (self->screenshot);

  if (image_width > width)
    {
      image_height = (gdouble) image_height / image_width * width;
      image_width = width;

    }

  if (image_height > height)
    {
      image_width = (gdouble) image_width / image_height * height;
      image_height = height;
    }

  x = (width - image_width) / 2;
  y = (height - image_height) / 2;

  if (!self->preview_image ||
      gdk_pixbuf_get_width (self->preview_image) != image_width ||
      gdk_pixbuf_get_height (self->preview_image) != image_height)
    {
      g_clear_object (&self->preview_image);
      self->preview_image = gdk_pixbuf_scale_simple (self->screenshot,
                                                     image_width,
                                                     image_height,
                                                     GDK_INTERP_BILINEAR);
    }

  cairo_save (cr);

  cairo_scale (cr, 1.0 / scale, 1.0 / scale);
  gdk_cairo_set_source_pixbuf (cr, self->preview_image, x, y);
  cairo_paint (cr);

  self->image_width = image_width;
  self->image_height = image_height;

  if (self->frame != NULL)
    {
      /* the frames were asked for in the size the preview had at the start */
      cairo_save (cr);
      cairo_translate (cr, x, y);
      cairo_scale (cr,
                   (gdouble) image_width / cairo_image_surface_get_width (self->frame),
                   (gdouble) image_height / cairo_image_surface_get_height (self->frame));
      cairo_set_source_surface (cr, self->frame, 0, 0);
      cairo_paint (cr);
      cairo_restore (cr);
    }

  if (self->video_path != NULL && self->icon_alpha > 0.0)
    draw_play_icon (cr, x + image_width / 2.0, y + image_height / 2.0,
                    MIN (36.0 * scale, MIN (image_width, image_height) / 3.0),
                    self->icon_alpha);

  cairo_restore (cr);
}

static gboolean
preview_button_press_event_cb (GtkWidget        *drawing_area,
                               GdkEventButton   *event,
                               ScreenshotDialog *self)
{
  self->drag_x = (int) event->x;
  self->drag_y = (int) event->y;

  return FALSE;
}

static gboolean
preview_button_release_event_cb (GtkWidget        *drawing_area,
                                 GdkEventButton   *event,
                                 ScreenshotDialog *self)
{
  self->drag_x = 0;
  self->drag_y = 0;

  /* a click on a videoshot plays it; a click while it plays stops it */
  if (self->video_path != NULL && event->button == GDK_BUTTON_PRIMARY)
    {
      if (self->player != NULL)
        player_stop (self);
      else
        player_start (self);
    }

  return FALSE;
}

static void
drag_begin_cb (GtkWidget        *widget,
               GdkDragContext   *context,
               ScreenshotDialog *self)
{
  gtk_drag_set_icon_pixbuf (context, self->preview_image,
                            self->drag_x, self->drag_y);
}

static void
drag_data_get_cb (GtkWidget        *widget,
                  GdkDragContext   *context,
                  GtkSelectionData *selection_data,
                  guint             info,
                  guint             time,
                  ScreenshotDialog *self)
{
  if (info == TYPE_IMAGE_PNG)
    gtk_selection_data_set_pixbuf (selection_data, self->screenshot);
  else
    g_warning ("Unknown type %d", info);
}

static gboolean
key_press_cb (GtkWidget        *widget,
              GdkEventKey      *event,
              ScreenshotDialog *self)
{
  if (event->keyval == GDK_KEY_Escape)
    {
      gtk_widget_destroy (widget);
      return TRUE;
    }

  return FALSE;
}

static void
back_clicked_cb (GtkButton        *button,
                 ScreenshotDialog *self)
{
  g_signal_emit (self, signals[SIGNAL_BACK], 0);
}

static void
save_clicked_cb (GtkButton        *button,
                 ScreenshotDialog *self)
{
  g_signal_emit (self, signals[SIGNAL_SAVE], 0);
}

static void
copy_clicked_cb (GtkButton        *button,
                 ScreenshotDialog *self)
{
  g_signal_emit (self, signals[SIGNAL_COPY], 0);
}

/* Keeps the ending of the file name in step with the chosen video format. */
static void
format_changed_cb (GtkComboBox      *combo,
                   ScreenshotDialog *self)
{
  const gchar *format = gtk_combo_box_get_active_id (combo);
  const gchar *name = gtk_entry_get_text (GTK_ENTRY (self->filename_entry));
  const gchar *ext = strrchr (name, '.');
  g_autofree gchar *stem = NULL, *new_name = NULL;

  if (format == NULL || !gtk_widget_get_visible (GTK_WIDGET (combo)))
    return;

  if (ext != NULL && g_ascii_strcasecmp (ext + 1, format) == 0)
    return;

  /* only an ending that names a video format is replaced */
  if (ext != NULL && screenshot_video_format_for_path (name) != NULL)
    stem = g_strndup (name, ext - name);
  else
    stem = g_strdup (name);

  new_name = g_strdup_printf ("%s.%s", stem, format);
  gtk_entry_set_text (GTK_ENTRY (self->filename_entry), new_name);
}

static void
screenshot_dialog_dispose (GObject *object)
{
  ScreenshotDialog *self = SCREENSHOT_DIALOG (object);

  if (self->player != NULL)
    {
      self->player->dialog = NULL;
      g_cancellable_cancel (self->player->cancellable);
      g_subprocess_force_exit (self->player->process);
      self->player = NULL;
    }

  if (self->fade_id != 0)
    {
      g_source_remove (self->fade_id);
      self->fade_id = 0;
    }

  g_clear_pointer (&self->frame, cairo_surface_destroy);
  g_clear_pointer (&self->video_path, g_free);

  G_OBJECT_CLASS (screenshot_dialog_parent_class)->dispose (object);
}

static void
screenshot_dialog_class_init (ScreenshotDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = screenshot_dialog_dispose;

  signals[SIGNAL_SAVE] =
    g_signal_new ("save",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);

  signals[SIGNAL_COPY] =
    g_signal_new ("copy",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);

  signals[SIGNAL_BACK] =
    g_signal_new ("back",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/com/royake/Videoshot/ui/screenshot-dialog.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, filename_entry);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, save_widget);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, preview_darea);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, format_label);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, format_combo);
  gtk_widget_class_bind_template_callback (widget_class, format_changed_cb);
  gtk_widget_class_bind_template_callback (widget_class, key_press_cb);
  gtk_widget_class_bind_template_callback (widget_class, back_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, save_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, copy_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, preview_draw_cb);
  gtk_widget_class_bind_template_callback (widget_class, preview_button_press_event_cb);
  gtk_widget_class_bind_template_callback (widget_class, preview_button_release_event_cb);
  gtk_widget_class_bind_template_callback (widget_class, drag_begin_cb);
  gtk_widget_class_bind_template_callback (widget_class, drag_data_get_cb);
}

static void
screenshot_dialog_init (ScreenshotDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

ScreenshotDialog *
screenshot_dialog_new (GtkApplication *app,
                       GdkPixbuf      *screenshot,
                       char           *initial_uri)
{
  g_autoptr(GFile) tmp_file = NULL, parent_file = NULL;
  g_autofree gchar *current_folder = NULL, *current_name = NULL;
  ScreenshotDialog *self;
  char *ext;
  gint pos;

  tmp_file = g_file_new_for_uri (initial_uri);
  parent_file = g_file_get_parent (tmp_file);

  current_name = g_file_get_basename (tmp_file);
  current_folder = g_file_get_uri (parent_file);

  self = g_object_new (SCREENSHOT_TYPE_DIALOG,
                       "application", app,
                       NULL);

  self->screenshot = screenshot;

  gtk_widget_realize (GTK_WIDGET (self));

  gtk_file_chooser_set_current_folder_uri (GTK_FILE_CHOOSER (self->save_widget), current_folder);
  gtk_entry_set_text (GTK_ENTRY (self->filename_entry), current_name);

  /* setup dnd */
  gtk_drag_source_set (self->preview_darea,
                       GDK_BUTTON1_MASK | GDK_BUTTON3_MASK,
                       drag_types, G_N_ELEMENTS (drag_types),
                       GDK_ACTION_COPY);

  gtk_widget_show_all (GTK_WIDGET (self));

  /* select the name of the file but leave out the extension if there's any;
   * the dialog must be realized for select_region to work
   */
  ext = g_utf8_strrchr (current_name, -1, '.');
  if (ext)
    pos = g_utf8_strlen (current_name, -1) - g_utf8_strlen (ext, -1);
  else
    pos = -1;

  gtk_widget_grab_focus (self->filename_entry);
  gtk_editable_select_region (GTK_EDITABLE (self->filename_entry),
                              0,
                              pos);

  return self;
}

/* Turns the dialog into the result window of a videoshot: the preview is
 * a still of the video at @path with a play icon over it, a click plays it
 * there, and a format can be chosen.
 */
void
screenshot_dialog_set_video (ScreenshotDialog *self,
                             const gchar      *path,
                             const gchar      *format)
{
  const ScreenshotVideoFormat *formats;
  guint n_formats, i;

  /* the still is not the video, so it is not offered for dragging */
  gtk_drag_source_unset (self->preview_darea);

  g_free (self->video_path);
  self->video_path = g_strdup (path);
  self->icon_alpha = self->icon_target = 1.0;
  gtk_widget_add_events (self->preview_darea, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

  formats = screenshot_video_get_formats (&n_formats);
  for (i = 0; i < n_formats; i++)
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (self->format_combo),
                               formats[i].id, _(formats[i].name));
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (self->format_combo), format);

  gtk_widget_show (self->format_label);
  gtk_widget_show (self->format_combo);
}

const gchar *
screenshot_dialog_get_video_format (ScreenshotDialog *self)
{
  return gtk_combo_box_get_active_id (GTK_COMBO_BOX (self->format_combo));
}

char *
screenshot_dialog_get_uri (ScreenshotDialog *self)
{
  g_autofree gchar *folder = NULL, *file = NULL, *tmp = NULL;

  folder = gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (self->save_widget));
  tmp = screenshot_dialog_get_filename (self);
  file = g_uri_escape_string (tmp, NULL, FALSE);

  return g_build_filename (folder, file, NULL);
}

char *
screenshot_dialog_get_folder (ScreenshotDialog *self)
{
  return gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (self->save_widget));
}

char *
screenshot_dialog_get_filename (ScreenshotDialog *self)
{
  g_autoptr(GError) error = NULL;
  const gchar *file_name;
  gchar *tmp;

  file_name = gtk_entry_get_text (GTK_ENTRY (self->filename_entry));
  tmp = g_filename_from_utf8 (file_name, -1, NULL, NULL, &error);

  if (error != NULL)
    {
      g_warning ("Unable to convert `%s' to valid UTF-8: %s\n"
                 "Falling back to default file.",
                 file_name,
                 error->message);
      tmp = g_strdup (_("Screenshot.png"));
    }

  return tmp;
}

void
screenshot_dialog_set_busy (ScreenshotDialog *self,
                            gboolean          busy)
{
  GdkWindow *window;

  window = gtk_widget_get_window (GTK_WIDGET (self));

  if (busy)
    {
      g_autoptr(GdkCursor) cursor = NULL;
      GdkDisplay *display;
      /* Change cursor to busy */
      display = gtk_widget_get_display (GTK_WIDGET (self));
      cursor = gdk_cursor_new_for_display (display, GDK_WATCH);
      gdk_window_set_cursor (window, cursor);
    }
  else
    {
      gdk_window_set_cursor (window, NULL);
    }

  gtk_widget_set_sensitive (GTK_WIDGET (self), !busy);
}

GtkWidget *
screenshot_dialog_get_filename_entry (ScreenshotDialog *self)
{
  return self->filename_entry;
}
