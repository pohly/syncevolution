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
#include <syncevo/gsignond-pipe-stream.h>
#include <syncevo/GuardFD.h>
#include <syncevo/GLibSupport.h>
#include <syncevo/util.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>

SE_GOBJECT_TYPE(GSignondPipeStream)

void intrusive_ptr_add_ref(GDBusConnection *con)  { g_object_ref(con); }
void intrusive_ptr_release(GDBusConnection *con)  { g_object_unref(con); }
void intrusive_ptr_add_ref(GDBusMessage    *msg)  { g_object_ref(msg); }
void intrusive_ptr_release(GDBusMessage    *msg)  { g_object_unref(msg); }

namespace GDBusCXX {

using namespace SyncEvo;

MethodHandler::MethodMap MethodHandler::m_methodMap;
boost::function<void (void)> MethodHandler::m_callback;

void appendArgInfo(GPtrArray *pa, const std::string &type)
{
    // Empty if not used in the current direction (in or out),
    // ignore then.
    if (type.empty()) {
        // TODO: replace runtime check with compile-time check
        // via type specialization... not terribly important, though.
        return;
    }

    GDBusArgInfo *argInfo = g_new0(GDBusArgInfo, 1);
    argInfo->signature = g_strdup(type.c_str());
    argInfo->ref_count = 1;
    g_ptr_array_add(pa, argInfo);
}

struct OwnNameAsyncData
{
    enum State {
        OWN_NAME_WAITING,
        OWN_NAME_OBTAINED,
        OWN_NAME_LOST
    };

    OwnNameAsyncData(const std::string &name,
                     const boost::function<void (bool)> &obtainedCB) :
        m_name(name),
        m_obtainedCB(obtainedCB),
        m_state(OWN_NAME_WAITING)
    {}

    static void busNameAcquired(GDBusConnection *connection,
                                const gchar *name,
                                gpointer userData) throw ()
    {
        boost::shared_ptr<OwnNameAsyncData> *data = static_cast< boost::shared_ptr<OwnNameAsyncData> *>(userData);
        (*data)->m_state = OWN_NAME_OBTAINED;
        try {
            g_debug("got D-Bus name %s", name);
            if ((*data)->m_obtainedCB) {
                (*data)->m_obtainedCB(true);
            }
        } catch (...) {
            (*data)->m_state = OWN_NAME_LOST;
        }
    }

    static void busNameLost(GDBusConnection *connection,
                            const gchar *name,
                            gpointer userData) throw ()
    {
        boost::shared_ptr<OwnNameAsyncData> *data = static_cast< boost::shared_ptr<OwnNameAsyncData> *>(userData);
        (*data)->m_state = OWN_NAME_LOST;
        try {
            g_debug("lost %s %s",
                    connection ? "D-Bus connection for name" :
                    "D-Bus name",
                    name);
            if ((*data)->m_obtainedCB) {
                (*data)->m_obtainedCB(false);
            }
        } catch (...) {
        }
    }

    static void freeData(gpointer userData) throw ()
    {
        delete static_cast< boost::shared_ptr<OwnNameAsyncData> *>(userData);
    }

    static boost::shared_ptr<OwnNameAsyncData> ownName(GDBusConnection *conn,
                                                       const std::string &name,
                                                       boost::function<void (bool)> obtainedCB =
                                                       boost::function<void (bool)>()) {
        boost::shared_ptr<OwnNameAsyncData> data(new OwnNameAsyncData(name, obtainedCB));
        g_bus_own_name_on_connection(conn,
                                     data->m_name.c_str(),
                                     G_BUS_NAME_OWNER_FLAGS_NONE,
                                     OwnNameAsyncData::busNameAcquired,
                                     OwnNameAsyncData::busNameLost,
                                     new boost::shared_ptr<OwnNameAsyncData>(data),
                                     OwnNameAsyncData::freeData);
        return data;
    }

    const std::string m_name;
    const boost::function<void (bool)> m_obtainedCB;
    State m_state;
};

void DBusConnectionPtr::undelay() const
{
    if (!m_name.empty()) {
        g_debug("starting to acquire D-Bus name %s", m_name.c_str());
        boost::shared_ptr<OwnNameAsyncData> data = OwnNameAsyncData::ownName(get(),
                                                                             m_name);
        while (data->m_state == OwnNameAsyncData::OWN_NAME_WAITING) {
            g_main_context_iteration(NULL, true);
        }
        g_debug("done with acquisition of %s", m_name.c_str());
        if (data->m_state == OwnNameAsyncData::OWN_NAME_LOST) {
            throw std::runtime_error("could not obtain D-Bus name - already running?");
        }
    }
    g_dbus_connection_start_message_processing(get());
}

void DBusConnectionPtr::ownNameAsync(const std::string &name,
                                     const boost::function<void (bool)> &obtainedCB) const
{
    OwnNameAsyncData::ownName(get(), name, obtainedCB);
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
            } else {
                g_clear_error(&error);
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
            } else {
                g_clear_error(&error);
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
                                          DBusErrorCXX *err)
{
    // "address" needs to be the file descriptor number set up
    // by DBusServerCXX::listen().
    GuardFD fd(atoi(address.c_str()));
    GSignondPipeStreamCXX stream(gsignond_pipe_stream_new(fd, fd, true),
                                 TRANSFER_REF);
    fd.release();
    GErrorCXX gerror;
    GDBusCXX::DBusConnectionPtr
        conn(g_dbus_connection_new_sync(G_IO_STREAM(stream.get()),
                                        NULL,
                                        G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
                                        NULL,
                                        NULL,
                                        gerror));
    if (!conn && err) {
        err->set(gerror.release());
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

boost::shared_ptr<DBusServerCXX> DBusServerCXX::listen(const NewConnection_t &newConnection, DBusErrorCXX *)
{
    // Create two fds connected via a two-way stream. The parent
    // keeps fd[0] which gets closed automatically when the child
    // execs. The parent closes the child's fd[1] after forking.
    int fds[2];
    int retval = socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, fds);
    if (retval) {
        SE_THROW(StringPrintf("socketpair: %s", strerror(errno)));
    }
    GuardFD parentfd(fds[0]);
    GuardFD childfd(fds[1]);

    // Child must inherit its fd.
    int fdflags;
    if ((fdflags = fcntl(childfd, F_GETFD)) == -1 ||
        fcntl(childfd, F_SETFD, fdflags & ~FD_CLOEXEC)) {
        SE_THROW(StringPrintf("fcntl: %s", strerror(errno)));
    }

    // Out listen "address" is the FD number.
    std::string address = StringPrintf("%d", childfd.get());

    // Transfer ownership of parent fd.
    GSignondPipeStreamCXX stream(gsignond_pipe_stream_new(parentfd, parentfd, true),
                                 TRANSFER_REF);
    parentfd.release();

    GErrorCXX gerror;
    GDBusCXX::DBusConnectionPtr
        connection(g_dbus_connection_new_sync(G_IO_STREAM(stream.get()),
                                              NULL,
                                              G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
                                              NULL,
                                              NULL,
                                              gerror),
                   false);
    if (!connection) {
        gerror.throwError(SE_HERE, "creating GIO D-Bus connection");
    }

    // A fake DBusServerCXX which does nothing more than return the address, aka
    // our FD number, and store data for the idle callback.
    boost::shared_ptr<DBusServerCXX> res(new DBusServerCXX(address));
    res->m_newConnection = newConnection;
    res->m_connection = connection;
    // Will be freed in the idle callback. Caller must have forked by then.
    res->m_childfd = childfd.release();

    // The caller must have some time to set up connection handling and fork.
    // Delay the newConnection() callback until we enter the main event loop
    // again. Callback must be removed when destructing prematurely because it
    // has a plain "this" pointer.
    res->m_connectionIdle = g_idle_add(onIdleOnce, &*res);

    return res;
}

DBusServerCXX::DBusServerCXX(const std::string &address) :
    m_connectionIdle(0),
    m_childfd(-1),
    m_address(address)
{
}

DBusServerCXX::~DBusServerCXX()
{
    if (m_connectionIdle) {
        g_source_remove(m_connectionIdle);
    }
    if (m_childfd >= 0) {
        close(m_childfd);
    }
}

gboolean DBusServerCXX::onIdleOnce(gpointer custom)
{
    DBusServerCXX *me = static_cast<DBusServerCXX *>(custom);
    me->m_connectionIdle = 0;
    me->m_newConnection(*me, me->m_connection);
    me->m_connection.reset();
    close(me->m_childfd);
    me->m_childfd = -1;

    // not again
    return false;
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

void getWatch(ExtractArgs &context,
              boost::shared_ptr<Watch> &value)
{
    std::unique_ptr<Watch> watch(new Watch(context.m_conn));
    watch->activate((context.m_msg && *context.m_msg) ?
                    g_dbus_message_get_sender(*context.m_msg) :
                    context.m_sender);
    value.reset(watch.release());
}

void ExtractArgs::init(GDBusConnection *conn,
                       GDBusMessage **msg,
                       GVariant *msgBody,
                       const char *sender,
                       const char *path,
                       const char *interface,
                       const char *signal)
{
    m_conn = conn;
    m_msg = msg;
    m_sender = sender;
    m_path = path;
    m_interface = interface;
    m_signal = signal;
    if (msgBody != NULL) {
        g_variant_iter_init(&m_iter, msgBody);
    }
}


ExtractArgs::ExtractArgs(GDBusConnection *conn, GDBusMessage *&msg)
{
    init(conn, &msg, g_dbus_message_get_body(msg), NULL, NULL, NULL, NULL);
}

ExtractArgs::ExtractArgs(GDBusConnection *conn,
                         const char *sender,
                         const char *path,
                         const char *interface,
                         const char *signal)
{
    init(conn, NULL, NULL, sender, path, interface, signal);
}

ExtractResponse::ExtractResponse(GDBusConnection *conn, GDBusMessage *msg)
{
    init(conn, NULL, g_dbus_message_get_body(msg),
         g_dbus_message_get_sender(msg), NULL, NULL, NULL);
}

} // namespace GDBusCXX
