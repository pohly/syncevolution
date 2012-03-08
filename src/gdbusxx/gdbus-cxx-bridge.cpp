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

struct FilterData
{
    FilterData(const DBusConnectionPtr::FilterFunc &filter)
      : m_filter(filter) {}
    DBusConnectionPtr::FilterFunc m_filter;
};

void filter_data_free(gpointer user_data)
{
    FilterData* filter_data(static_cast<FilterData*>(user_data));

    delete filter_data;
}

GDBusMessage* filter_cb(GDBusConnection *conn,
                        GDBusMessage *message,
                        gboolean /* incoming */,
                        gpointer user_data)
{
    if (user_data != NULL) {
        FilterData* filter_data(static_cast<FilterData*>(user_data));
        DBusMessagePtr message_ptr(message, true);
        DBusConnectionPtr connection_ptr(conn, true);

        if (!filter_data->m_filter(connection_ptr, message_ptr)) {
            g_object_unref (message);
            return NULL;
        }
    }
    return message;
}

unsigned int DBusConnectionPtr::add_filter(const DBusConnectionPtr::FilterFunc &filter)
{
    FilterData* filter_data(new FilterData(filter));

    return g_dbus_connection_add_filter(get(),
                                        filter_cb,
                                        static_cast<void*>(filter_data),
                                        filter_data_free);
}

void DBusConnectionPtr::remove_filter(unsigned int id)
{
    g_dbus_connection_remove_filter(get(), id);
}

void DBusConnectionPtr::send(const DBusMessagePtr &message)
{
    GError* error(0);

    g_dbus_connection_send_message(get(),
                                   message.get(),
                                   G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                   0,
                                   &error);

    if (error) {
        DBusErrorCXX error_cxx(error);

        error_cxx.throwFailure("Sending message");
    }
}

// static
DBusMessagePtr DBusMessagePtr::create_empty_signal()
{
    GDBusMessage *message(g_dbus_message_new());

    g_dbus_message_set_message_type(message, G_DBUS_MESSAGE_TYPE_SIGNAL);

    return DBusMessagePtr(message, false);
}
void DBusMessagePtr::set_path(const std::string &path)
{
    g_dbus_message_set_path(get(), path.c_str());
}

std::string DBusMessagePtr::get_path() const
{
    const char *const path(g_dbus_message_get_path(get()));

    if (path) {
        return std::string(path);
    } else {
        return std::string();
    }
}

void DBusMessagePtr::set_interface(const std::string &iface)
{
    g_dbus_message_set_interface(get(), iface.c_str());
}

std::string DBusMessagePtr::get_interface() const
{
    const char *const iface(g_dbus_message_get_interface(get()));

    if (iface) {
        return std::string(iface);
    } else {
        return std::string();
    }
}

void DBusMessagePtr::set_member(const std::string &member)
{
    g_dbus_message_set_member(get(), member.c_str());
}

std::string DBusMessagePtr::get_member() const
{
    const char *const member(g_dbus_message_get_member(get()));

    if (member) {
        return std::string(member);
    } else {
        return std::string();
    }
}

DBusConnectionPtr dbus_get_bus_connection(const char *busType,
                                          const char *name,
                                          bool unshared,
                                          DBusErrorCXX *err)
{
    DBusConnectionPtr conn;
    GError* error = NULL;

    if(unshared) {
        char *address = g_dbus_address_get_for_bus_sync(boost::iequals(busType, "SESSION") ?
                                                        G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
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
        conn = DBusConnectionPtr(g_bus_get_sync(boost::iequals(busType, "SESSION") ?
                                                G_BUS_TYPE_SESSION :
                                                G_BUS_TYPE_SYSTEM,
                                                NULL, &error),
                                 false);
        if(conn == NULL) {
            if (err) {
                err->set(error);
            }
            return NULL;
        }
    }

    if(name) {
        g_bus_own_name_on_connection(conn.get(), name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                     NULL, NULL, NULL, NULL);
        g_dbus_connection_set_exit_on_close(conn.get(), TRUE);
    }

    return conn;
}

DBusConnectionPtr dbus_get_bus_connection(const std::string &address,
                                          DBusErrorCXX *err)
{
    GError* error = NULL;
    DBusConnectionPtr conn(g_dbus_connection_new_for_address_sync(address.c_str(),
                                                                  G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                                  NULL, /* GDBusAuthObserver */
                                                                  NULL, /* GCancellable */
                                                                  &error),
                           false);
    if (!conn && err) {
        err->set(error);
    }

    return conn;
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

    g_dbus_server_start(server);
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
    m_server(server),
    m_address(address)
{
}


}
