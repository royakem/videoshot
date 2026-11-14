/* screenshot-portal.h - screenshots and screen casts through the desktop portal
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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef void (* ScreenshotPortalCastCallback) (gint          pipewire_fd,
                                               guint32       node,
                                               const GError *error,
                                               gpointer      user_data);

gboolean   screenshot_portal_is_wayland      (void);

void       screenshot_portal_cast_start      (gboolean                      include_pointer,
                                              ScreenshotPortalCastCallback  callback,
                                              gpointer                      user_data);
void       screenshot_portal_cast_close      (void);

GdkPixbuf *screenshot_portal_take_screenshot (gboolean   interactive,
                                              GError   **error);

G_END_DECLS
