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
#include <boost/algorithm/string/predicate.hpp>

#include <stdio.h>

void intrusive_ptr_add_ref(DBusConnection  *con)  { dbus_connection_ref(con); }
void intrusive_ptr_release(DBusConnection  *con)  { dbus_connection_unref(con); }
void intrusive_ptr_add_ref(DBusMessage     *msg)  { dbus_message_ref(msg); }
void intrusive_ptr_release(DBusMessage     *msg)  { dbus_message_unref(msg); }
void intrusive_ptr_add_ref(DBusPendingCall *call) { dbus_pending_call_ref (call); }
void intrusive_ptr_release(DBusPendingCall *call) { dbus_pending_call_unref (call); }

namespace GDBusCXX {

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

DBusHandlerResult filter_cb(DBusConnection *conn,
                            DBusMessage *message,
                            void* user_data)
{
    if (user_data != NULL) {
        FilterData* filter_data(static_cast<FilterData*>(user_data));
        DBusMessagePtr message_ptr(message, true);
        DBusConnectionPtr connection_ptr(conn, true);

        if (!filter_data->m_filter(connection_ptr, message_ptr)) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

struct DBusConnectionPtr::Priv
{
  Priv() :
    ids_to_data(),
    counter(0)
  {}

  typedef std::map<unsigned int, FilterData*> FilterIds;

  FilterIds ids_to_data;
  unsigned int counter;
};

void DBusConnectionPtr::init_priv()
{
  priv.reset(new Priv);
}

unsigned int DBusConnectionPtr::add_filter(const DBusConnectionPtr::FilterFunc &filter)
{
    FilterData* filter_data(new FilterData(filter));

    ++priv->counter;
    priv->ids_to_data.insert(std::make_pair(priv->counter, filter_data));
    dbus_connection_add_filter(get(),
                               filter_cb,
                               static_cast<void*>(filter_data),
                               filter_data_free);

    return priv->counter;
}

void DBusConnectionPtr::remove_filter(unsigned int id)
{
    Priv::FilterIds::iterator iter(priv->ids_to_data.find(id));

    if (iter != priv->ids_to_data.end()) {
        dbus_connection_remove_filter(get(), filter_cb, iter->second);
        priv->ids_to_data.erase(iter);
    }
}

void DBusConnectionPtr::send(const DBusMessagePtr &message)
{
    if (!dbus_connection_send(get(), message.get(), 0)) {
        throw std::runtime_error("Failed to send message");
    }
}

// static
DBusMessagePtr DBusMessagePtr::create_empty_signal()
{
    DBusMessage *message(dbus_message_new(DBUS_MESSAGE_TYPE_SIGNAL));

    return DBusMessagePtr(message, false);
}
void DBusMessagePtr::set_path(const std::string &path)
{
    dbus_message_set_path(get(), path.c_str());
}

std::string DBusMessagePtr::get_path() const
{
    const char *const path(dbus_message_get_path(get()));

    if (path) {
        return std::string(path);
    } else {
        return std::string();
    }
}

void DBusMessagePtr::set_interface(const std::string &iface)
{
    dbus_message_set_interface(get(), iface.c_str());
}

std::string DBusMessagePtr::get_interface() const
{
    const char *const iface(dbus_message_get_interface(get()));

    if (iface) {
        return std::string(iface);
    } else {
        return std::string();
    }
}

void DBusMessagePtr::set_member(const std::string &member)
{
    dbus_message_set_member(get(), member.c_str());
}

std::string DBusMessagePtr::get_member() const
{
    const char *const member(dbus_message_get_member(get()));

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
    return DBusConnectionPtr(b_dbus_setup_bus(boost::iequals(busType, "SYSTEM") ? DBUS_BUS_SYSTEM : DBUS_BUS_SESSION,
                                              name, unshared, err),
                             false);
}

DBusConnectionPtr dbus_get_bus_connection(const std::string &address,
                                          DBusErrorCXX *err)
{
    DBusConnectionPtr conn(dbus_connection_open_private(address.c_str(), err), false);
    if (conn) {
        b_dbus_setup_connection(conn.get(), TRUE, NULL);
    }
    return conn;
}


boost::shared_ptr<DBusServerCXX> DBusServerCXX::listen(const std::string &address, DBusErrorCXX *err)
{
    DBusServer *server = NULL;
    const char *realAddr = address.c_str();
    char buffer[80];

    if (address.empty()) {
        realAddr = buffer;
        buffer[0] = 0;
        for (int counter = 1; counter < 100 && !server; counter++) {
            if (*err) {
                g_debug("dbus_server_listen(%s) failed, trying next candidate: %s",
                        buffer, err->message);
                dbus_error_init(err);
            }
            sprintf(buffer, "unix:abstract=gdbuscxx-%d", counter);
            server = dbus_server_listen(realAddr, err);
        }
    } else {
        server = dbus_server_listen(realAddr, err);
    }

    if (!server) {
        return boost::shared_ptr<DBusServerCXX>();
    }

    b_dbus_setup_server(server);
    boost::shared_ptr<DBusServerCXX> res(new DBusServerCXX(server, realAddr));
    dbus_server_set_new_connection_function(server, newConnection, res.get(), NULL);
    return res;
}

void DBusServerCXX::newConnection(DBusServer *server, DBusConnection *newConn, void *data) throw()
{
    DBusServerCXX *me = static_cast<DBusServerCXX *>(data);
    if (me->m_newConnection) {
        try {
            b_dbus_setup_connection(newConn, FALSE, NULL);
            dbus_connection_set_exit_on_disconnect(newConn, FALSE);
            DBusConnectionPtr conn(newConn);
            me->m_newConnection(*me, conn);
        } catch (...) {
            g_error("handling new D-Bus connection failed with C++ exception");
        }
    }
}

DBusServerCXX::DBusServerCXX(DBusServer *server, const std::string &address) :
    m_server(server),
    m_address(address)
{
}

DBusServerCXX::~DBusServerCXX()
{
    if (m_server) {
        dbus_server_disconnect(m_server.get());
    }
}

bool CheckError(const DBusMessagePtr &reply,
                std::string &buffer)
{
    const char* errname = dbus_message_get_error_name (reply.get());
    if (errname) {
        buffer = errname;
        DBusMessageIter iter;
        if (dbus_message_iter_init(reply.get(), &iter)) {
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                buffer += ": ";
                const char *str;
                dbus_message_iter_get_basic(&iter, &str);
                buffer += str;
            }
        }
        return true;
    } else {
        return false;
    }
}

}
