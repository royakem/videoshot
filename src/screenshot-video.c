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
#include <fcntl.h>
#include <string.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-unix.h>

#include "screenshot-config.h"
#include "screenshot-portal.h"
#include "screenshot-video.h"

#ifdef HAVE_X11
#include "screenshot-backend-x11.h"
#endif

#define VIDEO_SAVE_DIRECTORY_KEY "video-save-directory"
#define VIDEO_FRAMERATE          "30"
#define GIF_FRAMERATE            "15"
#define CAST_SETTLE_TIME         600 /* milliseconds */

/* Lossless recording a GIF is made from, never shown to the user. */
#define MASTER_FORMAT            "master"

/* The recording is done by two child processes joined by a pipe. The first
 * writes plain frames: videoshot-grab reads them off the X screen, and on
 * Wayland gst-launch-1.0 takes them from the PipeWire stream the desktop
 * portal hands out. The second is ffmpeg, which encodes them.
 *
 * ffmpeg stops by itself after the requested length. Stopping early
 * interrupts the first process, so ffmpeg sees the end of its input and
 * finishes the file properly.
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
  GSubprocess *source;
  gchar *messages;
  gchar *source_messages;
  GError *error;
  guint pending;
  gboolean portal;
  gchar *path;
  gchar *part_path;
  GtkStatusIcon *status_icon;
  ScreenshotVideoCallback callback;
  gpointer user_data;
} Recording;

static Recording *current;
static gboolean starting;

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
  if (!screenshot_portal_is_wayland ())
    return screenshot_backend_x11_get_target_rect (selection, rect_out);
#endif

  return FALSE;
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
  g_clear_object (&recording->source);
  g_clear_object (&recording->status_icon);
  g_clear_error (&recording->error);
  g_free (recording->messages);
  g_free (recording->source_messages);
  g_free (recording->path);
  g_free (recording->part_path);
  g_free (recording);
}

static void
recording_done (Recording *recording)
{
  g_autoptr(GError) error = g_steal_pointer (&recording->error);
  GStatBuf info;

  if (current == recording)
    current = NULL;

  if (recording->portal)
    screenshot_portal_cast_close ();

  /* A recording stopped early ends with a non-zero status but leaves a
   * finished file behind, so the file is what tells success from failure.
   */
  if (error == NULL &&
      (g_stat (recording->part_path, &info) != 0 || info.st_size == 0))
    {
      const gchar *messages = recording->messages;

      /* without frames ffmpeg has little to say; their source knows why */
      if (recording->source_messages != NULL && *recording->source_messages != '\0')
        messages = recording->source_messages;

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
recording_finished_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  Recording *recording = user_data;

  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL,
                                        &recording->messages, &recording->error);
  if (recording->messages != NULL)
    g_strstrip (recording->messages);

  /* nobody reads the frames any more */
  if (recording->source != NULL)
    g_subprocess_send_signal (recording->source, SIGTERM);

  if (--recording->pending == 0)
    recording_done (recording);
}

static void
source_finished_cb (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  Recording *recording = user_data;

  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, NULL,
                                        &recording->source_messages, NULL);
  if (recording->source_messages != NULL)
    g_strstrip (recording->source_messages);

  if (--recording->pending == 0)
    recording_done (recording);
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
 *
 * With @source_argv that program is run as well, and what it writes is
 * what ffmpeg reads as its standard input. Without @source_fd it writes to
 * its standard output. Else @source_fd is handed to it as file descriptor
 * 3 and it writes to file descriptor 4, as gst-launch-1.0 has things of
 * its own to say on its standard output.
 */
static Recording *
run_ffmpeg (GPtrArray               *argv,
            GPtrArray               *source_argv,
            gint                     source_fd,
            const gchar             *format,
            const gchar             *path,
            ScreenshotVideoCallback  callback,
            gpointer                 user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL, source_launcher = NULL;
  g_autofree gchar *dirname = g_path_get_dirname (path);
  g_autofree gchar *basename = g_path_get_basename (path);
  g_autofree gchar *part_name = g_strdup_printf (".%s.part", basename);
  Recording *recording;

  recording = g_new0 (Recording, 1);
  recording->path = g_strdup (path);
  recording->part_path = g_build_filename (dirname, part_name, NULL);
  recording->callback = callback;
  recording->user_data = user_data;
  recording->portal = (source_fd != -1);

  add_format_arguments (argv, format);
  g_ptr_array_add (argv, g_strdup (recording->part_path));
  g_ptr_array_add (argv, NULL);

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                        G_SUBPROCESS_FLAGS_STDERR_PIPE);

  if (source_argv != NULL)
    {
      gint fds[2];

      g_ptr_array_add (source_argv, NULL);

      source_launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                                   (source_fd != -1 ?
                                                    G_SUBPROCESS_FLAGS_STDOUT_SILENCE : 0));
      if (source_fd != -1)
        g_subprocess_launcher_take_fd (source_launcher, source_fd, 3);

      if (g_unix_open_pipe (fds, FD_CLOEXEC, &error))
        {
          if (source_fd != -1)
            g_subprocess_launcher_take_fd (source_launcher, fds[1], 4);
          else
            g_subprocess_launcher_take_stdout_fd (source_launcher, fds[1]);

          g_subprocess_launcher_take_stdin_fd (launcher, fds[0]);

          recording->source = g_subprocess_launcher_spawnv (source_launcher,
                                                            (const gchar * const *) source_argv->pdata,
                                                            &error);
        }

      /* the launcher holds the writing end of the pipe: let go of it, or
       * ffmpeg would never see the end of the frames
       */
      g_clear_object (&source_launcher);

      if (recording->source == NULL)
        {
          g_prefix_error (&error, _("Could not start %s: "),
                          (const gchar *) g_ptr_array_index (source_argv, 0));
          callback (NULL, error, user_data);
          recording_free (recording);
          return NULL;
        }
    }

  recording->process = g_subprocess_launcher_spawnv (launcher,
                                                     (const gchar * const *) argv->pdata,
                                                     &error);
  if (recording->process == NULL)
    {
      if (recording->source != NULL)
        g_subprocess_force_exit (recording->source);

      g_prefix_error (&error, _("Could not start ffmpeg: "));
      callback (NULL, error, user_data);
      recording_free (recording);
      return NULL;
    }

  recording->pending = 1;
  g_subprocess_communicate_utf8_async (recording->process, NULL, NULL,
                                       recording_finished_cb, recording);

  if (recording->source != NULL)
    {
      recording->pending++;
      g_subprocess_communicate_utf8_async (recording->source, NULL, NULL,
                                           source_finished_cb, recording);
    }

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

/* Records @rect of the X screen or, when that is NULL, @node of the
 * PipeWire connection @pipewire_fd.
 */
static void
record (const GdkRectangle      *rect,
        gint                     pipewire_fd,
        guint32                  node,
        guint                    length,
        gboolean                 include_pointer,
        const gchar             *framerate,
        const gchar             *format,
        const gchar             *path,
        ScreenshotVideoCallback  callback,
        gpointer                 user_data)
{
  g_autoptr(GPtrArray) argv = new_ffmpeg_arguments ();
  g_autoptr(GPtrArray) source_argv = g_ptr_array_new_with_free_func (g_free);
  Recording *recording;

  if (rect != NULL)
    {
      const gchar *grabber = g_getenv ("VIDEOSHOT_GRAB");

      g_ptr_array_add (source_argv, g_strdup (grabber != NULL ? grabber : LIBEXECDIR "/videoshot-grab"));
      g_ptr_array_add (source_argv, g_strdup_printf ("%d", rect->x));
      g_ptr_array_add (source_argv, g_strdup_printf ("%d", rect->y));
      g_ptr_array_add (source_argv, g_strdup_printf ("%d", rect->width));
      g_ptr_array_add (source_argv, g_strdup_printf ("%d", rect->height));
      g_ptr_array_add (source_argv, g_strdup (framerate));
      g_ptr_array_add (source_argv, g_strdup (include_pointer ? "1" : "0"));

      g_ptr_array_add (argv, g_strdup ("-f"));
      g_ptr_array_add (argv, g_strdup ("rawvideo"));
      g_ptr_array_add (argv, g_strdup ("-pixel_format"));
      g_ptr_array_add (argv, g_strdup ("bgr0"));
      g_ptr_array_add (argv, g_strdup ("-video_size"));
      g_ptr_array_add (argv, g_strdup_printf ("%dx%d", rect->width, rect->height));
      g_ptr_array_add (argv, g_strdup ("-framerate"));
      g_ptr_array_add (argv, g_strdup (framerate));
    }
  else
    {
      /* The desktop only sends a frame when the picture changes; the
       * keepalive repeats the last one, and videorate makes the steady
       * rate the length is counted in.
       */
      g_ptr_array_add (source_argv, g_strdup ("gst-launch-1.0"));
      g_ptr_array_add (source_argv, g_strdup ("-q"));
      g_ptr_array_add (source_argv, g_strdup ("-e"));
      g_ptr_array_add (source_argv, g_strdup ("pipewiresrc"));
      g_ptr_array_add (source_argv, g_strdup ("fd=3"));
      g_ptr_array_add (source_argv, g_strdup_printf ("path=%u", node));
      g_ptr_array_add (source_argv, g_strdup ("do-timestamp=true"));
      g_ptr_array_add (source_argv, g_strdup ("keepalive-time=30"));
      g_ptr_array_add (source_argv, g_strdup ("always-copy=true"));
      g_ptr_array_add (source_argv, g_strdup ("!"));
      g_ptr_array_add (source_argv, g_strdup ("videoconvert"));
      g_ptr_array_add (source_argv, g_strdup ("!"));
      g_ptr_array_add (source_argv, g_strdup ("videorate"));
      g_ptr_array_add (source_argv, g_strdup ("!"));
      g_ptr_array_add (source_argv, g_strdup_printf ("video/x-raw,format=I420,framerate=%s/1", framerate));
      g_ptr_array_add (source_argv, g_strdup ("!"));
      g_ptr_array_add (source_argv, g_strdup ("y4menc"));
      g_ptr_array_add (source_argv, g_strdup ("!"));
      g_ptr_array_add (source_argv, g_strdup ("fdsink"));
      g_ptr_array_add (source_argv, g_strdup ("fd=4"));
      g_ptr_array_add (source_argv, g_strdup ("sync=false"));

      g_ptr_array_add (argv, g_strdup ("-f"));
      g_ptr_array_add (argv, g_strdup ("yuv4mpegpipe"));
    }

  /* the length limits the input: the GIF palette is only made once the
   * input has ended, so limiting the output would never end it
   */
  g_ptr_array_add (argv, g_strdup ("-t"));
  g_ptr_array_add (argv, g_strdup_printf ("%u", length));
  g_ptr_array_add (argv, g_strdup ("-i"));
  g_ptr_array_add (argv, g_strdup ("pipe:0"));

  recording = run_ffmpeg (argv, source_argv, rect != NULL ? -1 : pipewire_fd,
                          format, path, callback, user_data);
  if (recording == NULL)
    {
      if (rect == NULL)
        screenshot_portal_cast_close ();

      return;
    }

  current = recording;

  /* Something to click on to stop early. It sits in the panel, so it is
   * only part of the video when the panel is. On Wayland the desktop shows
   * an indicator of its own while the screen is shared.
   */
  if (rect != NULL)
    {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      recording->status_icon = gtk_status_icon_new_from_icon_name ("media-record");
      gtk_status_icon_set_title (recording->status_icon, _("Videoshot"));
      gtk_status_icon_set_tooltip_text (recording->status_icon,
                                        _("Recording a videoshot. Click to stop."));
G_GNUC_END_IGNORE_DEPRECATIONS
      g_signal_connect (recording->status_icon, "activate",
                        G_CALLBACK (status_icon_activate_cb), NULL);
    }
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

typedef struct {
  gboolean has_rect;
  GdkRectangle rect;
  guint length;
  gboolean include_pointer;
  gchar *format;
  gchar *path;
  gint pipewire_fd;
  guint32 node;
  ScreenshotVideoCallback callback;
  gpointer user_data;
} Job;

static void
job_free (Job *job)
{
  g_free (job->format);
  g_free (job->path);
  g_free (job);
}

static void
job_start (Job     *job,
           gint     pipewire_fd,
           guint32  node)
{
  const GdkRectangle *rect = job->has_rect ? &job->rect : NULL;

  if (g_strcmp0 (job->format, "gif") == 0)
    {
      GifData *data = g_new0 (GifData, 1);

      data->master_path = g_strdup_printf ("%s.master", job->path);
      data->path = g_strdup (job->path);
      data->callback = job->callback;
      data->user_data = job->user_data;

      record (rect, pipewire_fd, node, job->length, job->include_pointer,
              GIF_FRAMERATE, MASTER_FORMAT, data->master_path,
              gif_master_recorded_cb, data);
    }
  else
    record (rect, pipewire_fd, node, job->length, job->include_pointer,
            VIDEO_FRAMERATE, job->format, job->path,
            job->callback, job->user_data);

  job_free (job);
}

static gboolean
job_cast_settled_cb (gpointer user_data)
{
  Job *job = user_data;

  starting = FALSE;
  job_start (job, job->pipewire_fd, job->node);

  return G_SOURCE_REMOVE;
}

static void
job_cast_ready_cb (gint          pipewire_fd,
                   guint32       node,
                   const GError *error,
                   gpointer      user_data)
{
  Job *job = user_data;

  if (error != NULL)
    {
      starting = FALSE;
      job->callback (NULL, error, job->user_data);
      job_free (job);
      return;
    }

  /* give the desktop's question the time to leave the screen */
  job->pipewire_fd = pipewire_fd;
  job->node = node;
  g_timeout_add (CAST_SETTLE_TIME, job_cast_settled_cb, job);
}

/* Records @rect (in device pixels) for @length seconds into @path. Without
 * @rect the desktop asks the user what to record, which is the only way
 * there is on Wayland.
 */
void
screenshot_video_record_async (const GdkRectangle      *rect,
                               guint                    length,
                               gboolean                 include_pointer,
                               const gchar             *format,
                               const gchar             *path,
                               ScreenshotVideoCallback  callback,
                               gpointer                 user_data)
{
  Job *job;

  if (current != NULL || starting)
    {
      g_autoptr(GError) error = NULL;

      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           _("A videoshot is being recorded already"));
      callback (NULL, error, user_data);
      return;
    }

  job = g_new0 (Job, 1);
  job->length = length;
  job->include_pointer = include_pointer;
  job->format = g_strdup (format);
  job->path = g_strdup (path);
  job->callback = callback;
  job->user_data = user_data;

  if (rect != NULL)
    {
      job->has_rect = TRUE;
      job->rect = *rect;
      job_start (job, -1, 0);
    }
  else
    {
      starting = TRUE;
      screenshot_portal_cast_start (include_pointer, job_cast_ready_cb, job);
    }
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

  run_ffmpeg (argv, NULL, -1, format, path, callback, user_data);
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
  if (current == NULL)
    return;

  /* without frames coming ffmpeg finishes the file by itself */
  g_subprocess_send_signal (current->source != NULL ? current->source : current->process,
                            SIGINT);
}
