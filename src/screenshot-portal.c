/* screenshot-portal.c - screenshots and screen casts through the desktop portal
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

#include "config.h"

#include <string.h>
#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>

#include "screenshot-portal.h"

/* On Wayland a program cannot look at the screen by itself. It asks the
 * desktop portal, the desktop asks the user what to share, and the picture
 * arrives as a file (screenshot) or as a PipeWire stream (screen cast).
 *
 * Every portal call answers twice: at once with the path of a request
 * object, and later with a Response signal on that object.
 */

#define PORTAL_NAME        "org.freedesktop.portal.Desktop"
#define PORTAL_PATH        "/org/freedesktop/portal/desktop"
#define REQUEST_INTERFACE  "org.freedesktop.portal.Request"
#define SESSION_INTERFACE  "org.freedesktop.portal.Session"
#define CAST_INTERFACE     "org.freedesktop.portal.ScreenCast"
#define SHOT_INTERFACE     "org.freedesktop.portal.Screenshot"

#define SOURCE_MONITOR     1
#define SOURCE_WINDOW      2
#define CURSOR_HIDDEN      1
#define CURSOR_EMBEDDED    2

typedef void (* ResponseFunc) (GVariant     *results,
                               const GError *error,
                               gpointer      user_data);

typedef struct {
  GDBusConnection *connection;
  guint signal_id;
  ResponseFunc callback;
  gpointer user_data;
} Request;

static gchar *cast_session;

gboolean
screenshot_portal_is_wayland (void)
{
  const gchar *session_type = g_getenv ("XDG_SESSION_TYPE");

  if (session_type != NULL && *session_type != '\0')
    return g_strcmp0 (session_type, "wayland") == 0;

  return g_getenv ("WAYLAND_DISPLAY") != NULL;
}

static GDBusConnection *
get_connection (void)
{
  GApplication *application = g_application_get_default ();
  GDBusConnection *connection = NULL;

  if (application != NULL)
    connection = g_application_get_dbus_connection (application);

  if (connection == NULL)
    connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  return connection;
}

static void
request_finish (Request      *request,
                GVariant     *results,
                const GError *error)
{
  if (request->signal_id != 0)
    g_dbus_connection_signal_unsubscribe (request->connection, request->signal_id);

  request->callback (results, error, request->user_data);

  g_object_unref (request->connection);
  g_free (request);
}

static void
request_response_cb (GDBusConnection *connection,
                     const gchar     *sender_name,
                     const gchar     *object_path,
                     const gchar     *interface_name,
                     const gchar     *signal_name,
                     GVariant        *parameters,
                     gpointer         user_data)
{
  Request *request = user_data;
  g_autoptr(GVariant) results = NULL;
  g_autoptr(GError) error = NULL;
  guint32 response;

  g_variant_get (parameters, "(u@a{sv})", &response, &results);

  if (response == 1)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         _("Cancelled"));
  else if (response != 0)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("The desktop did not allow it"));

  request_finish (request, error == NULL ? results : NULL, error);
}

static void
request_called_cb (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  Request *request = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);

  /* the answer proper comes with the Response signal */
  if (reply == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      request_finish (request, NULL, error);
    }
}

/* Calls @method of the portal's @interface. The arguments are, in order:
 * @first_argument unless it is NULL, an empty parent window when
 * @parent_window_argument is set, and @options, which are consumed.
 */
static void
portal_request (const gchar     *interface,
                const gchar     *method,
                GVariant        *first_argument,
                GVariantBuilder *options,
                gboolean         parent_window_argument,
                ResponseFunc     callback,
                gpointer         user_data)
{
  static guint counter = 0;
  g_autofree gchar *token = NULL, *sender = NULL, *path = NULL;
  GVariantBuilder parameters;
  Request *request;
  gchar *c;

  request = g_new0 (Request, 1);
  request->connection = g_object_ref (get_connection ());
  request->callback = callback;
  request->user_data = user_data;

  token = g_strdup_printf ("videoshot%u_%u", ++counter, g_random_int ());
  sender = g_strdup (g_dbus_connection_get_unique_name (request->connection) + 1);
  for (c = sender; *c != '\0'; c++)
    if (*c == '.')
      *c = '_';

  path = g_strdup_printf (PORTAL_PATH "/request/%s/%s", sender, token);

  request->signal_id =
    g_dbus_connection_signal_subscribe (request->connection,
                                        PORTAL_NAME, REQUEST_INTERFACE, "Response",
                                        path, NULL,
                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                        request_response_cb, request, NULL);

  g_variant_builder_add (options, "{sv}", "handle_token", g_variant_new_string (token));

  g_variant_builder_init (&parameters, G_VARIANT_TYPE_TUPLE);
  if (first_argument != NULL)
    g_variant_builder_add_value (&parameters, first_argument);
  if (parent_window_argument)
    g_variant_builder_add (&parameters, "s", "");
  g_variant_builder_add_value (&parameters, g_variant_builder_end (options));

  g_dbus_connection_call (request->connection,
                          PORTAL_NAME, PORTAL_PATH, interface, method,
                          g_variant_builder_end (&parameters),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                          request_called_cb, request);
}

/* Screen cast */

typedef struct {
  gboolean include_pointer;
  ScreenshotPortalCastCallback callback;
  gpointer user_data;
} Cast;

static void
cast_finish (Cast         *cast,
             gint          fd,
             guint32       node,
             const GError *error)
{
  if (error != NULL)
    screenshot_portal_cast_close ();

  cast->callback (fd, node, error, cast->user_data);
  g_free (cast);
}

static void
cast_remote_opened_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  Cast *cast = user_data;
  guint32 node = GPOINTER_TO_UINT (g_object_get_data (source, "videoshot-cast-node"));
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GUnixFDList) fds = NULL;
  g_autoptr(GError) error = NULL;
  gint handle, fd = -1;

  reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                           &fds, res, &error);
  if (reply != NULL)
    {
      g_variant_get (reply, "(h)", &handle);
      fd = g_unix_fd_list_get (fds, handle, &error);
    }

  if (error != NULL)
    g_dbus_error_strip_remote_error (error);

  cast_finish (cast, fd, node, error);
}

static void
cast_started_cb (GVariant     *results,
                 const GError *error,
                 gpointer      user_data)
{
  Cast *cast = user_data;
  g_autoptr(GVariant) streams = NULL;
  GVariantBuilder options;
  GDBusConnection *connection = get_connection ();
  guint32 node;

  if (error != NULL)
    {
      cast_finish (cast, -1, 0, error);
      return;
    }

  streams = g_variant_lookup_value (results, "streams", G_VARIANT_TYPE ("a(ua{sv})"));
  if (streams == NULL || g_variant_n_children (streams) == 0)
    {
      g_autoptr(GError) no_stream = NULL;

      g_set_error_literal (&no_stream, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("The desktop did not share anything"));
      cast_finish (cast, -1, 0, no_stream);
      return;
    }

  g_variant_get_child (streams, 0, "(u@a{sv})", &node, NULL);
  g_object_set_data (G_OBJECT (connection), "videoshot-cast-node", GUINT_TO_POINTER (node));

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_call_with_unix_fd_list (connection,
                                            PORTAL_NAME, PORTAL_PATH,
                                            CAST_INTERFACE, "OpenPipeWireRemote",
                                            g_variant_new ("(oa{sv})", cast_session, &options),
                                            G_VARIANT_TYPE ("(h)"),
                                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                                            cast_remote_opened_cb, cast);
}

static void
cast_sources_selected_cb (GVariant     *results,
                          const GError *error,
                          gpointer      user_data)
{
  Cast *cast = user_data;
  GVariantBuilder options;

  if (error != NULL)
    {
      cast_finish (cast, -1, 0, error);
      return;
    }

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  portal_request (CAST_INTERFACE, "Start",
                  g_variant_new_object_path (cast_session), &options, TRUE,
                  cast_started_cb, cast);
}

static guint32
get_cast_property (const gchar *name)
{
  g_autoptr(GVariant) reply = NULL, value = NULL;

  reply = g_dbus_connection_call_sync (get_connection (),
                                       PORTAL_NAME, PORTAL_PATH,
                                       "org.freedesktop.DBus.Properties", "Get",
                                       g_variant_new ("(ss)", CAST_INTERFACE, name),
                                       G_VARIANT_TYPE ("(v)"),
                                       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
  if (reply == NULL)
    return 0;

  g_variant_get (reply, "(v)", &value);

  return g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32 (value) : 0;
}

static void
cast_session_created_cb (GVariant     *results,
                         const GError *error,
                         gpointer      user_data)
{
  Cast *cast = user_data;
  GVariantBuilder options;
  guint32 sources, pointers, pointer;

  if (error != NULL)
    {
      cast_finish (cast, -1, 0, error);
      return;
    }

  g_clear_pointer (&cast_session, g_free);
  g_variant_lookup (results, "session_handle", "s", &cast_session);
  if (cast_session == NULL)
    {
      g_autoptr(GError) no_session = NULL;

      g_set_error_literal (&no_session, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("The desktop did not start a screen cast"));
      cast_finish (cast, -1, 0, no_session);
      return;
    }

  sources = get_cast_property ("AvailableSourceTypes") & (SOURCE_MONITOR | SOURCE_WINDOW);
  pointers = get_cast_property ("AvailableCursorModes");
  pointer = cast->include_pointer ? CURSOR_EMBEDDED : CURSOR_HIDDEN;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "multiple", g_variant_new_boolean (FALSE));
  if (sources != 0)
    g_variant_builder_add (&options, "{sv}", "types", g_variant_new_uint32 (sources));
  if ((pointers & pointer) != 0)
    g_variant_builder_add (&options, "{sv}", "cursor_mode", g_variant_new_uint32 (pointer));

  portal_request (CAST_INTERFACE, "SelectSources",
                  g_variant_new_object_path (cast_session), &options, FALSE,
                  cast_sources_selected_cb, cast);
}

/* Asks the user what to record. The callback gets a connection to PipeWire
 * and the node to record from it; the cast lasts until
 * screenshot_portal_cast_close().
 */
void
screenshot_portal_cast_start (gboolean                      include_pointer,
                              ScreenshotPortalCastCallback  callback,
                              gpointer                      user_data)
{
  GVariantBuilder options;
  g_autofree gchar *token = NULL;
  Cast *cast;

  screenshot_portal_cast_close ();

  cast = g_new0 (Cast, 1);
  cast->include_pointer = include_pointer;
  cast->callback = callback;
  cast->user_data = user_data;

  token = g_strdup_printf ("videoshot_%u", g_random_int ());

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "session_handle_token", g_variant_new_string (token));

  portal_request (CAST_INTERFACE, "CreateSession", NULL, &options, FALSE,
                  cast_session_created_cb, cast);
}

void
screenshot_portal_cast_close (void)
{
  if (cast_session == NULL)
    return;

  g_dbus_connection_call (get_connection (),
                          PORTAL_NAME, cast_session, SESSION_INTERFACE, "Close",
                          NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
  g_clear_pointer (&cast_session, g_free);
}

/* Screenshot */

typedef struct {
  GMainLoop *loop;
  gchar *uri;
  GError *error;
} Shot;

static void
shot_taken_cb (GVariant     *results,
               const GError *error,
               gpointer      user_data)
{
  Shot *shot = user_data;

  if (error != NULL)
    shot->error = g_error_copy (error);
  else
    g_variant_lookup (results, "uri", "s", &shot->uri);

  g_main_loop_quit (shot->loop);
}

/* Takes a screenshot; with @interactive the desktop lets the user choose
 * what goes in it. Returns when the desktop is done.
 */
GdkPixbuf *
screenshot_portal_take_screenshot (gboolean   interactive,
                                   GError   **error)
{
  GVariantBuilder options;
  GdkPixbuf *pixbuf = NULL;
  Shot shot = { NULL, NULL, NULL };

  shot.loop = g_main_loop_new (NULL, FALSE);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "interactive", g_variant_new_boolean (interactive));

  portal_request (SHOT_INTERFACE, "Screenshot", NULL, &options, TRUE,
                  shot_taken_cb, &shot);

  g_main_loop_run (shot.loop);
  g_main_loop_unref (shot.loop);

  if (shot.uri != NULL)
    {
      g_autoptr(GFile) file = g_file_new_for_uri (shot.uri);
      g_autofree gchar *path = g_file_get_path (file);

      if (path != NULL)
        {
          pixbuf = gdk_pixbuf_new_from_file (path, error);
          /* the desktop saved a copy of its own; ours is what the user keeps */
          g_file_delete (file, NULL, NULL);
        }
    }
  else if (shot.error != NULL)
    g_propagate_error (error, g_steal_pointer (&shot.error));

  g_clear_error (&shot.error);
  g_free (shot.uri);

  return pixbuf;
}
