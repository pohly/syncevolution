/*
 * Copyright (C) 2012 Intel Corporation
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

#include <cstdio>
#include <exception>

#include <glib.h>

#include "GoogleAuthService.h"
#include "GoogleContactService.h"
#include "GoogleVCard.h"


#define GOOGLE_CLIENT_ID        "XXX.apps.googleusercontent.com"


static gboolean quit_cb (gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;

    g_main_loop_quit(loop);

    return FALSE;
}


int main (int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("%s <username> <password>\n", argv[0]);
        return 1;
    }

    try
    {
        g_type_init();

        GMainContext *ctx;
        GMainLoop *loop;
        google_contact_vector_t contacts;
        google_contact_vector_t::iterator contact_iter;

        ctx = g_main_context_default();
        loop = g_main_loop_new(ctx, TRUE);

        GoogleAuthService gas(GOOGLE_CLIENT_ID,
                              GoogleContactService::ServiceType());
        gas.Authenticate(argv[1], argv[2]);

        GoogleContactService gcs(gas);
        contacts = gcs.QueryAllContacts();
        for (contact_iter = contacts.begin();
             contact_iter != contacts.end();
             contact_iter++) {
            GoogleVCard gvc(**contact_iter);
            puts(gvc.card.c_str());
        }

        g_timeout_add_seconds(1, quit_cb, loop);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    }
    catch (std::exception &x)
    {
        fprintf(stderr, "%s\n", x.what());
        return -1;
    }
    catch (...)
    {
        fputs("unknown exception\n", stderr);
        return -1;
    }

    return 0;
}

