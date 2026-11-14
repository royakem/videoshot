/* screenshot-video.h - Record a video clip of a screen area
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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  const gchar *id;          /* GSettings nick and file extension */
  const gchar *name;        /* shown in the preferences */
} ScreenshotVideoFormat;

/* Called once when a recording ends. @path is the saved file, or NULL
 * with @error set when nothing was saved.
 */
typedef void (* ScreenshotVideoCallback) (const gchar  *path,
                                          const GError *error,
                                          gpointer      user_data);

const ScreenshotVideoFormat *screenshot_video_get_formats        (guint *n_formats);
const gchar                 *screenshot_video_format_for_path    (const gchar *path);

gboolean                     screenshot_video_get_target_rect    (GdkRectangle *selection,
                                                                  GdkRectangle *rect_out);

gchar                       *screenshot_video_get_save_dir       (void);
gchar                       *screenshot_video_get_cache_dir      (void);
gchar                       *screenshot_video_build_filename     (const gchar *dir,
                                                                  const gchar *format);
GdkPixbuf                   *screenshot_video_get_preview        (const gchar *path);

void                         screenshot_video_record_async       (const GdkRectangle      *rect,
                                                                  guint                    length,
                                                                  gboolean                 include_pointer,
                                                                  const gchar             *format,
                                                                  const gchar             *path,
                                                                  ScreenshotVideoCallback  callback,
                                                                  gpointer                 user_data);

void                         screenshot_video_convert_async      (const gchar             *source,
                                                                  const gchar             *format,
                                                                  const gchar             *path,
                                                                  ScreenshotVideoCallback  callback,
                                                                  gpointer                 user_data);

gboolean                     screenshot_video_is_recording       (void);
void                         screenshot_video_stop               (void);

G_END_DECLS
