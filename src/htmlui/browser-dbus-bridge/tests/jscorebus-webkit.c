/**
 * Browser D-Bus Bridge, JavaScriptCore version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Kalle Vahlman, <kalle.vahlman@movial.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */
 
/*
 * To build, execute this (on one line):
 *
 *   gcc -o jscorebus-webkit jscorebus-webkit.c \
 *       $(pkg-config --cflags --libs webkit-1.0 dbus-glib-1 jscorebus)
 *
 */
 
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <webkit/webkit.h>
#include <JavaScriptCore/JavaScript.h>
#include <jscorebus/jscorebus.h>

GtkWidget *view;

static
void _window_object_cleared (WebKitWebView* web_view,
                             WebKitWebFrame* frame,
                             JSGlobalContextRef context,
                             JSObjectRef windowObject,
                             gpointer user_data)
{
  jscorebus_export(context);
}

static gboolean
_window_delete_event(GtkWidget *widget, GdkEvent  *event, gpointer user_data)
{
  gtk_widget_destroy(widget);
  gtk_main_quit();
  return TRUE;
}

int main(int argc, char *argv[])
{
  GtkWidget *window;
  GtkWidget *swin;
  DBusConnection *session_connection;
  DBusConnection *system_connection;

  g_thread_init(NULL);
  gtk_init(&argc, &argv);

  if (argc < 2)
  {
    g_print("Usage: %s <url>\n", argv[0]);
    return(1);
  }
  
  session_connection = dbus_bus_get(DBUS_BUS_SESSION, NULL);
  system_connection = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);

  dbus_connection_setup_with_g_main(session_connection, NULL);
  dbus_connection_setup_with_g_main(system_connection, NULL);
  
  jscorebus_init(session_connection, system_connection);
  
  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  swin = gtk_scrolled_window_new(NULL, NULL);
  view = webkit_web_view_new();
  
  gtk_container_add(GTK_CONTAINER(swin), view);
  gtk_container_add(GTK_CONTAINER(window), swin);
  
  g_signal_connect(window, "delete-event",
                   G_CALLBACK(_window_delete_event), NULL);

  /***
   * Connect to the window-object-cleared signal. This is the moment when we
   * need to install the D-Bus bindings into the DOM.
   */
  g_signal_connect(view, "window-object-cleared",
                   G_CALLBACK(_window_object_cleared), session_connection);

  if (g_str_has_prefix("http://", argv[1])
   || g_str_has_prefix("https://", argv[1])
   || g_str_has_prefix("file://", argv[1]))
  {
    webkit_web_view_open(WEBKIT_WEB_VIEW(view), argv[1]);
  } else {
    gchar *url = NULL;
    if (g_path_is_absolute(argv[1])) {
      url = g_strjoin("", "file://", argv[1], NULL);
    } else {
      gchar *pwd = g_get_current_dir();
      url = g_strjoin("/", "file://", pwd, argv[1], NULL);
      g_free(pwd);
    }
    webkit_web_view_open(WEBKIT_WEB_VIEW(view), url);
    g_free(url);
  }

  gtk_widget_set_size_request(window,
                              640,
                              480);
  gtk_widget_show_all(window);
  gtk_main();

  return (0);
}

