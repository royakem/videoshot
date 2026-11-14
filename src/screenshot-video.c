/* screenshot-video.c - Record a video clip of a screen area
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

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "screenshot-config.h"
#include "screenshot-video.h"

#ifdef HAVE_X11
#include "screenshot-backend-x11.h"
#endif

#define VIDEO_SAVE_DIRECTORY_KEY "video-save-directory"
#define VIDEO_FRAMERATE          "30"
#define GIF_FRAMERATE            "15"

/* Lossless recording a GIF is made from, never shown to the user. */
#define MASTER_FORMAT            "master"

/* The recording is done by an ffmpeg child process grabbing the X display.
 * It stops by itself after the requested length; stopping early sends it
 * SIGINT, which makes it finish the file properly.
 *
 * A GIF needs its palette made from the whole clip, which ffmpeg cannot
 * finish when interrupted. So a GIF is recorded losslessly first and
 * converted when the recording has ended.
 */

static const ScreenshotVideoFormat formats[] = {
  { "mp4",  N_("MP4 (H.264)") },
  { "webm", N_("WebM (VP9)") },
  { "mkv",  N_("Matroska (H.264)") },
  { "gif",  N_("Animated GIF") },
};

typedef struct {
  GSubprocess *process;
  gchar *path;
  gchar *part_path;
  GtkStatusIcon *status_icon;
  ScreenshotVideoCallback callback;
  gpointer user_data;
} Recording;

static Recording *current;

const ScreenshotVideoFormat *
screenshot_video_get_formats (guint *n_formats)
{
  if (n_formats != NULL)
    *n_formats = G_N_ELEMENTS (formats);

  return formats;
}

/* Returns the format matching the extension of @path, or NULL. */
const gchar *
screenshot_video_format_for_path (const gchar *path)
{
  const gchar *extension = strrchr (path, '.');
  guint i;

  if (extension == NULL)
    return NULL;

  for (i = 0; i < G_N_ELEMENTS (formats); i++)
    if (g_ascii_strcasecmp (extension + 1, formats[i].id) == 0)
      return formats[i].id;

  return NULL;
}

gboolean
screenshot_video_get_target_rect (GdkRectangle *selection,
                                  GdkRectangle *rect_out)
{
#ifdef HAVE_X11
  return screenshot_backend_x11_get_target_rect (selection, rect_out);
#else
  return FALSE;
#endif
}

/* The folder videoshots are saved in, as a path. */
gchar *
screenshot_video_get_save_dir (void)
{
  g_autofree gchar *dir = NULL;

  dir = g_settings_get_string (screenshot_config->settings, VIDEO_SAVE_DIRECTORY_KEY);

  if (strstr (dir, "://") != NULL)
    {
      g_autoptr(GFile) file = g_file_new_for_uri (dir);
      g_autofree gchar *path = g_file_get_path (file);

      if (path != NULL)
        return g_steal_pointer (&path);
    }
  else if (*dir != '\0')
    return g_steal_pointer (&dir);

  if (g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS) != NULL)
    return g_strdup (g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS));

  return g_strdup (g_get_home_dir ());
}

/* Where recordings wait until they are saved or copied. Files left there
 * for more than a day are removed.
 */
gchar *
screenshot_video_get_cache_dir (void)
{
  gchar *dir = g_build_filename (g_get_user_cache_dir (), "videoshot", NULL);
  g_autoptr(GDir) contents = NULL;
  const gchar *name;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  g_mkdir_with_parents (dir, 0700);

  contents = g_dir_open (dir, 0, NULL);
  while (contents != NULL && (name = g_dir_read_name (contents)) != NULL)
    {
      g_autofree gchar *path = g_build_filename (dir, name, NULL);
      GStatBuf info;

      if (g_stat (path, &info) == 0 && now - info.st_mtime > 24 * 60 * 60)
        g_unlink (path);
    }

  return dir;
}

/* Picks a file name in @dir that is not taken yet. */
gchar *
screenshot_video_build_filename (const gchar *dir,
                                 const gchar *format)
{
  g_autofree gchar *timestamp = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gchar *path = NULL;
  guint i;

  g_mkdir_with_parents (dir, 0755);

  timestamp = g_date_time_format (now, "%Y-%m-%d %H-%M-%S");

  for (i = 0; path == NULL; i++)
    {
      g_autofree gchar *name = NULL;

      if (i == 0)
        /* translators: this is the name of the file that gets made up with
         * the videoshot. The first placeholder is a timestamp (e.g.
         * "2017-05-21 12-24-03"); the second placeholder is the file format
         * (e.g. "mp4").
         */
        name = g_strdup_printf (_("Videoshot from %s.%s"), timestamp, format);
      else
        /* translators: this is the name of the file that gets made up with
         * the videoshot when the simpler filename already exists. The first
         * and second placeholders are a timestamp and a counter to make it
         * unique; the third placeholder is the file format (e.g. "mp4").
         */
        name = g_strdup_printf (_("Videoshot from %s - %d.%s"), timestamp, i, format);

      path = g_build_filename (dir, name, NULL);
      if (g_file_test (path, G_FILE_TEST_EXISTS))
        g_clear_pointer (&path, g_free);
    }

  return path;
}

/* Returns the first frame of the video at @path, or NULL. */
GdkPixbuf *
screenshot_video_get_preview (const gchar *path)
{
  g_autoptr(GSubprocess) process = NULL;
  g_autoptr(GBytes) frame = NULL;
  g_autoptr(GInputStream) stream = NULL;

  process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                              G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                              NULL,
                              "ffmpeg", "-nostdin", "-loglevel", "error",
                              "-i", path, "-frames:v", "1",
                              "-f", "image2pipe", "-c:v", "png", "-",
                              NULL);
  if (process == NULL ||
      !g_subprocess_communicate (process, NULL, NULL, &frame, NULL, NULL) ||
      frame == NULL || g_bytes_get_size (frame) == 0)
    return NULL;

  stream = g_memory_input_stream_new_from_bytes (frame);

  return gdk_pixbuf_new_from_stream (stream, NULL, NULL);
}

static void
recording_free (Recording *recording)
{
  g_clear_object (&recording->process);
  g_clear_object (&recording->status_icon);
  g_free (recording->path);
  g_free (recording->part_path);
  g_free (recording);
}

static void
recording_finished_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  Recording *recording = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *messages = NULL;
  GStatBuf info;

  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL, &messages, &error);

  if (current == recording)
    current = NULL;

  /* A recording stopped early ends with a non-zero status but leaves a
   * finished file behind, so the file is what tells success from failure.
   */
  if (error == NULL &&
      (g_stat (recording->part_path, &info) != 0 || info.st_size == 0))
    {
      if (messages != NULL)
        g_strstrip (messages);

      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", (messages != NULL && *messages != '\0') ?
                         messages : _("The recorder did not produce a video"));
    }

  if (error == NULL &&
      g_rename (recording->part_path, recording->path) != 0)
    g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "%s", g_strerror (errno));

  if (error != NULL)
    g_unlink (recording->part_path);

  recording->callback (error == NULL ? recording->path : NULL, error, recording->user_data);

  recording_free (recording);
}

static void
status_icon_activate_cb (GtkStatusIcon *status_icon,
                         gpointer       user_data)
{
  screenshot_video_stop ();
}

/* Some distributions build ffmpeg without libx264. */
static gboolean
has_libx264 (void)
{
  static gsize checked = 0;
  static gboolean found = FALSE;

  if (g_once_init_enter (&checked))
    {
      g_autoptr(GSubprocess) process = NULL;
      g_autofree gchar *encoders = NULL;

      process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                  G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                  NULL,
                                  "ffmpeg", "-hide_banner", "-encoders", NULL);
      if (process != NULL &&
          g_subprocess_communicate_utf8 (process, NULL, NULL, &encoders, NULL, NULL) &&
          encoders != NULL)
        found = (strstr (encoders, " libx264 ") != NULL);

      g_once_init_leave (&checked, 1);
    }

  return found;
}

static void
add_format_arguments (GPtrArray   *argv,
                      const gchar *format)
{
  const gchar *muxer = format;

  if (g_strcmp0 (format, MASTER_FORMAT) == 0)
    {
      /* lossless, fast, and part of every ffmpeg build */
      g_ptr_array_add (argv, g_strdup ("-c:v"));
      g_ptr_array_add (argv, g_strdup ("utvideo"));
      g_ptr_array_add (argv, g_strdup ("-pix_fmt"));
      g_ptr_array_add (argv, g_strdup ("gbrp"));
      muxer = "matroska";
    }
  else if (g_strcmp0 (format, "gif") == 0)
    {
      g_ptr_array_add (argv, g_strdup ("-vf"));
      g_ptr_array_add (argv, g_strdup ("fps=" GIF_FRAMERATE ",split[a][b];[a]palettegen=stats_mode=diff[p];"
                                       "[b][p]paletteuse=dither=bayer:diff_mode=rectangle"));
    }
  else
    {
      /* the video encoders want even dimensions */
      g_ptr_array_add (argv, g_strdup ("-vf"));
      g_ptr_array_add (argv, g_strdup ("crop=trunc(iw/2)*2:trunc(ih/2)*2"));
      g_ptr_array_add (argv, g_strdup ("-pix_fmt"));
      g_ptr_array_add (argv, g_strdup ("yuv420p"));
      g_ptr_array_add (argv, g_strdup ("-c:v"));

      if (g_strcmp0 (format, "webm") == 0)
        {
          g_ptr_array_add (argv, g_strdup ("libvpx-vp9"));
          g_ptr_array_add (argv, g_strdup ("-deadline"));
          g_ptr_array_add (argv, g_strdup ("realtime"));
          g_ptr_array_add (argv, g_strdup ("-cpu-used"));
          g_ptr_array_add (argv, g_strdup ("8"));
          g_ptr_array_add (argv, g_strdup ("-row-mt"));
          g_ptr_array_add (argv, g_strdup ("1"));
          g_ptr_array_add (argv, g_strdup ("-crf"));
          g_ptr_array_add (argv, g_strdup ("32"));
          g_ptr_array_add (argv, g_strdup ("-b:v"));
          g_ptr_array_add (argv, g_strdup ("0"));
        }
      else
        {
          if (has_libx264 ())
            {
              g_ptr_array_add (argv, g_strdup ("libx264"));
              g_ptr_array_add (argv, g_strdup ("-preset"));
              g_ptr_array_add (argv, g_strdup ("veryfast"));
              g_ptr_array_add (argv, g_strdup ("-crf"));
              g_ptr_array_add (argv, g_strdup ("20"));
            }
          else
            {
              /* what distributions ship when they leave libx264 out */
              g_ptr_array_add (argv, g_strdup ("libopenh264"));
              g_ptr_array_add (argv, g_strdup ("-b:v"));
              g_ptr_array_add (argv, g_strdup ("6M"));
            }

          if (g_strcmp0 (format, "mkv") == 0)
            muxer = "matroska";
          else
            {
              g_ptr_array_add (argv, g_strdup ("-movflags"));
              g_ptr_array_add (argv, g_strdup ("+faststart"));
            }
        }
    }

  /* the temporary file name hides the extension, so name the container */
  g_ptr_array_add (argv, g_strdup ("-f"));
  g_ptr_array_add (argv, g_strdup (muxer));
}

/* Runs ffmpeg with the given input arguments, writing @format to @path
 * through a temporary file next to it.
 */
static Recording *
run_ffmpeg (GPtrArray               *argv,
            const gchar             *format,
            const gchar             *path,
            ScreenshotVideoCallback  callback,
            gpointer                 user_data)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dirname = g_path_get_dirname (path);
  g_autofree gchar *basename = g_path_get_basename (path);
  g_autofree gchar *part_name = g_strdup_printf (".%s.part", basename);
  Recording *recording;

  recording = g_new0 (Recording, 1);
  recording->path = g_strdup (path);
  recording->part_path = g_build_filename (dirname, part_name, NULL);
  recording->callback = callback;
  recording->user_data = user_data;

  add_format_arguments (argv, format);
  g_ptr_array_add (argv, g_strdup (recording->part_path));
  g_ptr_array_add (argv, NULL);

  recording->process = g_subprocess_newv ((const gchar * const *) argv->pdata,
                                          G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                          G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                          &error);
  if (recording->process == NULL)
    {
      g_prefix_error (&error, _("Could not start ffmpeg: "));
      callback (NULL, error, user_data);
      recording_free (recording);
      return NULL;
    }

  g_subprocess_communicate_utf8_async (recording->process, NULL, NULL,
                                       recording_finished_cb, recording);

  return recording;
}

static GPtrArray *
new_ffmpeg_arguments (void)
{
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);

  g_ptr_array_add (argv, g_strdup ("ffmpeg"));
  g_ptr_array_add (argv, g_strdup ("-y"));
  g_ptr_array_add (argv, g_strdup ("-nostdin"));
  g_ptr_array_add (argv, g_strdup ("-hide_banner"));
  g_ptr_array_add (argv, g_strdup ("-loglevel"));
  g_ptr_array_add (argv, g_strdup ("error"));

  return argv;
}

static void
record_x11 (const GdkRectangle      *rect,
            guint                    length,
            gboolean                 include_pointer,
            const gchar             *framerate,
            const gchar             *format,
            const gchar             *path,
            ScreenshotVideoCallback  callback,
            gpointer                 user_data)
{
  g_autoptr(GPtrArray) argv = NULL;
  const gchar *display_name = gdk_display_get_name (gdk_display_get_default ());
  Recording *recording;

  g_return_if_fail (rect != NULL && callback != NULL);

  if (current != NULL)
    {
      g_autoptr(GError) error = NULL;

      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           _("A videoshot is being recorded already"));
      callback (NULL, error, user_data);
      return;
    }

  argv = new_ffmpeg_arguments ();
  g_ptr_array_add (argv, g_strdup ("-f"));
  g_ptr_array_add (argv, g_strdup ("x11grab"));
  g_ptr_array_add (argv, g_strdup ("-framerate"));
  g_ptr_array_add (argv, g_strdup (framerate));
  g_ptr_array_add (argv, g_strdup ("-draw_mouse"));
  g_ptr_array_add (argv, g_strdup (include_pointer ? "1" : "0"));
  g_ptr_array_add (argv, g_strdup ("-video_size"));
  g_ptr_array_add (argv, g_strdup_printf ("%dx%d", rect->width, rect->height));
  /* the length limits the input: the GIF palette is only made once the
   * input has ended, so limiting the output would never end it
   */
  g_ptr_array_add (argv, g_strdup ("-t"));
  g_ptr_array_add (argv, g_strdup_printf ("%u", length));
  g_ptr_array_add (argv, g_strdup ("-i"));
  g_ptr_array_add (argv, g_strdup_printf ("%s+%d,%d", display_name, rect->x, rect->y));

  recording = run_ffmpeg (argv, format, path, callback, user_data);
  if (recording == NULL)
    return;

  current = recording;

  /* Something to click on to stop early. It sits in the panel, so it is
   * only part of the video when the panel is.
   */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  recording->status_icon = gtk_status_icon_new_from_icon_name ("media-record");
  gtk_status_icon_set_title (recording->status_icon, _("Videoshot"));
  gtk_status_icon_set_tooltip_text (recording->status_icon,
                                    _("Recording a videoshot. Click to stop."));
G_GNUC_END_IGNORE_DEPRECATIONS
  g_signal_connect (recording->status_icon, "activate",
                    G_CALLBACK (status_icon_activate_cb), NULL);
}

typedef struct {
  gchar *master_path;
  gchar *path;
  ScreenshotVideoCallback callback;
  gpointer user_data;
} GifData;

static void
gif_data_free (GifData *data)
{
  g_unlink (data->master_path);
  g_free (data->master_path);
  g_free (data->path);
  g_free (data);
}

static void
gif_converted_cb (const gchar  *path,
                  const GError *error,
                  gpointer      user_data)
{
  GifData *data = user_data;

  data->callback (path, error, data->user_data);
  gif_data_free (data);
}

static void
gif_master_recorded_cb (const gchar  *path,
                        const GError *error,
                        gpointer      user_data)
{
  GifData *data = user_data;

  if (error != NULL)
    {
      data->callback (NULL, error, data->user_data);
      gif_data_free (data);
      return;
    }

  screenshot_video_convert_async (data->master_path, "gif", data->path,
                                  gif_converted_cb, data);
}

/* Records @rect (in device pixels) for @length seconds into @path. */
void
screenshot_video_record_async (const GdkRectangle      *rect,
                               guint                    length,
                               gboolean                 include_pointer,
                               const gchar             *format,
                               const gchar             *path,
                               ScreenshotVideoCallback  callback,
                               gpointer                 user_data)
{
  if (g_strcmp0 (format, "gif") == 0 && current == NULL)
    {
      GifData *data = g_new0 (GifData, 1);

      data->master_path = g_strdup_printf ("%s.master", path);
      data->path = g_strdup (path);
      data->callback = callback;
      data->user_data = user_data;

      record_x11 (rect, length, include_pointer, GIF_FRAMERATE,
                  MASTER_FORMAT, data->master_path,
                  gif_master_recorded_cb, data);
    }
  else
    record_x11 (rect, length, include_pointer, VIDEO_FRAMERATE,
                format, path, callback, user_data);
}

/* Writes the video at @source to @path in another @format. */
void
screenshot_video_convert_async (const gchar             *source,
                                const gchar             *format,
                                const gchar             *path,
                                ScreenshotVideoCallback  callback,
                                gpointer                 user_data)
{
  g_autoptr(GPtrArray) argv = new_ffmpeg_arguments ();

  g_ptr_array_add (argv, g_strdup ("-i"));
  g_ptr_array_add (argv, g_strdup (source));

  run_ffmpeg (argv, format, path, callback, user_data);
}

gboolean
screenshot_video_is_recording (void)
{
  return current != NULL;
}

/* Ends the running recording early; what was recorded so far is kept. */
void
screenshot_video_stop (void)
{
  if (current != NULL)
    g_subprocess_send_signal (current->process, SIGINT);
}
