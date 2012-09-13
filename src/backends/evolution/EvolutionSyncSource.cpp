/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include "EvolutionSyncSource.h"
#include <syncevo/SmartPtr.h>
#include <syncevo/SyncContext.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef HAVE_EDS

#ifdef USE_EDS_CLIENT

void EvolutionSyncSource::getDatabasesFromRegistry(SyncSource::Databases &result,
                                                   const char *extension,
                                                   ESource *(*refDef)(ESourceRegistry *))
{
    GErrorCXX gerror;
    ESourceRegistryCXX registry = getSourceRegistry();
    if (!registry) {
        throwError("unable to access databases registry", gerror);
    }
    ESourceListCXX sources(e_source_registry_list_sources(registry, extension));
    ESourceCXX def(refDef ? refDef(registry) : NULL,
                   false);
    BOOST_FOREACH (ESource *source, sources) {
        result.push_back(Database(e_source_get_display_name(source),
                                  e_source_get_uid(source),
                                  e_source_equal(def, source)));
    }
}

ESourceRegistryCXX EvolutionSyncSource::getSourceRegistry()
{
    // Keep the instance forever. This is a bit more efficient. But
    // primarily it avoids a file and memory descriptor leak in EDS
    // 3.5.x when the registry is created and freed over and over
    // again.
    static ESourceRegistryCXX registry;
    if (!registry) {
        GErrorCXX gerror;
        // A workaround for
        // https://bugzilla.gnome.org/show_bug.cgi?id=683519:
        // e_source_registry_new_sync() hangs randomly in 3.5 because
        // of a glib bug and it is unclear whether the workaround
        // suggested in the bug will be added to it.
        //
        // e_source_registry_new() doesn't have the problem and
        // (with the helper macro) isn't harder to use either.
        SYNCEVO_GLIB_CALL_SYNC(registry, gerror,
                               e_source_registry_new,
                               NULL);
        // registry = ESourceRegistryCXX::steal(e_source_registry_new_sync(NULL, gerror));
        if (!registry) {
            throwError("unable to access databases registry", gerror);
        }
    }
    return registry;
}

static void handleErrorCB(EClient */*client*/, const gchar *error_msg, gpointer user_data)
{
    EvolutionSyncSource *that = static_cast<EvolutionSyncSource *>(user_data);
    SE_LOG_ERROR(that, NULL, "%s", error_msg);
}

EClientCXX EvolutionSyncSource::openESource(const char *extension,
                                            ESource *(*refBuiltin)(ESourceRegistry *),
                                            const boost::function<EClient *(ESource *, GError **gerror)> &newClient)
{
    EClientCXX client;
    GErrorCXX gerror;
    ESourceRegistryCXX registry = getSourceRegistry();
    if (!registry) {
        throwError("unable to access databases registry", gerror);
    }
    ESourceListCXX sources(e_source_registry_list_sources(registry, extension));
    string id = getDatabaseID();
    ESource *source = findSource(sources, id);
    bool created = false;

    if (!source) {
        if (refBuiltin && (id.empty() || id == "<<system>>")) {
            ESourceCXX builtin(refBuiltin(registry), false);
            client = EClientCXX::steal(newClient(builtin, gerror));
            // } else if (!id.compare(0, 7, "file://")) {
                // TODO: create source
                // m_calendar = ECalClientCXX::steal(e_cal_client_new_from_uri(id.c_str(), sourceType(), gerror));
        } else {
            throwError(string("database not found: '") + id + "'");
        }
        created = true;
    } else {
        client = EClientCXX::steal(newClient(source, gerror));
    }

    if (!client) {
        throwError("accessing database", gerror);
    }

    // Listen for errors
    g_signal_connect (client, "backend-error", G_CALLBACK(handleErrorCB), this); 
    g_signal_connect_after(client,
                           "backend-died",
                           G_CALLBACK(SyncContext::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly.");


    // Always allow EDS to create the database. "only-if-exists =
    // true" does not make sense.
    if (!e_client_open_sync(client, false, NULL, gerror)) {
        if (created) {
            // Opening newly created address books often failed in old
            // EDS releases - try again.
            gerror.clear();
            sleep(5);
            if (!e_client_open_sync(client, false, NULL, gerror)) {
                throwError("opening database", gerror);
            }
        } else {
            throwError("opening database", gerror);
        }
    }

    // record result for SyncSource::getDatabase()
    source = e_client_get_source(client);
    if (source) {
        Database database(e_source_get_display_name(source),
                          e_source_get_uid(source));
        setDatabase(database);
    }

    return client;
}

SyncSource::Database EvolutionSyncSource::createDatabase(const Database &database)
{
    GErrorCXX gerror;
    ESourceCXX source(e_source_new_with_uid(database.m_uri.empty() ? NULL : database.m_uri.c_str(),
                                            NULL, gerror), false);
    if (!source) {
        gerror.throwError("e_source_new()");
    }
    e_source_set_enabled(source, true);
    e_source_set_display_name(source, database.m_name.c_str());
    e_source_set_parent(source, "local-stub");


    // create extension of the right type and set "local" as backend
    ESourceBackend *backend = static_cast<ESourceBackend *>(e_source_get_extension(source, sourceExtension()));
    e_source_backend_set_backend_name(backend, "local");

    ESourceRegistryCXX registry = getSourceRegistry();
    ESourceListCXX sources;
    sources.push_back(source.ref()); // ESourceListCXX unrefs sources it points to
    if (!e_source_registry_create_sources_sync(registry,
                                               sources,
                                               NULL,
                                               gerror)) {
        gerror.throwError(StringPrintf("creating EDS database of type %s with name '%s'%s%s",
                                       sourceExtension(),
                                       database.m_name.c_str(),
                                       database.m_uri.empty() ? "" : " and URI ",
                                       database.m_uri.c_str()));
    }
    const gchar *name = e_source_get_display_name(source);
    const gchar *uid = e_source_get_uid(source);
    return Database(name, uid);
}

void EvolutionSyncSource::deleteDatabase(const std::string &uri)
{
    ESourceRegistryCXX registry = getSourceRegistry();
    ESourceCXX source(e_source_registry_ref_source(registry, uri.c_str()), false);
    if (!source) {
        throwError(StringPrintf("EDS database with URI '%s' cannot be deleted, does not exist",
                                uri.c_str()));
    }
    GErrorCXX gerror;
    if (!e_source_remove_sync(source, NULL, gerror)) {
        throwError(StringPrintf("deleting EDS database with URI '%s'", uri.c_str()),
                   gerror);
    }
}


#endif // USE_EDS_CLIENT

ESource *EvolutionSyncSource::findSource(const ESourceListCXX &list, const string &id )
{
    string finalID;
    if (!id.empty()) {
        finalID = id;
    } else {
        // Nothing selected specifically, use the one marked as default.
        BOOST_FOREACH(const Database &db, getDatabases()) {
            if (db.m_isDefault) {
                finalID = db.m_uri;
                break;
            }
        }
    }

#ifdef USE_EDS_CLIENT
    BOOST_FOREACH (ESource *source, list) {
        bool found =
            !finalID.compare(e_source_get_display_name(source)) ||
            !finalID.compare(e_source_get_uid(source));
        if (found) {
            return source;
        }
    }
#else
    for (GSList *g = e_source_list_peek_groups (list.get()); g; g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        GSList *s;
        for (s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            GStringPtr uri(e_source_get_uri(source));
            bool found = finalID.empty() ||
                !finalID.compare(e_source_peek_name(source)) ||
                (uri && !finalID.compare(uri));
            if (found) {
                return source;
            }
        }
    }
#endif
    return NULL;
}

void EvolutionSyncSource::throwError(const string &action, GErrorCXX &gerror)
{
    string gerrorstr;
    if (gerror) {
        gerrorstr += ": ";
        gerrorstr += gerror->message;
    } else {
        gerrorstr = ": failure";
    }

    throwError(action + gerrorstr);
}
#endif // HAVE_EDS

SE_END_CXX
