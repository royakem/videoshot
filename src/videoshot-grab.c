/* videoshot-grab.c - writes frames of a part of the X screen to stdout
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA
 */

/* Usage: videoshot-grab X Y WIDTH HEIGHT FRAMERATE POINTER
 *
 * Writes raw frames (four bytes a pixel: blue, green, red, unused) at a
 * steady rate until it is interrupted or nobody reads them any more.
 *
 * With a compositing window manager the screen can be read while it is
 * half painted: the wallpaper shows where a window has yet to be drawn.
 * Such a state lasts a moment only. So every frame is read until two reads
 * in a row are the same, which a half painted screen does not manage.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>

#define MAX_READS 4

typedef struct {
  XImage *image;
  XShmSegmentInfo segment;
} Buffer;

static volatile sig_atomic_t stopped = 0;
static int use_shm = 0;

static void
stop_handler (int signal_number)
{
  stopped = 1;
}

static int
buffer_init (Display *display,
             Buffer  *buffer,
             int      width,
             int      height)
{
  int screen = DefaultScreen (display);
  Visual *visual = DefaultVisual (display, screen);
  int depth = DefaultDepth (display, screen);

  if (use_shm)
    {
      buffer->image = XShmCreateImage (display, visual, depth, ZPixmap, NULL,
                                       &buffer->segment, width, height);
      if (buffer->image == NULL)
        return 0;

      buffer->segment.shmid = shmget (IPC_PRIVATE,
                                      (size_t) buffer->image->bytes_per_line * height,
                                      IPC_CREAT | 0600);
      if (buffer->segment.shmid < 0)
        return 0;

      buffer->segment.shmaddr = buffer->image->data = shmat (buffer->segment.shmid, NULL, 0);
      buffer->segment.readOnly = False;
      XShmAttach (display, &buffer->segment);
      XSync (display, False);
      /* gone as soon as both sides let go of it */
      shmctl (buffer->segment.shmid, IPC_RMID, NULL);

      return buffer->image->data != (char *) -1;
    }

  buffer->image = XCreateImage (display, visual, depth, ZPixmap, 0, NULL,
                                width, height, 32, 0);
  if (buffer->image == NULL)
    return 0;

  buffer->image->data = malloc ((size_t) buffer->image->bytes_per_line * height);

  return buffer->image->data != NULL;
}

static void
buffer_read (Display *display,
             Window   root,
             Buffer  *buffer,
             int      x,
             int      y)
{
  if (use_shm)
    XShmGetImage (display, root, buffer->image, x, y, AllPlanes);
  else
    XGetSubImage (display, root, x, y,
                  buffer->image->width, buffer->image->height,
                  AllPlanes, ZPixmap, buffer->image, 0, 0);
}

/* Draws the pointer over the frame. */
static void
draw_pointer (Display *display,
              XImage  *image,
              int      x,
              int      y)
{
  XFixesCursorImage *cursor = XFixesGetCursorImage (display);
  int row, column;

  if (cursor == NULL)
    return;

  for (row = 0; row < cursor->height; row++)
    {
      int target_y = cursor->y - cursor->yhot + row - y;

      if (target_y < 0 || target_y >= image->height)
        continue;

      for (column = 0; column < cursor->width; column++)
        {
          int target_x = cursor->x - cursor->xhot + column - x;
          unsigned long pixel = cursor->pixels[row * cursor->width + column];
          unsigned int alpha = (pixel >> 24) & 0xff;
          unsigned char *target;
          int channel;

          if (target_x < 0 || target_x >= image->width || alpha == 0)
            continue;

          target = (unsigned char *) image->data +
                   (size_t) target_y * image->bytes_per_line + target_x * 4;

          /* the pointer's colours come multiplied with its alpha */
          for (channel = 0; channel < 3; channel++)
            {
              unsigned int colour = (pixel >> (channel * 8)) & 0xff;
              unsigned int mixed = colour + target[channel] * (255 - alpha) / 255;

              target[channel] = mixed > 255 ? 255 : mixed;
            }
        }
    }

  XFree (cursor);
}

static int
write_all (const char *data,
           size_t      size)
{
  while (size > 0)
    {
      ssize_t written = write (STDOUT_FILENO, data, size);

      if (written <= 0)
        return 0;

      data += written;
      size -= written;
    }

  return 1;
}

int
main (int    argc,
      char **argv)
{
  Display *display;
  Window root;
  Buffer buffers[2];
  struct sigaction action;
  struct timespec next;
  int x, y, width, height, framerate, pointer;
  int screen_width, screen_height, unused;
  size_t size;

  if (argc != 7)
    {
      fprintf (stderr, "Usage: %s X Y WIDTH HEIGHT FRAMERATE POINTER\n", argv[0]);
      return 2;
    }

  x = atoi (argv[1]);
  y = atoi (argv[2]);
  width = atoi (argv[3]);
  height = atoi (argv[4]);
  framerate = atoi (argv[5]);
  pointer = atoi (argv[6]);

  display = XOpenDisplay (NULL);
  if (display == NULL)
    {
      fprintf (stderr, "Could not open the X display\n");
      return 1;
    }

  root = DefaultRootWindow (display);
  screen_width = DisplayWidth (display, DefaultScreen (display));
  screen_height = DisplayHeight (display, DefaultScreen (display));

  if (framerate < 1 || width < 1 || height < 1 || x < 0 || y < 0 ||
      x + width > screen_width || y + height > screen_height)
    {
      fprintf (stderr, "The area %dx%d+%d+%d is not on the %dx%d screen\n",
               width, height, x, y, screen_width, screen_height);
      return 2;
    }

  use_shm = XShmQueryExtension (display);
  if (pointer && !XFixesQueryExtension (display, &unused, &unused))
    pointer = 0;

  if (!buffer_init (display, &buffers[0], width, height) ||
      !buffer_init (display, &buffers[1], width, height))
    {
      fprintf (stderr, "Could not make room for the frames\n");
      return 1;
    }

  if (buffers[0].image->bits_per_pixel != 32)
    {
      fprintf (stderr, "Screens with %d bits a pixel are not supported\n",
               buffers[0].image->bits_per_pixel);
      return 1;
    }

  size = (size_t) buffers[0].image->bytes_per_line * height;

  memset (&action, 0, sizeof action);
  action.sa_handler = stop_handler;
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGTERM, &action, NULL);
  signal (SIGPIPE, SIG_IGN);

  clock_gettime (CLOCK_MONOTONIC, &next);

  while (!stopped)
    {
      int current = 0;
      int reads;

      buffer_read (display, root, &buffers[0], x, y);

      for (reads = 1; reads < MAX_READS; reads++)
        {
          current = !current;
          buffer_read (display, root, &buffers[current], x, y);

          if (memcmp (buffers[0].image->data, buffers[1].image->data, size) == 0)
            break;
        }

      if (pointer)
        draw_pointer (display, buffers[current].image, x, y);

      if (!write_all (buffers[current].image->data, size))
        break;

      next.tv_nsec += 1000000000L / framerate;
      if (next.tv_nsec >= 1000000000L)
        {
          next.tv_nsec -= 1000000000L;
          next.tv_sec++;
        }

      clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

  XCloseDisplay (display);

  return 0;
}
