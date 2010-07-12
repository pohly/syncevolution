#!/usr/bin/python

import dbus
import dbus.service
import dbus.glib
import gconf
import gettext

import gobject
import gtk
import gtk.glade
import gnome.ui

import locale
import os
import pynotify
import signal
import subprocess
import sys

APPNAME             = 'Syncevolution-http-server'
APP_VERSION         = '0.1'
SYNCEVO_HTTP_SERVER = "/usr/bin/syncevo-http-server"
DEFAULT_HTTP_URL    = "http://localhost:8765/"
DBUS_OBJECT_PATH    = "/com/meego/syncevolution/ui"
DBUS_SERVICE_NAME   = "com.meego.syncevolution"
GCONF_URL_KEY       = "/apps/syncevolution/http_url"
ABOUT_LONG          = \
"""A HTTP server synchronizing  personal data such
 as contacts and calendars using the SyncML open protocol."""

#BEWARE! Following line is patched by installation scripts.
DATADIR             = "."
RUNNING_ICON        = DATADIR + "/" + "sync-running.png"
STOPPED_ICON        = DATADIR + "/" + "sync-stopped.png"
GLADE_FILE          = DATADIR + "/" + "syncevo-http-trayicon.glade"

locale.setlocale( locale.LC_ALL, "")
locale.textdomain( 'syncevo-http-ui')
gettext.textdomain( 'syncevo-http-ui')
_ = gettext.gettext

class HttpServerStatusIcon(gtk.StatusIcon):
    """
    Exposes http server status (running/not running) and a menu
    for server management.
    """

    def __init__(self):

        gnome.init( APPNAME, APP_VERSION)
        gtk.StatusIcon.__init__(self)

        self.server = None
        self.gconf_client = gconf.client_get_default()

        self.w_tree = gtk.glade.XML( GLADE_FILE)
        self.menu = self.w_tree.get_widget( "popup_menu")
        self.preferences_dialog = self.w_tree.get_widget("preferences-dialog")
        self.preferences_dialog.connect("destroy", self.on_prefs_death)
        self.connect('popup-menu', self.on_popup_menu)
        self.connect('activate', self.on_activate )
        self.connect("button_press_event", self.button_press_event )
        self.w_tree.signal_autoconnect( self)
        self._stop_ui()
        self.set_visible(True)

    def _start_ui(self, url):

        self.w_tree.get_widget('stop_menu_item').set_sensitive( True)
        self.w_tree.get_widget('start_menu_item').set_sensitive( False)
        self.set_from_file( RUNNING_ICON)
        self.set_tooltip(_("Server running at %s") % url)

    def _stop_ui(self):

        self.w_tree.get_widget('stop_menu_item').set_sensitive( False)
        self.w_tree.get_widget('start_menu_item').set_sensitive( True)
        self.set_from_file( STOPPED_ICON )
        self.set_tooltip(_("Server is stopped."))

    def on_sigchld(self, signum = None, frame = None):

        self.server = None
        self._stop_ui()

    def button_press_event(self, widget, event):

        self.last_button_clicked = event.button
        self.last_button_clicked_time = event.get_time()

    def on_activate(self, statusIcon):

        self.menu.popup(None,
                        None,
                        None,
                        self.last_button_clicked,
                        self.last_button_clicked_time)

    def on_popup_menu(self, status, button, time):
        self.menu.popup(None, None, None, button, time)

    def on_quit_activate(self, data):

        self.on_stop_activate()
        gtk.main_quit()

    def on_properties_activate(self, data):

        url = self.gconf_client.get_string( GCONF_URL_KEY)
        self.w_tree.get_widget( "url_entry").set_text( url)
        self.preferences_dialog.show()

    def on_prefs_ok_btn_clicked(self, widget):

        url = self.w_tree.get_widget("url_entry").get_text()
        self.gconf_client.set_string( GCONF_URL_KEY, url)
        self.preferences_dialog.hide()

    def on_prefs_cancel_btn_clicked(self, widget):
        self.preferences_dialog.hide()

    def on_prefs_death(self, widget):
        self.preferences_dialog.hide()

    def on_start_activate(self, data):

        url = self.gconf_client.get_string( GCONF_URL_KEY)
        self.server = subprocess.Popen([SYNCEVO_HTTP_SERVER, url])
        print "server  pid %s started on url %s " % (self.server.pid, url)
        self._start_ui(url)

    def on_stop_activate(self, data = None):

        if self.server != None:
            self.server.kill()
            self.server  = None
        self.set_tooltip(_("Server stop under way"))

    def on_about_activate(self, data):

        dialog = gtk.AboutDialog()
        dialog.set_name('Syncevolution HTTP server')
        dialog.set_version('0.0.1')
        dialog.set_comments(ABOUT_LONG)
        dialog.set_website('http://synevolution.org')
        dialog.run()
        dialog.destroy()


def notify_already_running():
    """ Display an "Already running" notification. """

    n = pynotify.Notification(
             _("Already running"),
             _("Syncevolution http server is already running"),
             _("dialog-warning")
    )
    n.set_urgency( pynotify.URGENCY_NORMAL)
    n.set_timeout( 4000)
    n.show()

def check_already_running():
    """ Make sure that we are the only instance, else exit. """

    class SingletonDBusObject(dbus.service.Object):
        """ Guarantees a single instance on DBus. """

        def __init__(self, bus_name):
            dbus.service.Object.__init__(self,
                                         bus_name,
                                         DBUS_OBJECT_PATH)

    try:
        bus_name = dbus.service.BusName( DBUS_SERVICE_NAME,
                                         bus = dbus.SessionBus(),
                                         do_not_queue = True)
        SingletonDBusObject( bus_name)
    except:
        notify_already_running()
        print "Another instance is already running!"
        sys.exit(1)

def on_sigchld(signum, frame):
     gobject.idle_add(status_icon.on_sigchld, signum, frame)

def main():

    global status_icon

    gobject.threads_init()
    pynotify.init(_("Syncevolution HTTP server"))
    check_already_running()
    signal.signal( signal.SIGCHLD, on_sigchld)
    status_icon = HttpServerStatusIcon()
    gtk.main()

if __name__ == '__main__':
    main()

