/*
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "config.h"
#include "sync-ui.h"

static char *settings_id = NULL;

static GOptionEntry entries[] =
{
  { "show-settings", 0, 0, G_OPTION_ARG_STRING, &settings_id, "Open sync settings for given sync url or configuration name", "url or config name" },
  { NULL }
};


static void
set_app_name_and_icon ()
{
    /* TRANSLATORS: this is the application name that may be used by e.g.
       the windowmanager */
    g_set_application_name (_("Sync"));
    gtk_window_set_default_icon_name ("sync");
}

static void
init (int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;

    gtk_init (&argc, &argv);
    bindtextdomain (GETTEXT_PACKAGE, SYNCEVOLUTION_LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    context = g_option_context_new ("- synchronise PIM data with Syncevolution");
    g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
    g_option_context_add_group (context, gtk_get_option_group (TRUE));
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_warning ("option parsing failed: %s\n", error->message);
    }
}

static void
activate (GtkApplication *app)
{
    GList *list;
    GtkWindow *window;

    list = gtk_application_get_windows (app);

    if (list) {
        gtk_window_present (GTK_WINDOW (list->data));
    } else {
        app_data *data = sync_ui_create ();
        window = sync_ui_get_main_window (data);
        gtk_window_set_application (window, app);
        gtk_widget_show (GTK_WIDGET (window));

        if (settings_id) {
            sync_ui_show_settings (data, settings_id);
        }
    }
}

int
main (int argc, char *argv[])
{
    GtkApplication *app;
    gint status;

    init (argc, argv);
    set_app_name_and_icon ();

    app = gtk_application_new ("org.Moblin.Sync", 0);
    g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);

    status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}
