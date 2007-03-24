/*
 * Copyright (C) 2005-2006 Patrick Ohly
 * Copyright (C) 2007 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <config.h>

#include <base/Log.h>
#include <posix/base/posixlog.h>

#include <iostream>
using namespace std;

#include <libgen.h>
#ifdef HAVE_GLIB
#include <glib-object.h>
#endif

#include "EvolutionSyncSource.h"
#include "EvolutionSyncClient.h"

#if defined(ENABLE_MAEMO) && defined (ENABLE_EBOOK)

#include <dlfcn.h>

extern "C" EContact *e_contact_new_from_vcard(const char *vcard)
{
    static typeof(e_contact_new_from_vcard) *impl;

    if (!impl) {
        impl = (typeof(impl))dlsym(RTLD_NEXT, "e_contact_new_from_vcard");
    }

    // Old versions of EDS-DBus parse_changes_array() call
    // e_contact_new_from_vcard() with a pointer which starts
    // with a line break; Evolution is not happy with that and
    // refuses to parse it. This code forwards until it finds
    // the first non-whitespace, presumably the BEGIN:VCARD.
    while (*vcard && isspace(*vcard)) {
        vcard++;
    }

    return impl ? impl(vcard) : NULL;
}
#endif

/**
 * list all known data sources of a certain type
 */
static void listSources( EvolutionSyncSource &syncSource, const string &header )
{
    cout << header << ":\n";
    EvolutionSyncSource::sources sources = syncSource.getSyncBackends();

    for( EvolutionSyncSource::sources::const_iterator it = sources.begin();
         it != sources.end();
         it++ ) {
        cout << it->m_name << " (" << it->m_uri << ")\n";
    }
}

int main( int argc, char **argv )
{
#ifdef ENABLE_MAEMO
    // EDS-DBus uses potentially long-running calls which may fail due
    // to the default 25s timeout. Some of these can be replaced by
    // their async version, but e_book_async_get_changes() still
    // triggered it.
    //
    // The workaround for this is to link the binary against a libdbus
    // which has the dbus-timeout.patch and thus let's users and
    // the application increase the default timeout.
    setenv("DBUS_DEFAULT_TIMEOUT", "600000", 0);
#endif
    
#ifdef HAVE_GLIB
    // this is required on Maemo and does not harm either on a normal
    // desktop system with Evolution
    g_type_init();
#endif

    setLogFile("-");
    LOG.reset();
    LOG.setLevel(LOG_LEVEL_INFO);
    resetError();

    // Expand PATH to cover the directory we were started from?
    // This might be needed to find normalize_vcard.
    char *exe = strdup(argv[0]);
    if (strchr(exe, '/') ) {
        char *dir = dirname(exe);
        string path;
        char *oldpath = getenv("PATH");
        if (oldpath) {
            path += oldpath;
            path += ":";
        }
        path += dir;
        setenv("PATH", path.c_str(), 1);
    }
    free(exe);

    try {
        if ( argc == 1 ) {
            const struct { const char *mimeType, *kind; } kinds[] = {
                { "text/vcard",  "address books" },
                { "text/calendar", "calendars" },
                { "text/x-journal", "memos" },
                { "text/x-todo", "tasks" },
                { NULL }
            };

            for (int i = 0; kinds[i].mimeType; i++ ) {
                auto_ptr<EvolutionSyncSource> source(EvolutionSyncSource::createSource("list", NULL, "", "", kinds[i].mimeType, false));
                if (source.get() != NULL) {
                    listSources(*source, kinds[i].kind);
                    cout << "\n";
                }
            }

            fprintf( stderr, "usage: %s <server>\n", argv[0] );
        } else {
            set<string> sources;

            for (int source = 2; source < argc; source++ ) {
                sources.insert(argv[source]);
            }

            EvolutionSyncClient client(argv[1], true, sources);
            client.sync();
        }
        return 0;
    } catch ( const std::exception &ex ) {
        LOG.error( "%s", ex.what() );
    } catch (...) {
        LOG.error( "unknown error" );
    }

    return 1;
}
