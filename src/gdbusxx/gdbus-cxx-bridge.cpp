/*
 * Copyright (C) 2011 Intel Corporation
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

#include "gdbus-cxx-bridge.h"
#include <stdio.h>

void intrusive_ptr_add_ref(GDBusConnection *con)  { g_object_ref(con); }
void intrusive_ptr_release(GDBusConnection *con)  { g_object_unref(con); }
void intrusive_ptr_add_ref(GDBusMessage    *msg)  { g_object_ref(msg); }
void intrusive_ptr_release(GDBusMessage    *msg)  { g_object_unref(msg); }
static void intrusive_ptr_add_ref(GDBusServer *server) { g_object_ref(server); }
static void intrusive_ptr_release(GDBusServer *server) { g_object_unref(server); }


namespace GDBusCXX {

MethodHandler::MethodMap MethodHandler::m_methodMap;
boost::function<void (void)> MethodHandler::m_callback;

// It should be okay to use global variables here because they are
// only used inside the main thread while it waits in undelay() for a
// positive or negative result to g_bus_own_name_on_connection().
// Once acquired, the name can only get lost again when the
// D-Bus daemon dies (no name owership alloed), in which case the
// process dies anyway.
static bool nameError;
static bool nameAcquired;
static void BusNameAcquired(GDBusConnection *connection,
                            const gchar *name,
                            gpointer userData) throw ()
{
    try {
        g_debug("got D-Bus name %s", name);
        nameAcquired = true;
    } catch (...) {
        nameError = true;
    }
}

static void BusNameLost(GDBusConnection *connection,
                        const gchar *name,
                        gpointer userData) throw ()
{
    try {
        g_debug("lost %s %s",
                connection ? "D-Bus connection for name" :
                "D-Bus name",
                name);
    } catch (...) {
    }
    nameError = true;
}

void DBusConnectionPtr::undelay() const
{
    if (!m_name.empty()) {
        g_debug("starting to acquire D-Bus name %s", m_name.c_str());
        nameAcquired = false;
        nameError = false;
        char *copy = g_strdup(m_name.c_str());
        g_bus_own_name_on_connection(get(),
                                     copy,
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     BusNameAcquired,
                                     BusNameLost,
                                     copy,
                                     g_free);
        while (!nameAcquired && !nameError) {
            g_main_context_iteration(NULL, true);
        }
        g_debug("done with acquisition of %s", m_name.c_str());
        if (nameError) {
            throw std::runtime_error("could not obtain D-Bus name - already running?");
        }
    }
    g_dbus_connection_start_message_processing(get());
}

DBusConnectionPtr dbus_get_bus_connection(const char *busType,
                                          const char *name,
                                          bool unshared,
                                          DBusErrorCXX *err)
{
    DBusConnectionPtr conn;
    GError* error = NULL;
    GBusType type =
        boost::iequals(busType, "SESSION") ?
        G_BUS_TYPE_SESSION :
        G_BUS_TYPE_SYSTEM;

    if (unshared) {
        char *address = g_dbus_address_get_for_bus_sync(type,
                                                        NULL, &error);
        if(address == NULL) {
            if (err) {
                err->set(error);
            }
            return NULL;
        }
        // Here we set up a private client connection using the chosen bus' address.
        conn = DBusConnectionPtr(g_dbus_connection_new_for_address_sync(address,
                                                                        (GDBusConnectionFlags)
                                                                        (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                                         G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                                        NULL, NULL, &error),
                                 false);
        g_free(address);

        if(conn == NULL) {
            if (err) {
                err->set(error);
            }
            return NULL;
        }
    } else {
        // This returns a singleton, shared connection object.
        conn = DBusConnectionPtr(g_bus_get_sync(type,
                                                NULL, &error),
                                 false);
        if(conn == NULL) {
            if (err) {
                err->set(error);
            }
            return NULL;
        }
    }

    if (name) {
        // Request name later in undelay(), after the caller
        // had a chance to add objects.
        conn.addName(name);
        // Acting as client, need to stop when D-Bus daemon dies.
        g_dbus_connection_set_exit_on_close(conn.get(), TRUE);
    }

    return conn;
}

DBusConnectionPtr dbus_get_bus_connection(const std::string &address,
                                          DBusErrorCXX *err,
                                          bool delayed /*= false*/)
{
    GError* error = NULL;
    int flags = G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT;

    if (delayed) {
        flags |= G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING;
    }

    DBusConnectionPtr conn(g_dbus_connection_new_for_address_sync(address.c_str(),
                                                                  static_cast<GDBusConnectionFlags>(flags),
                                                                  NULL, /* GDBusAuthObserver */
                                                                  NULL, /* GCancellable */
                                                                  &error),
                           false);
    if (!conn && err) {
        err->set(error);
    }

    return conn;
}

static void ConnectionLost(GDBusConnection *connection,
                           gboolean remotePeerVanished,
                           GError *error,
                           gpointer data)
{
    DBusConnectionPtr::Disconnect_t *cb = static_cast<DBusConnectionPtr::Disconnect_t *>(data);
    (*cb)();
}

static void DestroyDisconnect(gpointer data,
                              GClosure *closure)
                           {
    DBusConnectionPtr::Disconnect_t *cb = static_cast<DBusConnectionPtr::Disconnect_t *>(data);
    delete cb;
}

void DBusConnectionPtr::flush()
{
    // ignore errors
    g_dbus_connection_flush_sync(get(), NULL, NULL);
}

void DBusConnectionPtr::setDisconnect(const Disconnect_t &func)
{
    g_signal_connect_closure(get(),
                             "closed",
                             g_cclosure_new(G_CALLBACK(ConnectionLost),
                                            new Disconnect_t(func),
                                            DestroyDisconnect),
                             true);
}

boost::shared_ptr<DBusServerCXX> DBusServerCXX::listen(const std::string &address, DBusErrorCXX *err)
{
    GDBusServer *server = NULL;
    const char *realAddr = address.c_str();
    char buffer[80];

    gchar *guid = g_dbus_generate_guid();
    GError *error = NULL;
    if (address.empty()) {
        realAddr = buffer;
        for (int counter = 1; counter < 100 && !server; counter++) {
            if (error) {
                // previous attempt failed
                g_debug("setting up D-Bus server on %s failed, trying next address: %s",
                        realAddr,
                        error->message);
                g_clear_error(&error);
            }
            sprintf(buffer, "unix:abstract=gdbuscxx-%d", counter);
            server = g_dbus_server_new_sync(realAddr,
                                            G_DBUS_SERVER_FLAGS_NONE,
                                            guid,
                                            NULL, /* GDBusAuthObserver */
                                            NULL, /* GCancellable */
                                            &error);
        }
    } else {
        server = g_dbus_server_new_sync(realAddr,
                                        G_DBUS_SERVER_FLAGS_NONE,
                                        guid,
                                        NULL, /* GDBusAuthObserver */
                                        NULL, /* GCancellable */
                                        &error);
    }
    g_free(guid);

    if (!server) {
        if (err) {
            err->set(error);
        }
        return boost::shared_ptr<DBusServerCXX>();
    }

    // steals reference to 'server'
    boost::shared_ptr<DBusServerCXX> res(new DBusServerCXX(server, realAddr));
    g_signal_connect(server,
                     "new-connection",
                     G_CALLBACK(DBusServerCXX::newConnection),
                     res.get());
    return res;
}

gboolean DBusServerCXX::newConnection(GDBusServer *server, GDBusConnection *newConn, void *data) throw()
{
    DBusServerCXX *me = static_cast<DBusServerCXX *>(data);
    if (me->m_newConnection) {
        GCredentials *credentials;
        std::string credString;

        credentials = g_dbus_connection_get_peer_credentials(newConn);
        if (credentials == NULL) {
            credString = "(no credentials received)";
        } else {
            gchar *s = g_credentials_to_string(credentials);
            credString = s;
            g_free(s);
        }
        g_debug("Client connected.\n"
                "Peer credentials: %s\n"
                "Negotiated capabilities: unix-fd-passing=%d\n",
                credString.c_str(),
                g_dbus_connection_get_capabilities(newConn) & G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING);

        try {
            // Ref count of connection has to be increased if we want to handle it.
            // Something inside m_newConnection has to take ownership of connection,
            // because conn increases ref count only temporarily.
            DBusConnectionPtr conn(newConn, true);
            me->m_newConnection(*me, conn);
        } catch (...) {
            g_error("handling new D-Bus connection failed with C++ exception");
            return FALSE;
        }

        return TRUE;
    } else {
        return FALSE;
    }
}

DBusServerCXX::DBusServerCXX(GDBusServer *server, const std::string &address) :
    m_server(server, false), // steal reference
    m_address(address)
{
    g_dbus_server_start(server);
}

DBusServerCXX::~DBusServerCXX()
{
    g_dbus_server_stop(m_server.get());
}

void Watch::nameOwnerChanged(GDBusConnection *connection,
                             const gchar *sender_name,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *signal_name,
                             GVariant *parameters,
                             gpointer user_data)
{
    Watch *watch = static_cast<Watch *>(user_data);
    if (!watch->m_called) {
        gchar *name = NULL, *oldOwner = NULL, *newOwner = NULL;
        g_variant_get(parameters, "(sss)", &name, &oldOwner, &newOwner);
        bool matches = name && watch->m_peer == name &&
            newOwner && !*newOwner;
        g_free(name);
        g_free(oldOwner);
        g_free(newOwner);
        if (matches) {
            watch->disconnected();
        }
    }
}

void Watch::disconnected()
{
    if (!m_called) {
        m_called = true;
        if (m_callback) {
            m_callback();
        }
    }
}

Watch::Watch(const DBusConnectionPtr &conn,
                     const boost::function<void (void)> &callback) :
    m_conn(conn),
    m_callback(callback),
    m_called(false),
    m_watchID(0)
{
}

void Watch::setCallback(const boost::function<void (void)> &callback)
{
    m_callback = callback;
    if (m_called && m_callback) {
        m_callback();
    }
}

void Watch::activate(const char *peer)
{
    if (!peer) {
        throw std::runtime_error("Watch::activate(): no peer");
    }
    m_peer = peer;

    // Install watch first ...
    m_watchID = g_dbus_connection_signal_subscribe(m_conn.get(),
                                                   NULL, // TODO org.freedesktop.DBus?
                                                   "org.freedesktop.DBus",
                                                   "NameOwnerChanged",
                                                   "/org/freedesktop/DBus",
                                                   NULL,
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   nameOwnerChanged,
                                                   this,
                                                   NULL);
    if (!m_watchID) {
        throw std::runtime_error("g_dbus_connection_signal_subscribe(): NameLost failed");
    }

    // ... then check that the peer really exists,
    // otherwise we'll never notice the disconnect.
    // If it disconnects while we are doing this,
    // then disconnect() will be called twice,
    // but it handles that.
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(m_conn.get(),
                                                   "org.freedesktop.DBus",
                                                   "/org/freedesktop/DBus",
                                                   "org.freedesktop.DBus",
                                                   "NameHasOwner",
                                                   g_variant_new("(s)", peer),
                                                   G_VARIANT_TYPE("(b)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1, // default timeout
                                                   NULL,
                                                   &error);

    if (result != NULL) {
        bool actual_result = false;

        g_variant_get(result, "(b)", &actual_result);
        if (!actual_result) {
            disconnected();
        }
    } else {
        std::string error_message(error->message);
        g_error_free(error);
        std::string err_msg("g_dbus_connection_call_sync(): NameHasOwner - ");
        throw std::runtime_error(err_msg + error_message);
    }
}

Watch::~Watch()
{
    if (m_watchID) {
        g_dbus_connection_signal_unsubscribe(m_conn.get(), m_watchID);
        m_watchID = 0;
    }
}

void getWatch(GDBusConnection *conn, GDBusMessage *msg,
              GVariantIter &iter, boost::shared_ptr<Watch> &value)
{
    std::auto_ptr<Watch> watch(new Watch(conn));
    watch->activate(g_dbus_message_get_sender(msg));
    value.reset(watch.release());
}

} // namespace GDBusCXX
