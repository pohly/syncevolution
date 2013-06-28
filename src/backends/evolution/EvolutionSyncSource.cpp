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
#include <syncevo/GLibSupport.h>

#ifdef USE_EDS_CLIENT
#include <syncevo/GValueSupport.h>
SE_GLIB_TYPE(GKeyFile, g_key_file)
#endif

#include <syncevo/declarations.h>
SE_BEGIN_CXX

#ifdef HAVE_EDS

#ifdef USE_EDS_CLIENT

void EvolutionSyncSource::getDatabasesFromRegistry(SyncSource::Databases &result,
                                                   const char *extension,
                                                   ESource *(*refDef)(ESourceRegistry *))
{
    ESourceRegistryCXX registry = EDSRegistryLoader::getESourceRegistry();
    ESourceListCXX sources(e_source_registry_list_sources(registry, extension));
    ESourceCXX def(refDef ? refDef(registry) : NULL,
                   TRANSFER_REF);
    BOOST_FOREACH (ESource *source, sources) {
        result.push_back(Database(e_source_get_display_name(source),
                                  e_source_get_uid(source),
                                  e_source_equal(def, source)));
    }
}

static void handleErrorCB(EClient */*client*/, const gchar *error_msg, gpointer user_data)
{
    EvolutionSyncSource *that = static_cast<EvolutionSyncSource *>(user_data);
    SE_LOG_ERROR(that->getDisplayName(), "%s", error_msg);
}

EClientCXX EvolutionSyncSource::openESource(const char *extension,
                                            ESource *(*refBuiltin)(ESourceRegistry *),
                                            const boost::function<EClient *(ESource *, GError **gerror)> &newClient)
{
    EClientCXX client;
    GErrorCXX gerror;
    ESourceRegistryCXX registry = EDSRegistryLoader::getESourceRegistry();
    ESourceListCXX sources(e_source_registry_list_sources(registry, extension));
    string id = getDatabaseID();
    ESource *source = findSource(sources, id);
    bool created = false;

    if (!source) {
        if (refBuiltin && (id.empty() || id == "<<system>>")) {
            ESourceCXX builtin(refBuiltin(registry), TRANSFER_REF);
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


    while (true) {
        // Always allow EDS to create the database. "only-if-exists =
        // true" does not make sense.
        if (!e_client_open_sync(client, false, NULL, gerror)) {
            if (gerror && g_error_matches(gerror, E_CLIENT_ERROR, E_CLIENT_ERROR_BUSY)) {
                gerror.clear();
                sleep(1);
            } else if (created) {
                // Opening newly created address books often failed in
                // old EDS releases - try again. Probably covered by
                // more recently added E_CLIENT_ERROR_BUSY check above.
                gerror.clear();
                sleep(5);
            } else {
                throwError("opening database", gerror);
            }
        } else {
            // Success!
            break;
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
    // We'll need this later. Create it before doing any real work.
    ESourceRegistryCXX registry = EDSRegistryLoader::getESourceRegistry();

    // Clone the system DB. This allows the distro to change the
    // configuration (backend, extensions (= in particular
    // the contacts DB summary fields)) without having to
    // modify SyncEvolution.
    ESourceCXX systemSource = refSystemDB();
    gsize len;
    PlainGStr ini(e_source_to_string(systemSource, &len));

    // Modify the entries in the key file directly. We can't
    // instantiate an ESource (no API for it), copying the values from
    // the key file into a fresh ESource is difficult (would have to
    // reimplement EDS internal encoding/decoding), and copying from
    // systemSource is hard (don't know which extensions it has,
    // cannot instantiate extensions of unknown types, because
    // e_source_get_extension() only works for types that were
    // created).
    static const char *mainSection = "Data Source";
    GKeyFileCXX keyfile(g_key_file_new(), TRANSFER_REF);
    GErrorCXX gerror;
    if (!g_key_file_load_from_data(keyfile, ini, len, G_KEY_FILE_NONE, gerror)) {
        gerror.throwError("parsing ESource .ini data");
    }
    PlainGStrArray keys(g_key_file_get_keys(keyfile, mainSection, NULL, gerror));
    if (!keys) {
        gerror.throwError("listing keys in main section");
    }
    for (int i = 0; keys.at(i); i++) {
        if (boost::starts_with(keys[i], "DisplayName[")) {
            if (!g_key_file_remove_key(keyfile, mainSection, keys.at(i), gerror)) {
                gerror.throwError("remove key");
            }
        }
    }
    g_key_file_set_string(keyfile, mainSection, "DisplayName", database.m_name.c_str());
    g_key_file_set_boolean(keyfile, mainSection, "Enabled", true);
    ini = g_key_file_to_data(keyfile, &len, NULL);
    const char *configDir = g_get_user_config_dir();
    int fd;
    std::string filename;
    std::string uid;

    // Create sources dir. It might have been removed (for example, while
    // testing) without having been recreated by evolution-source-registry.
    std::string sourceDir = StringPrintf("%s/evolution/sources",
                                         configDir);
    mkdir_p(sourceDir);

    // Create unique ID if necessary.
    while (true) {
        uid = database.m_uri.empty() ?
            UUID() :
            database.m_uri;
        filename = StringPrintf("%s/%s.source", sourceDir.c_str(), uid.c_str());
        fd = ::open(filename.c_str(),
                    O_WRONLY|O_CREAT|O_EXCL,
                    S_IRUSR|S_IWUSR);
        if (fd >= 0) {
            break;
        }
        if (errno == EEXIST) {
            if (!database.m_uri.empty()) {
                SE_THROW(StringPrintf("ESource UUID %s already in use", database.m_uri.c_str()));
            } else {
                // try again with new random UUID
            }
        } else {
            SE_THROW(StringPrintf("creating %s failed: %s", filename.c_str(), strerror(errno)));
        }
    }
    ssize_t written = write(fd, ini.get(), len);
    int res = ::close(fd);
    if (written != (ssize_t)len || res) {
        SE_THROW(StringPrintf("writing to %s failed: %s", filename.c_str(), strerror(errno)));
    }

    // We need to wait until ESourceRegistry notices the new file.
    SE_LOG_DEBUG(getDisplayName(), "waiting for ESourceRegistry to notice new ESource %s", uid.c_str());
    while (!ESourceCXX(e_source_registry_ref_source(registry, uid.c_str()), TRANSFER_REF)) {
        // This will block forever if called from the non-main-thread.
        // Don't do that...
        g_main_context_iteration(NULL, true);
    }
    SE_LOG_DEBUG(getDisplayName(), "ESourceRegistry has new ESource %s", uid.c_str());

    // Try triggering that by attempting to create an ESource with the same
    // UUID. Does not work! evolution-source-registry simply overwrites the
    // file that we created earlier.
    // ESourceCXX source(e_source_new_with_uid(uid.c_str(),
    //                                         NULL, gerror),
    //                   TRANSFER_REF);
    // e_source_set_display_name(source, "syncevolution-fake");
    // e_source_set_parent(source, "local-stub");
    // ESourceListCXX sources;
    // sources.push_back(source.ref()); // ESourceListCXX unrefs sources it points to
    // if (!e_source_registry_create_sources_sync(registry,
    //                                            sources,
    //                                            NULL,
    //                                            gerror)) {
    //     gerror.throwError(StringPrintf("creating EDS database of type %s with name '%s'%s%s",
    //                                    sourceExtension(),
    //                                    database.m_name.c_str(),
    //                                    database.m_uri.empty() ? "" : " and URI ",
    //                                    database.m_uri.c_str()));
    // } else {
    //     SE_THROW("creating syncevolution-fake ESource succeeded although it should have failed");
    // }

    return Database(database.m_name, uid);
}

void EvolutionSyncSource::deleteDatabase(const std::string &uri, RemoveData removeData)
{
    ESourceRegistryCXX registry = EDSRegistryLoader::getESourceRegistry();
    ESourceCXX source(e_source_registry_ref_source(registry, uri.c_str()), TRANSFER_REF);
    if (!source) {
        throwError(StringPrintf("EDS database with URI '%s' cannot be deleted, does not exist",
                                uri.c_str()));
    }
    GErrorCXX gerror;
    if (!e_source_remove_sync(source, NULL, gerror)) {
        throwError(StringPrintf("deleting EDS database with URI '%s'", uri.c_str()),
                   gerror);
    }
    if (removeData == REMOVE_DATA_FORCE) {
        // Don't wait for evolution-source-registry cache-reaper to
        // run, instead remove files ourselves. The reaper runs only
        // once per day and also only moves the data into a trash
        // folder, were it would linger until finally removed after 30
        // days.
        //
        // This is equivalent to "rm -rf $XDG_DATA_HOME/evolution/*/<uuid>".
        std::string basedir = StringPrintf("%s/evolution", g_get_user_data_dir());
        if (isDir(basedir)) {
            BOOST_FOREACH (const std::string &kind, ReadDir(basedir)) {
                std::string subdir = basedir + "/" + kind;
                if (isDir(subdir)) {
                    BOOST_FOREACH (const std::string &source, ReadDir(subdir)) {
                        // We assume that the UUID of the database
                        // consists only of characters which can be
                        // used in the directory name, i.e., no
                        // special encoding of the directory name.
                        if (source == uri) {
                            rm_r(subdir + "/" + source);
                            // Keep searching, just in case, although
                            // there should only be one.
                        }
                    }
                }
            }
        }
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
