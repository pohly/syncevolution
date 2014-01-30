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

/**
 * This file contains everything that a D-Bus server needs to
 * integrate a normal C++ class into D-Bus. Argument and result
 * marshaling is done in wrapper functions which convert directly
 * to normal C++ types (bool, integers, std::string, std::map<>, ...).
 * See dbus_traits for the full list of supported types.
 *
 * Before explaining the binding, some terminology first:
 * - A function has a return type and multiple parameters.
 * - Input parameters are read-only arguments of the function.
 * - The function can return values to the caller via the
 *   return type and output parameters (retvals).
 *
 * The C++ binding roughly looks like this:
 * - Arguments can be passed as plain types or const references:
     void foo(int arg); void bar(const std::string &str);
 * - A single result can be returned as return value:
 *   int foo();
 * - Multiple results can be copied into instances provided by
 *   the wrapper, passed by reference: void foo(std::string &res);
 * - A return value, arguments and retvals can be combined
 *   arbitrarily. In the D-Bus reply the return code comes before
 *   all return values.
 *
 * Asynchronous methods are possible by declaring one parameter as a
 * Result pointer and later calling the virtual function provided by
 * it. Parameter passing of results is less flexible than that of
 * method parameters: the later allows both std::string as well as
 * const std::string &, for results only the const reference is
 * supported. The Result instance is passed as pointer and then owned
 * by the called method.
 *
 * Reference counting via boost::intrusive_ptr ensures that all
 * D-Bus objects are handled automatically internally.
 */


#ifndef INCL_GDBUS_CXX_BRIDGE
#define INCL_GDBUS_CXX_BRIDGE
#include "gdbus-cxx.h"

#include <stdint.h>
#include <gio/gio.h>

#include <map>
#include <list>
#include <vector>
#include <deque>
#include <utility>

#include <boost/bind.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/utility.hpp>

/* The SyncEvolution exception handler must integrate into the D-Bus
 * C++ wrapper. In contrast to the rest of the code, that handler uses
 * some of the internal classes.
 *
 * To keep changes to a minimum while supporting both dbus
 * implementations, this is made to be a define. The intention is to
 * remove the define once the in-tree gdbus is dropped. */
#define DBUS_NEW_ERROR_MSG   g_dbus_message_new_method_error

// allow some code to deal with API differences betwee GDBus C++ for GIO
// and the version for libdbus
#define GDBUS_CXX_GIO 1

// Boost docs want this in the boost:: namespace, but
// that fails with clang 2.9 depending on the inclusion order of
// header files. Global namespace works in all cases.
void intrusive_ptr_add_ref(GDBusConnection       *con);
void intrusive_ptr_release(GDBusConnection       *con);
void intrusive_ptr_add_ref(GDBusMessage          *msg);
void intrusive_ptr_release(GDBusMessage          *msg);

namespace GDBusCXX {

// GDBusCXX aliases for the underlying types.
// Useful for some external dbus_traits which
// need to pass pointers to these types in their
// append()/get() methods without depending on GIO or
// libdbus types.
typedef GDBusConnection connection_type;
typedef GDBusMessage message_type;
typedef GVariantBuilder builder_type;
typedef GVariantIter reader_type;

/**
 * Simple auto_ptr for GVariant.
 */
class GVariantCXX : boost::noncopyable
{
    GVariant *m_var;
 public:
    /** takes over ownership */
    GVariantCXX(GVariant *var = NULL) : m_var(var) {}
    ~GVariantCXX() { if (m_var) { g_variant_unref(m_var); } }

    operator GVariant * () { return m_var; }
    GVariantCXX &operator = (GVariant *var) {
        if (m_var != var) {
            if (m_var) {
                g_variant_unref(m_var);
            }
            m_var = var;
        }
        return *this;
    }
};

class GVariantIterCXX : boost::noncopyable
{
    GVariantIter *m_var;
 public:
    /** takes over ownership */
    GVariantIterCXX(GVariantIter *var = NULL) : m_var(var) {}
    ~GVariantIterCXX() { if (m_var) { g_variant_iter_free(m_var); } }

    operator GVariantIter * () { return m_var; }
    GVariantIterCXX &operator = (GVariantIter *var) {
        if (m_var != var) {
            if (m_var) {
                g_variant_iter_free(m_var);
            }
            m_var = var;
        }
        return *this;
    }
};

class DBusMessagePtr;

inline void throwFailure(const std::string &object,
                         const std::string &operation,
                         GError *error)
{
    std::string description = object;
    if (!description.empty()) {
        description += ": ";
    }
    description += operation;
    if (error) {
        description += ": ";
        description += error->message;
        g_clear_error(&error);
    } else {
        description += " failed";
    }
    throw std::runtime_error(description);
}

class DBusConnectionPtr : public boost::intrusive_ptr<GDBusConnection>
{
    /**
     * Bus name of client, as passed to dbus_get_bus_connection().
     * The name will be requested in dbus_bus_connection_undelay() =
     * undelay(), to give the caller a chance to register objects on
     * the new connection.
     */
    std::string m_name;

 public:
    DBusConnectionPtr() {}
    // connections are typically created once, so increment the ref counter by default
    DBusConnectionPtr(GDBusConnection *conn, bool add_ref = true) :
      boost::intrusive_ptr<GDBusConnection>(conn, add_ref)
      {}

    GDBusConnection *reference(void) throw()
    {
        GDBusConnection *conn = get();
        g_object_ref(conn);
        return conn;
    }

    /**
     * Ensure that all IO is sent out of the process.
     * Blocks. Only use it right before shutting down.
     */
    void flush();

    typedef boost::function<void ()> Disconnect_t;
    void setDisconnect(const Disconnect_t &func);
    // #define GDBUS_CXX_HAVE_DISCONNECT 1

    /**
     * Starts processing of messages,
     * claims the bus name set with addName() or
     * when creating the connection (legacy API,
     * done that way for compatibility with GDBus for libdbus).
     */
    void undelay() const;
    void addName(const std::string &name) { m_name = name; }

    /**
     * Claims another name on the connection.
     *
     * The callback will be invoked with true as
     * parameter once the name was successfully
     * claimed. If that fails, false will be passed.
     *
     * The caller should be prepared to get called
     * again later on, when loosing an already obtained
     * name. Currently this shouldn't happen, though,
     * because name transfer is not enabled when
     * registering the name.
     *
     * The callback is allowed to be empty.
     */
    void ownNameAsync(const std::string &name,
                      const boost::function<void (bool)> &obtainedCB) const;
};

class DBusMessagePtr : public boost::intrusive_ptr<GDBusMessage>
{
 public:
    DBusMessagePtr() {}
    // expected to be used for messages created anew,
    // so use the reference already incremented for us
    // and don't increment by default
    DBusMessagePtr(GDBusMessage *msg, bool add_ref = false) :
      boost::intrusive_ptr<GDBusMessage>(msg, add_ref)
    {}

    GDBusMessage *reference(void) throw()
    {
        GDBusMessage *msg = get();
        g_object_ref(msg);
        return msg;
    }
};

/**
 * wrapper around GError which initializes
 * the struct automatically, then can be used to
 * throw an exception
 */
class DBusErrorCXX
{
    GError *m_error;
 public:
    DBusErrorCXX(GError *error = NULL)
    : m_error(error)
    {
    }
    DBusErrorCXX(const DBusErrorCXX &dbus_error)
    : m_error(NULL)
    {
        if (dbus_error.m_error) {
            m_error = g_error_copy (dbus_error.m_error);
        }
    }

    DBusErrorCXX & operator=(const DBusErrorCXX &dbus_error)
    {
        if (this != &dbus_error) {
            set(dbus_error.m_error ?
                g_error_copy(dbus_error.m_error) :
                NULL);
        }
        return *this;
    }

    void set(GError *error) {
        if (m_error) {
            g_error_free (m_error);
        }
        m_error = error;
    }

    ~DBusErrorCXX() {
        if (m_error) {
            g_error_free (m_error);
        }
    }

    void throwFailure(const std::string &operation, const std::string &explanation = " failed")
    {
        std::string error_message(m_error ? (std::string(": ") + m_error->message) : "");
        throw std::runtime_error(operation + explanation + error_message);
    }

    std::string getMessage() const { return m_error ? m_error->message : ""; }
};

DBusConnectionPtr dbus_get_bus_connection(const char *busType,
                                          const char *name,
                                          bool unshared,
                                          DBusErrorCXX *err);

DBusConnectionPtr dbus_get_bus_connection(const std::string &address,
                                          DBusErrorCXX *err,
                                          bool delayed = false);

inline void dbus_bus_connection_undelay(const DBusConnectionPtr &conn) { conn.undelay(); }

/**
 * Wrapper around DBusServer. Does intentionally not expose
 * any of the underlying methods so that the public API
 * can be implemented differently for GIO libdbus.
 */
class DBusServerCXX : private boost::noncopyable
{
 public:
    ~DBusServerCXX();

    /**
     * Called for each new connection. Callback must store the DBusConnectionPtr,
     * otherwise it will be unref'ed after the callback returns.
     * If the new connection is not wanted, then it is good style to close it
     * explicitly in the callback.
     */
    typedef boost::function<void (DBusServerCXX &, DBusConnectionPtr &)> NewConnection_t;

    void setNewConnectionCallback(const NewConnection_t &newConnection) { m_newConnection = newConnection; }
    NewConnection_t getNewConnectionCallback() const { return m_newConnection; }

    /**
     * Start listening for new connections on the given address, like unix:abstract=myaddr.
     * Address may be empty, in which case a new, unused address will chosen.
     */
    static boost::shared_ptr<DBusServerCXX> listen(const std::string &address, DBusErrorCXX *err);

    /**
     * address used by the server
     */
    std::string getAddress() const { return m_address; }

 private:
    DBusServerCXX(GDBusServer *server, const std::string &address);
    static gboolean newConnection(GDBusServer *server, GDBusConnection *newConn, void *data) throw();

    NewConnection_t m_newConnection;
    boost::intrusive_ptr<GDBusServer> m_server;
    std::string m_address;
};

/**
 * Special type for object paths. A string in practice.
 */
class DBusObject_t : public std::string
{
 public:
    DBusObject_t() {}
    template <class T> DBusObject_t(T val) : std::string(val) {}
    template <class T> DBusObject_t &operator = (T val) { assign(val); return *this; }
};

/**
 * specializations of this must defined methods for encoding and
 * decoding type C and declare its signature
 */
template<class C> struct dbus_traits {};

struct dbus_traits_base
{
    /**
     * A C++ method or function can handle a call asynchronously by
     * asking to be passed a "boost::shared_ptr<Result*>" parameter.
     * The dbus_traits for those parameters have "asynchronous" set to
     * true, which skips all processing after calling the method.
     */
    static const bool asynchronous = false;
};

/**
 * Append a varying number of parameters as result to the
 * message, using AppendRetvals(msg) << res1 << res2 << ...;
 *
 * Types can be anything that has a dbus_traits, including
 * types which are normally recognized as input parameters in D-Bus
 * method calls.
 */
class AppendRetvals {
    GDBusMessage *m_msg;
    GVariantBuilder m_builder;

 public:
    AppendRetvals(DBusMessagePtr &msg) {
        m_msg = msg.get();
        g_variant_builder_init(&m_builder, G_VARIANT_TYPE_TUPLE);
    }

    ~AppendRetvals()
    {
        g_dbus_message_set_body(m_msg, g_variant_builder_end(&m_builder));
    }

    template<class A> AppendRetvals & operator << (const A &a) {
        dbus_traits<A>::append(m_builder, a);
        return *this;
    }
};

/**
 * Append a varying number of method parameters as result to the reply
 * message, using AppendArgs(msg) << Set<A1>(res1) << Set<A2>(res2) << ...;
 */
struct AppendArgs {
    GDBusMessage *m_msg;
    GVariantBuilder m_builder;

    AppendArgs(const std::auto_ptr<GDBusMessage> &msg) {
        m_msg = msg.get();
        if (!m_msg) {
            throw std::runtime_error("NULL GDBusMessage reply");
        }
        g_variant_builder_init(&m_builder, G_VARIANT_TYPE_TUPLE);
    }

    ~AppendArgs() {
        g_dbus_message_set_body(m_msg, g_variant_builder_end(&m_builder));
    }

    /** syntactic sugar: redirect << into Set instance */
    template<class A> AppendArgs & operator << (const A &a) {
        return a.set(*this);
    }

    /**
     * Always append argument, including those types which
     * would be recognized by << as parameters and thus get
     * skipped.
     */
    template<class A> AppendArgs & operator + (const A &a) {
        dbus_traits<A>::append(m_builder, a);
        return *this;
    }
};

/** default: skip it, not a result of the method */
template<class A> struct Set
{
    Set(typename dbus_traits<A>::host_type &a) {}
    AppendArgs &set(AppendArgs &context) const {
        return context;
    }
};

/** same for const reference */
template<class A> struct Set <const A &>
{
    Set(typename dbus_traits<A>::host_type &a) {}
    AppendArgs &set(AppendArgs &context) const {
        return context;
    }
};

/** specialization for reference: marshal result */
template<class A> struct Set <A &>
{
    typename dbus_traits<A>::host_type &m_a;
    Set(typename dbus_traits<A>::host_type &a) : m_a(a) {}
    AppendArgs &set(AppendArgs &context) const {
        dbus_traits<A>::append(context.m_builder, m_a);
        return context;
    }
};

/**
 * Extract values from a message, using ExtractArgs(conn, msg) >> Get<A1>(val1) >> Get<A2>(val2) >> ...;
 *
 * This complements AppendArgs: it skips over those method arguments
 * which are results of the method. Which values are skipped and
 * which are marshalled depends on the specialization of Get and thus
 * ultimately on the prototype of the method.
 */
struct ExtractArgs {
    // always set
    GDBusConnection *m_conn;

    // only set when handling a method call
    GDBusMessage **m_msg;

    // only set for method call or response
    GVariantIter m_iter;

    // only set when m_msg is NULL (happens when handling signal)
    const char *m_sender;
    const char *m_path;
    const char *m_interface;
    const char *m_signal;

protected:
    void init(GDBusConnection *conn,
              GDBusMessage **msg,
              GVariant *msgBody,
              const char *sender,
              const char *path,
              const char *interface,
              const char *signal);

    ExtractArgs() {}

 public:
    /** constructor for parsing a method invocation message, which must not be NULL */
    ExtractArgs(GDBusConnection *conn, GDBusMessage *&msg);

    /** constructor for parsing signal parameters */
    ExtractArgs(GDBusConnection *conn,
                const char *sender,
                const char *path,
                const char *interface,
                const char *signal);

    /** syntactic sugar: redirect >> into Get instance */
    template<class A> ExtractArgs & operator >> (const A &a) {
        return a.get(*this);
    }
};

/**
 * Need separate class because overloading ExtractArgs constructor
 * with "GDBusMessage &*msg" (for method calls and its special
 * DBusResult semantic) and "GDBusMessage *msg" (here) is not
 * possible. We can't just use the former in, for example,
 * Ret1Traits::demarshal() because we only have a temporary value on
 * the stack to bind the reference to.
 */
class ExtractResponse : public ExtractArgs
{
 public:
    /** constructor for message response */
    ExtractResponse(GDBusConnection *conn, GDBusMessage *msg);
};

/** default: extract data from message */
template<class A> struct Get
{
    typename dbus_traits<A>::host_type &m_a;
    Get(typename dbus_traits<A>::host_type &a) : m_a(a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        dbus_traits<A>::get(context, context.m_iter, m_a);
        return context;
    }
};

/** same for const reference */
template<class A> struct Get <const A &>
{
    typename dbus_traits<A>::host_type &m_a;
    Get(typename dbus_traits<A>::host_type &a) : m_a(a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        dbus_traits<A>::get(context, context.m_iter, m_a);
        return context;
    }
};

/** specialization for reference: skip it, not an input parameter */
template<class A> struct Get <A &>
{
    Get(typename dbus_traits<A>::host_type &a) {}
    ExtractArgs &get(ExtractArgs &context) const {
        return context;
    }
};

/**
 * combines D-Bus connection, path and interface
 */
class DBusObject
{
 protected:
    DBusConnectionPtr m_conn;
    DBusObject_t m_path;
    std::string m_interface;

 private:
    bool m_closeConnection;

 public:
    /**
     * @param closeConnection    set to true if the connection
     *                           is private and this instance of
     *                           DBusObject is meant to be the
     *                           last user of the connection;
     *                           when this DBusObject deconstructs,
     *                           it'll close the connection
     *                           (required  by libdbus for private
     *                           connections; the mechanism in GDBus for
     *                           this didn't work)
     */
    DBusObject(const DBusConnectionPtr &conn,
               const std::string &path,
               const std::string &interface,
               bool closeConnection = false) :
        m_conn(conn),
        m_path(path),
        m_interface(interface),
        m_closeConnection(closeConnection)
    {}
    virtual ~DBusObject() {
        if (m_closeConnection &&
            m_conn) {
            // TODO: is this also necessary for GIO GDBus?
            // dbus_connection_close(m_conn.get());
        }
    }

    GDBusConnection *getConnection() const { return m_conn.get(); }
    const char *getPath() const { return m_path.c_str(); }
    const DBusObject_t &getObject() const { return m_path; }
    const char *getInterface() const { return m_interface.c_str(); }
};

/**
 * adds destination to D-Bus connection, path and interface
 */
class DBusRemoteObject : public DBusObject
{
 protected:
    std::string m_destination;
public:
    DBusRemoteObject(const DBusConnectionPtr &conn,
                     const std::string &path,
                     const std::string &interface,
                     const std::string &destination,
                     bool closeConnection = false) :
    DBusObject(conn, path, interface, closeConnection),
        m_destination(destination)
    {}

    const char *getDestination() const { return m_destination.c_str(); }
};

template<bool optional> class EmitSignalHelper
{
 protected:
    const DBusObject &m_object;
    const std::string m_signal;

    EmitSignalHelper(const DBusObject &object,
                     const std::string &signal) :
       m_object(object),
       m_signal(signal)
       {}


    void sendMsg(const DBusMessagePtr &msg)
    {
        if (optional) {
            g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                           G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
        } else {
            if (!msg) {
                throwFailure(m_signal, "g_dbus_message_new_signal()", NULL);
            }

            GError *error = NULL;
            if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                                G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error)) {
                throwFailure(m_signal, "g_dbus_connection_send_message()", error);
            }
        }
    }
};

template<bool optional = false> class EmitSignal0Template : private EmitSignalHelper<optional>
{
 public:
    EmitSignal0Template(const DBusObject &object,
                        const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () ()
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());

        entry->ref_count = 1;
        return entry;
    }
};

typedef EmitSignal0Template<false> EmitSignal0;

void appendArgInfo(GPtrArray *pa, const std::string &type);

template <typename Arg>
void appendNewArg(GPtrArray *pa)
{
    // "in" direction
    appendArgInfo(pa, dbus_traits<Arg>::getSignature());
}

template <typename Arg>
void appendNewArgForReply(GPtrArray *pa)
{
    // "out" direction
    appendArgInfo(pa, dbus_traits<Arg>::getReply());
}

template <typename Arg>
void appendNewArgForReturn(GPtrArray *pa)
{
    // "out" direction, type must not be skipped
    appendArgInfo(pa, dbus_traits<Arg>::getType());
}

template <typename A1, bool optional = false>
class EmitSignal1 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal1(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template <typename A1, typename A2, bool optional = false>
class EmitSignal2 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal2(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1, A2 a2)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1 << a2;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template <typename A1, typename A2, typename A3, bool optional = false>
class EmitSignal3 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal3(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1, A2 a2, A3 a3)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1 << a2 << a3;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template <typename A1, typename A2, typename A3, typename A4, bool optional = false>
class EmitSignal4 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal4(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1 << a2 << a3 << a4;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5, bool optional = false>
class EmitSignal5 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal5(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        appendNewArg<A5>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6, bool optional = false>
class EmitSignal6 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal6(const DBusObject &object,
                const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", NULL);
        }
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6;
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_sized_new(7);
        appendNewArg<A1>(args);
        appendNewArg<A2>(args);
        appendNewArg<A3>(args);
        appendNewArg<A4>(args);
        appendNewArg<A5>(args);
        appendNewArg<A6>(args);
        g_ptr_array_add(args, NULL);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

struct FunctionWrapperBase {
    void* m_func_ptr;
    FunctionWrapperBase(void* func_ptr)
    : m_func_ptr(func_ptr)
    {}

    virtual ~FunctionWrapperBase() {}
};

template<typename M>
struct FunctionWrapper : public FunctionWrapperBase {
    FunctionWrapper(boost::function<M>* func_ptr)
    : FunctionWrapperBase(reinterpret_cast<void*>(func_ptr))
    {}

    virtual ~FunctionWrapper() {
        delete reinterpret_cast<boost::function<M>*>(m_func_ptr);
    }
};

struct MethodHandler
{
    typedef GDBusMessage *(*MethodFunction)(GDBusConnection *conn, GDBusMessage *msg, void *data);
    typedef boost::shared_ptr<FunctionWrapperBase> FuncWrapper;
    typedef std::pair<MethodFunction, FuncWrapper > CallbackPair;
    typedef std::map<const std::string, CallbackPair > MethodMap;
    static MethodMap m_methodMap;
    static boost::function<void (void)> m_callback;

    static std::string make_prefix(const char *object_path) {
        return std::string(object_path) + "~";
    }

    static std::string make_method_key(const char *object_path,
                                       const char *method_name) {
        return make_prefix(object_path) + method_name;
    }

    static void handler(GDBusConnection       *connection,
                        const gchar           *sender,
                        const gchar           *object_path,
                        const gchar           *interface_name,
                        const gchar           *method_name,
                        GVariant              *parameters,
                        GDBusMethodInvocation *invocation,
                        gpointer               user_data)
    {
        MethodMap::iterator it;
        it = m_methodMap.find(make_method_key(object_path, method_name));
        if (it == m_methodMap.end()) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                                                       "org.SyncEvolution.NoMatchingMethodName",
                                                       "No methods registered with this name");
            return;
        }

        // http://developer.gnome.org/gio/stable/GDBusConnection.html#GDBusInterfaceMethodCallFunc
        // does not say so explicitly, but it seems that 'invocation' was created for us.
        // If we don't unref it, it leaks (visible in refdbg).
        //
        // The documentation for the class itself says that 'the normal way to obtain a
        // GDBusMethodInvocation object is to receive it as an argument to the
        // handle_method_call()' - note the word 'obtain', which seems to imply ownership.
        // This is consistent with the transfer of ownership to calls like
        // g_dbus_method_invocation_return_dbus_error(), which take over ownership
        // of the invocation instance.
        //
        // Because we work with messages directly for the reply from now on, we
        // unref 'invocation' immediately after referencing the underlying message.
        DBusMessagePtr msg(g_dbus_method_invocation_get_message(invocation), true);
        g_object_unref(invocation);
        // Set to NULL, just to be sure we remember that it is gone.
        // cppcheck-suppress uselessAssignmentPtrArg
        invocation = NULL;

        // We are calling callback because we want to keep server alive as long
        // as possible. This callback is in fact delaying server's autotermination.
        if (m_callback) {
            m_callback();
        }

        MethodFunction methodFunc = it->second.first;
        void *methodData          = reinterpret_cast<void*>(it->second.second->m_func_ptr);
        GDBusMessage *reply;
        reply = (methodFunc)(connection,
                             msg.get(),
                             methodData);

        if (!reply) {
            // probably asynchronous, might also be out-of-memory;
            // either way, don't send a reply now
            return;
        }

        GError *error = NULL;
        g_dbus_connection_send_message(connection,
                                       reply,
                                       G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                       NULL,
                                       &error);
        g_object_unref(reply);
        // Cannot throw an exception, glib event loop won't know what to do with it;
        // pretend that the problem didn't happen.
        if (error != NULL) {
            g_error_free (error);
        }
    }
};

template <class M>
struct MakeMethodEntry
{
    // There is no generic implementation of this method.
    // If you get an error about it missing, then write
    // a specialization for your type M (the method pointer).
    //
    // static GDBusMethodEntry make(const char *name)
};

// Wrapper around g_dbus_method_info_unref or g_dbus_signal_info_unref
// with additional NULL check. The methods themselves crash on NULL,
// which happens when appending the terminating NULL to m_methods/m_signals
// below.
template<class I, void (*f)(I *)> void InfoDestroy(gpointer ptr)
{
    if (ptr) {
        I *info = static_cast<I *>(ptr);
        f(info);
    }
}


/**
 * utility class for registering an interface
 */
class DBusObjectHelper : public DBusObject
{
    // 0 when not registered (= activated).
    guint m_connId;
    GDBusInterfaceInfo m_ifInfo;
    GDBusInterfaceVTable m_ifVTable;

    // These arrays must stay valid as long as we are active,
    // because our GDBusInterfaceInfo points to it without
    // ever freeing the memory.
    GPtrArray *m_methods;
    GPtrArray *m_signals;


 public:
    typedef boost::function<void (void)> Callback_t;

    DBusObjectHelper(DBusConnectionPtr conn,
                     const std::string &path,
                     const std::string &interface,
                     const Callback_t &callback = Callback_t(),
                     bool closeConnection = false) :
        DBusObject(conn, path, interface, closeConnection),
        m_connId(0),
        m_methods(g_ptr_array_new_with_free_func(InfoDestroy<GDBusMethodInfo, g_dbus_method_info_unref>)),
        m_signals(g_ptr_array_new_with_free_func(InfoDestroy<GDBusSignalInfo, g_dbus_signal_info_unref>))
    {
        memset(&m_ifInfo, 0, sizeof(m_ifInfo));
        memset(&m_ifVTable, 0, sizeof(m_ifVTable));
        if (!MethodHandler::m_callback) {
            MethodHandler::m_callback = callback;
        }
    }

    ~DBusObjectHelper()
    {
        deactivate();

        MethodHandler::MethodMap::iterator iter(MethodHandler::m_methodMap.begin());
        MethodHandler::MethodMap::iterator iter_end(MethodHandler::m_methodMap.end());
        MethodHandler::MethodMap::iterator first_to_erase(iter_end);
        MethodHandler::MethodMap::iterator last_to_erase(iter_end);
        const std::string prefix(MethodHandler::make_prefix(getPath()));

        while (iter != iter_end) {
            const bool prefix_equal(!iter->first.compare(0, prefix.size(), prefix));

            if (prefix_equal && (first_to_erase == iter_end)) {
                first_to_erase = iter;
            } else if (!prefix_equal && (first_to_erase != iter_end)) {
                last_to_erase = iter;
                break;
            }
            ++iter;
        }
        if (first_to_erase != iter_end) {
            MethodHandler::m_methodMap.erase(first_to_erase, last_to_erase);
        }

        g_ptr_array_free(m_methods, TRUE);
        g_ptr_array_free(m_signals, TRUE);
    }

    /**
     * binds a member to the this pointer of its instance
     * and invokes it when the specified method is called
     */
    template <class A1, class C, class M> void add(A1 instance, M C::*method, const char *name)
    {
        if (m_connId) {
            throw std::logic_error("You can't add new methods after registration!");
        }

        typedef MakeMethodEntry< boost::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));

        boost::function<M> *func = new boost::function<M>(entry_type::boostptr(method, instance));
        MethodHandler::FuncWrapper wrapper(new FunctionWrapper<M>(func));
        MethodHandler::CallbackPair methodAndData = std::make_pair(entry_type::methodFunction, wrapper);
        const std::string key(MethodHandler::make_method_key(getPath(), name));

        MethodHandler::m_methodMap.insert(std::make_pair(key, methodAndData));
    }

    /**
     * binds a plain function pointer with no additional arguments and
     * invokes it when the specified method is called
     */
    template <class M> void add(M *function, const char *name)
    {
        if (m_connId) {
            throw std::logic_error("You can't add new functions after registration!");
        }

        typedef MakeMethodEntry< boost::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));

        boost::function<M> *func = new boost::function<M>(function);
        MethodHandler::FuncWrapper wrapper(new FunctionWrapper<M>(func));
        MethodHandler::CallbackPair methodAndData = std::make_pair(entry_type::methodFunction,
                                                                   wrapper);
        const std::string key(MethodHandler::make_method_key(getPath(), name));

        MethodHandler::m_methodMap.insert(std::make_pair(key, methodAndData));
    }

    /**
     * add an existing signal entry
     */
    template <class S> void add(const S &s)
    {
        if (m_connId) {
            throw std::logic_error("You can't add new signals after registration!");
        }

        g_ptr_array_add(m_signals, s.makeSignalEntry());
    }

    void activate() {
        // method and signal array must be NULL-terminated.
        if (m_connId) {
            throw std::logic_error("This object was already activated.");
        }
        if (m_methods->len &&
            m_methods->pdata[m_methods->len - 1] != NULL) {
            g_ptr_array_add(m_methods, NULL);
        }
        if (m_signals->len &&
            m_signals->pdata[m_signals->len - 1] != NULL) {
            g_ptr_array_add(m_signals, NULL);
        }
        // Meta data is owned by this instance, not GDBus.
        // This is what most examples do and deviating from that
        // can (did!) lead to memory leaks. For example,
        // the ownership of the method array cannot be transferred
        // to GDBusInterfaceInfo.
        m_ifInfo.ref_count = -1;
        m_ifInfo.name      = const_cast<char *>(getInterface()); // Due to ref_count == -1, m_ifInfo.name is not going to get freed despite the missing const.
        m_ifInfo.methods   = (GDBusMethodInfo **)m_methods->pdata;
        m_ifInfo.signals   = (GDBusSignalInfo **)m_signals->pdata;
        m_ifVTable.method_call = MethodHandler::handler;

        m_connId = g_dbus_connection_register_object(getConnection(),
                                                     getPath(),
                                                     &m_ifInfo,
                                                     &m_ifVTable,
                                                     this,
                                                     NULL,
                                                     NULL);
        if (m_connId == 0) {
            throw std::runtime_error(std::string("g_dbus_connection_register_object() failed for ") +
                                     getPath() + " " + getInterface());
        }
    }

    void deactivate()
    {
        if (m_connId) {
            if (!g_dbus_connection_unregister_object(getConnection(), m_connId)) {
                throw std::runtime_error(std::string("g_dbus_connection_unregister_object() failed for ") +
                                         getPath() + " " + getInterface());
            }
            m_connId = 0;
        }
    }
};


/**
 * to be used for plain parameters like int32_t:
 * treat as arguments which have to be extracted
 * from the GVariants and can be skipped when
 * encoding the reply
 */

struct VariantTypeBoolean { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_BOOLEAN; } };
struct VariantTypeByte    { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_BYTE;    } };
struct VariantTypeInt16   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT16;   } };
struct VariantTypeUInt16  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT16;  } };
struct VariantTypeInt32   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT32;   } };
struct VariantTypeUInt32  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT32;  } };
struct VariantTypeInt64   { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_INT64;   } };
struct VariantTypeUInt64  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_UINT64;  } };
struct VariantTypeDouble  { static const GVariantType* getVariantType() { return G_VARIANT_TYPE_DOUBLE;  } };

#define GDBUS_CXX_QUOTE(x) #x
#define GDBUS_CXX_LINE(l) GDBUS_CXX_QUOTE(l)
#define GDBUS_CXX_SOURCE_INFO __FILE__ ":" GDBUS_CXX_LINE(__LINE__)

template<class host, class VariantTraits> struct basic_marshal : public dbus_traits_base
{
    typedef host host_type;
    typedef host arg_type;

    /**
     * copy value from GVariant iterator into variable
     */
    static void get(ExtractArgs &context,
                    GVariantIter &iter, host &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), VariantTraits::getVariantType())) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        const char *type = g_variant_get_type_string(var);
        g_variant_get(var, type, &value);
    }

    /**
     * copy value into D-Bus iterator
     */
    static void append(GVariantBuilder &builder, arg_type value)
    {
        const gchar *typeStr = g_variant_type_dup_string(VariantTraits::getVariantType());
        g_variant_builder_add(&builder, typeStr, value);
        g_free((gpointer)typeStr);
    }
};

template<> struct dbus_traits<uint8_t> :
public basic_marshal< uint8_t, VariantTypeByte >
{
    /**
     * plain type, regardless of whether used as
     * input or output parameter
     */
    static std::string getType() { return "y"; }

    /**
     * plain type => input parameter => non-empty signature
     */
    static std::string getSignature() {return getType(); }

    /**
     * plain type => not returned to caller
     */
    static std::string getReply() { return ""; }

};

/** if the app wants to use signed char, let it and treat it like a byte */
template<> struct dbus_traits<int8_t> : dbus_traits<uint8_t>
{
    typedef int8_t host_type;
    typedef int8_t arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &value)
    {
        dbus_traits<uint8_t>::get(context, iter, reinterpret_cast<uint8_t &>(value));
    }
};

template<> struct dbus_traits<int16_t> :
    public basic_marshal< int16_t, VariantTypeInt16 >
{
    static std::string getType() { return "n"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<uint16_t> :
    public basic_marshal< uint16_t, VariantTypeUInt16 >
{
    static std::string getType() { return "q"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<int32_t> :
    public basic_marshal< int32_t, VariantTypeInt32 >
{
    static std::string getType() { return "i"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<uint32_t> :
    public basic_marshal< uint32_t, VariantTypeUInt32 >
{
    static std::string getType() { return "u"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<int64_t> :
    public basic_marshal< int64_t, VariantTypeInt64 >
{
    static std::string getType() { return "x"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<uint64_t> :
    public basic_marshal< uint64_t, VariantTypeUInt64 >
{
    static std::string getType() { return "t"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<> struct dbus_traits<double> :
    public basic_marshal< double, VariantTypeDouble >
{
    static std::string getType() { return "d"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};

template<> struct dbus_traits<bool> : public dbus_traits_base
// cannot use basic_marshal because VariantTypeBoolean packs/unpacks
// a gboolean, which is not a C++ bool (4 bytes vs 1 on x86_64)
// public basic_marshal< bool, VariantTypeBoolean >
{
    static std::string getType() { return "b"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    typedef bool host_type;
    typedef bool arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, bool &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), VariantTypeBoolean::getVariantType())) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        gboolean buffer;
        const char *type = g_variant_get_type_string(var);
        g_variant_get(var, type, &buffer);
        value = buffer;
    }

    static void append(GVariantBuilder &builder, bool value)
    {
        const gchar *typeStr = g_variant_type_dup_string(VariantTypeBoolean::getVariantType());
        g_variant_builder_add(&builder, typeStr, (gboolean)value);
        g_free((gpointer)typeStr);
    }
};

template<> struct dbus_traits<std::string> : public dbus_traits_base
{
    static std::string getType() { return "s"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, std::string &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_STRING)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        const char *str = g_variant_get_string(var, NULL);
        value = str;
    }

    static void append(GVariantBuilder &builder, const std::string &value)
    {
        g_variant_builder_add_value(&builder, g_variant_new_string(value.c_str()));
    }

    typedef std::string host_type;
    typedef const std::string &arg_type;
};

template<> struct dbus_traits<const char *> : public dbus_traits_base
{
    static std::string getType() { return "s"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    // Cannot copy into that type. Can only be used for encoding.

    static void append(GVariantBuilder &builder, const char *value)
    {
        g_variant_builder_add_value(&builder, g_variant_new_string(value ? value : ""));
    }

    typedef const char *host_type;
    typedef const char *arg_type;
};

template <> struct dbus_traits<DBusObject_t> : public dbus_traits_base
{
    static std::string getType() { return "o"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, DBusObject_t &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_OBJECT_PATH)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        const char *objPath = g_variant_get_string(var, NULL);
        value = objPath;
    }

    static void append(GVariantBuilder &builder, const DBusObject_t &value)
    {
        if (!g_variant_is_object_path(value.c_str())) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        g_variant_builder_add_value(&builder, g_variant_new_object_path(value.c_str()));
    }

    typedef DBusObject_t host_type;
    typedef const DBusObject_t &arg_type;
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits<Caller_t> : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, Caller_t &value)
    {
        const char *peer = (context.m_msg && *context.m_msg) ?
            g_dbus_message_get_sender(*context.m_msg) :
            context.m_sender;
        if (!peer) {
            throw std::runtime_error("D-Bus method call without sender?!");
        }
        value = peer;
    }

    typedef Caller_t host_type;
    typedef const Caller_t &arg_type;
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits<Path_t> : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, Path_t &value)
    {
        const char *path = (context.m_msg && *context.m_msg) ?
            g_dbus_message_get_path(*context.m_msg) :
            context.m_path;
        if (!path) {
            throw std::runtime_error("D-Bus message without path?!");
        }
        value = path;
    }

    typedef Path_t host_type;
    typedef const Path_t &arg_type;
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits<Interface_t> : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, Interface_t &value)
    {
        const char *path = (context.m_msg && *context.m_msg) ?
            g_dbus_message_get_interface(*context.m_msg) :
            context.m_interface;
        if (!path) {
            throw std::runtime_error("D-Bus message without interface?!");
        }
        value = path;
    }

    typedef Interface_t host_type;
    typedef const Interface_t &arg_type;
};

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits<Member_t> : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, Member_t &value)
    {
        const char *path = (context.m_msg && *context.m_msg) ?
            g_dbus_message_get_member(*context.m_msg) :
            NULL;
        if (!path) {
            throw std::runtime_error("D-Bus message without member?!");
        }
        value = path;
    }

    typedef Member_t host_type;
    typedef const Member_t &arg_type;
};

/**
 * a std::pair - maps to D-Bus struct
 */
template<class A, class B> struct dbus_traits< std::pair<A,B> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<A>::getType() + dbus_traits<B>::getType();
    }
    static std::string getType()
    {
        return "(" + getContainedType() + ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::pair<A,B> host_type;
    typedef const std::pair<A,B> &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &pair)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        dbus_traits<A>::get(context, tupIter, pair.first);
        dbus_traits<B>::get(context, tupIter, pair.second);
    }

    static void append(GVariantBuilder &builder, arg_type pair)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        dbus_traits<A>::append(builder, pair.first);
        dbus_traits<B>::append(builder, pair.second);
        g_variant_builder_close(&builder);
    }
};

/**
 * a boost::tuple - maps to D-Bus struct
 */
template<class A, class B> struct dbus_traits< boost::tuple<A,B> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<A>::getType() + dbus_traits<B>::getType();
    }
    static std::string getType()
    {
        return "(" + getContainedType() + ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef boost::tuple<A,B> host_type;
    typedef const boost::tuple<A,B> &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &t)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        dbus_traits<A>::get(context, tupIter, boost::tuples::get<0>(t));
        dbus_traits<B>::get(context, tupIter, boost::tuples::get<1>(t));
    }

    static void append(GVariantBuilder &builder, arg_type t)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        dbus_traits<A>::append(builder, boost::tuples::get<0>(t));
        dbus_traits<B>::append(builder, boost::tuples::get<1>(t));
        g_variant_builder_close(&builder);
    }
};

/**
 * a boost::tuple - maps to D-Bus struct
 */
template<class A, class B, class C> struct dbus_traits< boost::tuple<A,B,C> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<A>::getType() + dbus_traits<B>::getType() + dbus_traits<C>::getType();
    }
    static std::string getType()
    {
        return "(" + getContainedType() + ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef boost::tuple<A,B,C> host_type;
    typedef const boost::tuple<A,B,C> &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &t)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        dbus_traits<A>::get(context, tupIter, boost::tuples::get<0>(t));
        dbus_traits<B>::get(context, tupIter, boost::tuples::get<1>(t));
        dbus_traits<C>::get(context, tupIter, boost::tuples::get<2>(t));
    }

    static void append(GVariantBuilder &builder, arg_type t)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        dbus_traits<A>::append(builder, boost::tuples::get<0>(t));
        dbus_traits<B>::append(builder, boost::tuples::get<1>(t));
        dbus_traits<C>::append(builder, boost::tuples::get<2>(t));
        g_variant_builder_close(&builder);
    }
};

/**
 * a boost::tuple - maps to D-Bus struct
 */
template<class A, class B, class C, class D> struct dbus_traits< boost::tuple<A,B,C,D> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<A>::getType() + dbus_traits<B>::getType() + dbus_traits<C>::getType() + dbus_traits<D>::getType();
    }
    static std::string getType()
    {
        return "(" + getContainedType() + ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef boost::tuple<A,B,C,D> host_type;
    typedef const boost::tuple<A,B,C,D> &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &t)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        dbus_traits<A>::get(context, tupIter, boost::tuples::get<0>(t));
        dbus_traits<B>::get(context, tupIter, boost::tuples::get<1>(t));
        dbus_traits<C>::get(context, tupIter, boost::tuples::get<2>(t));
        dbus_traits<D>::get(context, tupIter, boost::tuples::get<3>(t));
    }

    static void append(GVariantBuilder &builder, arg_type t)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        dbus_traits<A>::append(builder, boost::tuples::get<0>(t));
        dbus_traits<B>::append(builder, boost::tuples::get<1>(t));
        dbus_traits<C>::append(builder, boost::tuples::get<2>(t));
        dbus_traits<D>::append(builder, boost::tuples::get<3>(t));
        g_variant_builder_close(&builder);
    }
};

/**
 * dedicated type for chunk of data, to distinguish this case from
 * a normal std::pair of two values
 */
template<class V> class DBusArray : public std::pair<size_t, const V *>
{
 public:
     DBusArray() :
        std::pair<size_t, const V *>(0, NULL)
        {}
     DBusArray(size_t len, const V *data) :
        std::pair<size_t, const V *>(len, data)
        {}
};
template<class V> class DBusArray<V> makeDBusArray(size_t len, const V *data) { return DBusArray<V>(len, data); }

/**
 * Pass array of basic type plus its number of entries.
 * Can only be used in cases where the caller owns the
 * memory and can discard it when the call returns, in
 * other words, for method calls, asynchronous replys and
 * signals, but not for return values.
 */
template<class V> struct dbus_traits< DBusArray<V> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<V>::getType();
    }
    static std::string getType()
    {
        return std::string("a") +
            dbus_traits<V>::getType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef DBusArray<V> host_type;
    typedef const host_type &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &array)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        typedef typename dbus_traits<V>::host_type V_host_type;
        gsize nelements;
        V_host_type *data;
        data = (V_host_type *) g_variant_get_fixed_array(var,
                                                         &nelements,
                                                         static_cast<gsize>(sizeof(V_host_type)));
        array.first = nelements;
        array.second = data;
    }

    static void append(GVariantBuilder &builder, arg_type array)
    {
        g_variant_builder_add_value(&builder,
                                    g_variant_new_from_data(G_VARIANT_TYPE(getType().c_str()),
                                                            (gconstpointer)array.second,
                                                            array.first,
                                                            true, // data is trusted to be in serialized form
                                                            NULL, NULL // no need to free data
                                                            ));
    }
};

/**
 * a std::map - treat it like a D-Bus dict
 */
template<class K, class V, class C> struct dbus_traits< std::map<K, V, C> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return std::string("{") +
            dbus_traits<K>::getType() +
            dbus_traits<V>::getType() +
            "}";
    }
    static std::string getType()
    {
        return std::string("a") +
            getContainedType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::map<K, V, C> host_type;
    typedef const host_type &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &dict)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter contIter;
        GVariantCXX child;
        g_variant_iter_init(&contIter, var);
        while((child = g_variant_iter_next_value(&contIter)) != NULL) {
            K key;
            V value;
            GVariantIter childIter;
            g_variant_iter_init(&childIter, child);
            dbus_traits<K>::get(context, childIter, key);
            dbus_traits<V>::get(context, childIter, value);
            dict.insert(std::make_pair(key, value));
        }
    }

    static void append(GVariantBuilder &builder, arg_type dict)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));

        for(typename host_type::const_iterator it = dict.begin();
            it != dict.end();
            ++it) {
            g_variant_builder_open(&builder, G_VARIANT_TYPE(getContainedType().c_str()));
            dbus_traits<K>::append(builder, it->first);
            dbus_traits<V>::append(builder, it->second);
            g_variant_builder_close(&builder);
        }

        g_variant_builder_close(&builder);
    }
};

/**
 * A collection of items (works for std::list, std::vector, std::deque, ...).
 * Maps to D-Bus array, but with inefficient marshaling
 * because we cannot get a base pointer for the whole array.
 */
template<class C, class V> struct dbus_traits_collection : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return dbus_traits<V>::getType();
    }
    static std::string getType()
    {
        return std::string("a") +
            getContainedType();
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef C host_type;
    typedef const C &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &array)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        int nelements = g_variant_n_children(var);
        GVariantIter childIter;
        g_variant_iter_init(&childIter, var);
        for(int i = 0; i < nelements; ++i) {
            V value;
            dbus_traits<V>::get(context, childIter, value);
            array.push_back(value);
        }
    }

    static void append(GVariantBuilder &builder, arg_type array)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));

        for(typename host_type::const_iterator it = array.begin();
            it != array.end();
            ++it) {
            dbus_traits<V>::append(builder, *it);
        }

        g_variant_builder_close(&builder);
    }
};

template<class V> struct dbus_traits< std::vector<V> > : public dbus_traits_collection<std::vector<V>, V> {};
template<class V> struct dbus_traits< std::list<V> > : public dbus_traits_collection<std::list<V>, V> {};
template<class V> struct dbus_traits< std::deque<V> > : public dbus_traits_collection<std::deque<V>, V> {};

struct append_visitor : public boost::static_visitor<>
{
    GVariantBuilder &builder;
    append_visitor(GVariantBuilder &b) : builder(b) {}
    template <class V> void operator()(const V &v) const
    {
        dbus_traits<V>::append(builder, v);
    }
};

/**
 * A boost::variant <V> maps to a dbus variant, only care about values of
 * type V but will not throw error if type is not matched, this is useful if
 * application is interested on only a sub set of possible value types
 * in variant.
 */
template<class BV> struct dbus_variant_base : public dbus_traits_base
{
    static std::string getType() { return "v"; }
    static std::string getSignature() { return getType(); }
    static std::string getReply() { return ""; }

    typedef BV host_type;
    typedef const BV &arg_type;

    static void append(GVariantBuilder &builder, const BV &value)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        boost::apply_visitor(append_visitor(builder), value);
        g_variant_builder_close(&builder);
    }
};


template <class V> struct dbus_traits <boost::variant <V> > :
    public dbus_variant_base< boost::variant <V> >
{
    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::variant <V> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        if (dbus_traits<V>::getSignature() != type) {
            //ignore unrecognized sub type in variant
            return;
        }

        V val;
        // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
        g_variant_iter_init(&varIter, var);
        dbus_traits<V>::get(context, varIter, val);
        value = val;
    }
};

template <class V1, class V2> struct dbus_traits <boost::variant <V1, V2> > :
    public dbus_variant_base< boost::variant <V1, V2> >
{
    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::variant <V1, V2> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        if (dbus_traits<V1>::getSignature() == type) {
            V1 val;
            // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V1>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V2>::getSignature() == type) {
            V2 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V2>::get(context, varIter, val);
            value = val;
        } else {
            // ignore unrecognized sub type in variant
            return;
        }
    }
};

template <class V1, class V2, class V3> struct dbus_traits <boost::variant <V1, V2, V3> > :
    public dbus_variant_base< boost::variant <V1, V2, V3> >
{
    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::variant <V1, V2, V3> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        if (dbus_traits<V1>::getSignature() == type) {
            V1 val;
            // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V1>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V2>::getSignature() == type) {
            V2 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V2>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V3>::getSignature() == type) {
            V3 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V3>::get(context, varIter, val);
            value = val;
        } else {
            // ignore unrecognized sub type in variant
            return;
        }
    }
};

template <class V1, class V2, class V3, class V4> struct dbus_traits <boost::variant <V1, V2, V3, V4> > :
    public dbus_variant_base< boost::variant <V1, V2, V3, V4> >
{
    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::variant <V1, V2, V3> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        if (dbus_traits<V1>::getSignature() == type) {
            V1 val;
            // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V1>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V2>::getSignature() == type) {
            V2 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V2>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V3>::getSignature() == type) {
            V3 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V3>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V4>::getSignature() == type) {
            V4 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V4>::get(context, varIter, val);
            value = val;
        } else {
            // ignore unrecognized sub type in variant
            return;
        }
    }
};

template <class V1, class V2, class V3, class V4, class V5> struct dbus_traits <boost::variant <V1, V2, V3, V4, V5> > :
    public dbus_variant_base< boost::variant <V1, V2, V3, V4, V5> >
{
    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::variant <V1, V2, V3> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        if (dbus_traits<V1>::getSignature() == type) {
            V1 val;
            // Note: Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V1>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V2>::getSignature() == type) {
            V2 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V2>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V3>::getSignature() == type) {
            V3 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V3>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V4>::getSignature() == type) {
            V4 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V4>::get(context, varIter, val);
            value = val;
        } else if (dbus_traits<V5>::getSignature() == type) {
            V5 val;
            g_variant_iter_init(&varIter, var);
            dbus_traits<V5>::get(context, varIter, val);
            value = val;
        } else {
            // ignore unrecognized sub type in variant
            return;
        }
    }
};


/**
 * A recursive variant. Can represent values of a certain type V and
 * vectors with a mixture of such values and the variant. Limiting
 * this to vectors is done for the sake of simplicity and because
 * vector is fairly efficient to work with, in particular when
 * implementing methods.
 *
 * It would be nice to not refer to boost internals here. But using
 * "typename boost::make_recursive_variant<V, std::vector< boost::recursive_variant_ > >::type"
 * instead of the expanded
 * "boost::variant< boost::detail::variant::recursive_flag<V>, A>"
 * leads to a compiler error:
 * class template partial specialization contains a template parameter that can not be deduced; this partial specialization will never be used [-Werror]
 *  ...dbus_traits < typename boost::make_recursive_variant<V, std::vector< boost::recursive_variant_ > >::type > ...
 *     ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
template <class V, class A> struct dbus_traits < boost::variant<boost::detail::variant::recursive_flag<V>, A>  > :  public dbus_traits_base
{
    static std::string getType() { return "v"; }
    static std::string getSignature() { return getType(); }
    static std::string getReply() { return ""; }

    typedef boost::variant<boost::detail::variant::recursive_flag<V>, A> host_type;
    typedef const host_type &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &value)
    {
        GVariantIterCXX itercopy(g_variant_iter_copy(&iter));

        // Peek at next element, then decide what to do with it.
        GVariantCXX var(g_variant_iter_next_value(&iter));
        // We accept a variant, the plain type V, or an array.
        // This is necessary for clients like Python which
        // send [['foo', 'bar']] as 'aas' when seeing 'v'
        // as signature.
        if (var == NULL) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        const char *type = g_variant_get_type_string(var);
        if (!strcmp(type, "v")) {
            // Strip the outer variant and decode the inner value recursively, in
            // our own else branch.
            GVariantIter varIter;
            g_variant_iter_init(&varIter, var);
            dbus_traits<host_type>::get(context, varIter, value);
        } else if (dbus_traits<V>::getSignature() == type) {
            V val;
            dbus_traits<V>::get(context, *itercopy, val);
            value = val;
        } else if (type[0] == 'a') {
            std::vector<host_type> val;
            dbus_traits< std::vector<host_type> >::get(context, *itercopy, val);
            value = val;
        } else if (type[0] == '(') {
            // Treat a tuple like an array. We have to iterate ourself here.
            std::vector<host_type> val;
            GVariantIter tupIter;
            g_variant_iter_init(&tupIter, var);
            GVariantIterCXX copy(g_variant_iter_copy(&tupIter));
            // Step through the elements in lockstep. We need this
            // because we must not call the get() method when there is
            // nothing to unpack.
            while (GVariantCXX(g_variant_iter_next_value(copy))) {
                host_type tmp;
                dbus_traits<host_type>::get(context, tupIter, tmp);
                val.push_back(tmp);
            }
            value = val;
        } else {
            // More strict than the other variants, because it is mostly used for
            // method calls where we don't want to silently ignore parts of the
            // parameter.
            throw std::runtime_error(std::string("expected recursive variant containing " + dbus_traits<V>::getSignature() + ", got " + type));
            return;
        }
    }

    static void append(GVariantBuilder &builder, const boost::variant<V> &value)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        boost::apply_visitor(append_visitor(builder), value);
        g_variant_builder_close(&builder);
    }

};

/**
 * a single member m of type V in a struct K
 */
template<class K, class V, V K::*m> struct dbus_member_single
{
    static std::string getType()
    {
        return dbus_traits<V>::getType();
    }
    typedef V host_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, K &val)
    {
        dbus_traits<V>::get(context, iter, val.*m);
    }

    static void append(GVariantBuilder &builder, const K &val)
    {
        dbus_traits<V>::append(builder, val.*m);
    }
};

/**
 * a member m of type V in a struct K, followed by another dbus_member
 * or dbus_member_single to end the chain
 */
template<class K, class V, V K::*m, class M> struct dbus_member
{
    static std::string getType()
    {
        return dbus_traits<V>::getType() + M::getType();
    }
    typedef V host_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, K &val)
    {
        dbus_traits<V>::get(context, iter, val.*m);
        M::get(context, iter, val);
    }

    static void append(GVariantBuilder &builder, const K &val)
    {
        dbus_traits<V>::append(builder, val.*m);
        M::append(builder, val);
    }
};

/**
 * a helper class which implements dbus_traits for
 * a class, use with:
 * struct foo { int a; std::string b;  };
 * template<> struct dbus_traits< foo > : dbus_struct_traits< foo,
 *                                                            dbus_member<foo, int, &foo::a,
 *                                                            dbus_member_single<foo, std::string, &foo::b> > > {};
 */
template<class K, class M> struct dbus_struct_traits : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return M::getType();
    }
    static std::string getType()
    {
        return std::string("(") +
            getContainedType() +
            ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef K host_type;
    typedef const K &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &val)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == NULL || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        M::get(context, tupIter, val);
    }

    static void append(GVariantBuilder &builder, arg_type val)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        M::append(builder, val);
        g_variant_builder_close(&builder);
    }
};

/**
 * a helper class which implements dbus_traits for an enum,
 * parameterize it with the enum type and an integer type
 * large enough to hold all valid enum values
 */
template<class E, class I> struct dbus_enum_traits : public dbus_traits<I>
{
    typedef E host_type;
    typedef E arg_type;

    // cast from enum to int in append() is implicit; in
    // get() we have to make it explicit
    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &val)
    {
        I ival;
        dbus_traits<I>::get(context, iter, ival);
        val = static_cast<E>(ival);
    }
};


/**
 * special case const reference parameter:
 * treat like pass-by-value input argument
 *
 * Example: const std::string &arg
 */
template<class C> struct dbus_traits<const C &> : public dbus_traits<C> {};

/**
 * special case writeable reference parameter:
 * must be a return value
 *
 * Example: std::string &retval
 */
template<class C> struct dbus_traits<C &> : public dbus_traits<C>
{
    static std::string getSignature() { return ""; }
    static std::string getReply() { return dbus_traits<C>::getType(); }
};

/**
 * dbus-cxx base exception thrown in dbus server
 * org.syncevolution.gdbuscxx.Exception
 * This base class only contains interfaces, no data members
 */
class DBusCXXException
{
 public:
    /**
     * get exception name, used to convert to dbus error name
     * subclasses should override it
     */
    virtual std::string getName() const { return "org.syncevolution.gdbuscxx.Exception"; }

    /**
     * get error message
     */
    virtual const char* getMessage() const { return "unknown"; }
};

static GDBusMessage *handleException(GDBusMessage *&callerMsg)
{
    // We provide a reply to the message. Clear the "msg" variable
    // in our caller's context to make it as done.
    GDBusMessage *msg = callerMsg;
    callerMsg = NULL;

    try {
#ifdef DBUS_CXX_EXCEPTION_HANDLER
        return DBUS_CXX_EXCEPTION_HANDLER(msg);
#else
        throw;
#endif
    } catch (const dbus_error &ex) {
        return g_dbus_message_new_method_error_literal(msg, ex.dbusName().c_str(), ex.what());
    } catch (const DBusCXXException &ex) {
        return g_dbus_message_new_method_error_literal(msg, ex.getName().c_str(), ex.getMessage());
    } catch (const std::runtime_error &ex) {
        return g_dbus_message_new_method_error_literal(msg, "org.syncevolution.gdbuscxx.Exception", ex.what());
    } catch (...) {
        return g_dbus_message_new_method_error_literal(msg, "org.syncevolution.gdbuscxx.Exception", "unknown");
    }
}

/**
 * Check presence of a certain D-Bus client.
 */
class Watch : private boost::noncopyable
{
    DBusConnectionPtr m_conn;
    boost::function<void (void)> m_callback;
    bool m_called;
    guint m_watchID;
    std::string m_peer;

    static void nameOwnerChanged(GDBusConnection *connection,
                                 const gchar *sender_name,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *signal_name,
                                 GVariant *parameters,
                                 gpointer user_data);

    void disconnected();

 public:
    Watch(const DBusConnectionPtr &conn,
          const boost::function<void (void)> &callback = boost::function<void (void)>());
    ~Watch();

    /**
     * Changes the callback triggered by this Watch.  If the watch has
     * already fired, the callback is invoked immediately.
     */
    void setCallback(const boost::function<void (void)> &callback);

    /**
     * Starts watching for disconnect of that peer
     * and also checks whether it is currently
     * still around.
     */
    void activate(const char *peer);
};

void getWatch(ExtractArgs &context, boost::shared_ptr<Watch> &value);

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits< boost::shared_ptr<Watch> >  : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, boost::shared_ptr<Watch> &value) { getWatch(context, value); }
    static void append(GVariantBuilder &builder, const boost::shared_ptr<Watch> &value) {}

    typedef boost::shared_ptr<Watch> host_type;
    typedef const boost::shared_ptr<Watch> &arg_type;
};

/**
 * base class for D-Bus results,
 * keeps references to required objects and provides the
 * failed() method
 */
class DBusResult : virtual public Result
{
 protected:
    DBusConnectionPtr m_conn;     /**< connection via which the message was received */
    DBusMessagePtr m_msg;         /**< the method invocation message */
    bool m_haveOwnership;         /**< this class is responsible for sending a method reply */
    bool m_replied;               /**< a response was sent */

    void sendMsg(const DBusMessagePtr &msg)
    {
        m_replied = true;
        GError *error = NULL;
        if (!g_dbus_connection_send_message(m_conn.get(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error)) {
            throwFailure("", "g_dbus_connection_send_message()", error);
        }
    }

 public:
    DBusResult(GDBusConnection *conn,
               GDBusMessage *msg) :
        m_conn(conn, true),
        m_msg(msg, true),
        m_haveOwnership(false),
        m_replied(false)
    {}

    ~DBusResult()
    {
        if (m_haveOwnership && !m_replied) {
            try {
                failed(dbus_error("org.syncevolution.gdbus", "processing the method call failed"));
            } catch (...) {
                // Ignore failure, we are probably shutting down
                // anyway, which will tell the caller that the
                // method won't be processed.
            }
        }
    }

    void transferOwnership() throw ()
    {
        m_haveOwnership = true;
    }

    virtual void failed(const dbus_error &error)
    {
        DBusMessagePtr errMsg(g_dbus_message_new_method_error(m_msg.get(), error.dbusName().c_str(),
                                                              "%s", error.what()));
        sendMsg(errMsg);
    }

    virtual Watch *createWatch(const boost::function<void (void)> &callback)
    {
        std::auto_ptr<Watch> watch(new Watch(m_conn, callback));
        watch->activate(g_dbus_message_get_sender(m_msg.get()));
        return watch.release();
    }
};

class DBusResult0 :
    public Result0,
    public DBusResult
{
 public:
    DBusResult0(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done()
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        sendMsg(reply);
    }

    static std::string getSignature() { return ""; }
};

template <typename A1>
class DBusResult1 :
    public Result1<A1>,
    public DBusResult
{
 public:
    DBusResult1(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1;
        sendMsg(reply);
    }

    static std::string getSignature() { return dbus_traits<A1>::getSignature(); }

    static const bool asynchronous = dbus_traits<A1>::asynchronous;
};

template <typename A1, typename A2>
class DBusResult2 :
    public Result2<A1, A2>,
    public DBusResult
{
 public:
    DBusResult2(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult1<A2>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult1<A2>::asynchronous;
};

template <typename A1, typename A2, typename A3>
class DBusResult3 :
    public Result3<A1, A2, A3>,
    public DBusResult
{
 public:
    DBusResult3(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult2<A2, A3>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult2<A2, A3>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4>
class DBusResult4 :
    public Result4<A1, A2, A3, A4>,
    public DBusResult
{
 public:
    DBusResult4(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4;
        sendMsg(reply);
    }
    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult3<A2, A3, A4>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult3<A2, A3, A4>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5>
class DBusResult5 :
    public Result5<A1, A2, A3, A4, A5>,
    public DBusResult
{
 public:
    DBusResult5(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult4<A2, A3, A4, A5>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult4<A2, A3, A4, A5>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6>
class DBusResult6 :
    public Result6<A1, A2, A3, A4, A5, A6>,
    public DBusResult
{
 public:
    DBusResult6(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5 << a6;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult5<A2, A3, A4, A5, A6>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult5<A2, A3, A4, A5, A6>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7>
class DBusResult7 :
    public Result7<A1, A2, A3, A4, A5, A6, A7>,
    public DBusResult
{
 public:
    DBusResult7(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5 << a6 << a7;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult6<A2, A3, A4, A5, A6, A7>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult6<A2, A3, A4, A5, A6, A7>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8>
class DBusResult8 :
    public Result8<A1, A2, A3, A4, A5, A6, A7, A8>,
    public DBusResult
{
 public:
    DBusResult8(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult7<A2, A3, A4, A5, A6, A7, A8>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult7<A2, A3, A4, A5, A6, A7, A8>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8, typename A9>
class DBusResult9 :
    public Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9>,
    public DBusResult
{
 public:
    DBusResult9(GDBusConnection *conn,
                GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult8<A2, A3, A4, A5, A6, A7, A8, A9>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult8<A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;
};

template <typename A1, typename A2, typename A3, typename A4, typename A5,
          typename A6, typename A7, typename A8, typename A9, typename A10>
class DBusResult10 :
    public Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10>,
    public DBusResult
{
 public:
    DBusResult10(GDBusConnection *conn,
                 GDBusMessage *msg) :
        DBusResult(conn, msg)
    {}

    virtual void done(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9, A10 a10)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
        sendMsg(reply);
    }

    static std::string getSignature() {
        return dbus_traits<A1>::getSignature() +
            DBusResult9<A2, A3, A4, A5, A6, A7, A8, A9, A10>::getSignature();
    }

    static const bool asynchronous =
        dbus_traits<A1>::asynchronous ||
        DBusResult9<A2, A3, A4, A5, A6, A7, A8, A9, A10>::asynchronous;
};

/**
 * Helper class for constructing a DBusResult: while inside the
 * initial method call handler, we have a try/catch block which will
 * reply to the caller. Once we leave that block, this class here
 * destructs and transfers the responsibility for sending a reply to
 * the DBusResult instance.
 */
template <class DBusR> class DBusResultGuard : public boost::shared_ptr<DBusR>
{
    GDBusMessage **m_msg;
 public:
     DBusResultGuard() : m_msg(NULL) {}
    ~DBusResultGuard() throw ()
    {
        DBusR *result = boost::shared_ptr<DBusR>::get();
        // Our caller has not cleared its "msg" instance,
        // which means that from now on it will be our
        // responsibility to provide a response.
        if (result && m_msg && *m_msg) {
            result->transferOwnership();
        }
    }

    void initDBusResult(ExtractArgs &context)
    {
        m_msg = context.m_msg;
        boost::shared_ptr<DBusR>::reset(new DBusR(context.m_conn, context.m_msg ? *context.m_msg : NULL));
    }
};

/**
 * A parameter which points towards one of our Result* structures.
 * All of the types contained in it count towards the Reply signature.
 * The requested Result type itself is constructed here.
 *
 * @param R      Result0, Result1<type>, ...
 * @param DBusR  the class implementing R
 */
template <class R, class DBusR> struct dbus_traits_result
{
    static std::string getType() { return DBusR::getSignature(); }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return getType(); }

    typedef DBusResultGuard<DBusR> host_type;
    typedef boost::shared_ptr<R> &arg_type;
    static const bool asynchronous = true;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &value)
    {
        value.initDBusResult(context);
    }
};

template <>
struct dbus_traits< boost::shared_ptr<Result0> > :
    public dbus_traits_result<Result0, DBusResult0>
{};
template <class A1>
struct dbus_traits< boost::shared_ptr< Result1<A1> > >:
    public dbus_traits_result< Result1<A1>, DBusResult1<A1> >
{};
template <class A1, class A2>
struct dbus_traits< boost::shared_ptr< Result2<A1, A2> > >:
    public dbus_traits_result< Result2<A1, A2>, DBusResult2<A1, A2> >
{};
template <class A1, class A2, class A3>
    struct dbus_traits< boost::shared_ptr< Result3<A1, A2, A3> > >:
    public dbus_traits_result< Result3<A1, A2, A3>, DBusResult3<A1, A2, A3> >
{};
template <class A1, class A2, class A3, class A4>
    struct dbus_traits< boost::shared_ptr< Result4<A1, A2, A3, A4> > >:
    public dbus_traits_result< Result4<A1, A2, A3, A4>, DBusResult4<A1, A2, A3, A4> >
{};
template <class A1, class A2, class A3, class A4, class A5>
    struct dbus_traits< boost::shared_ptr< Result5<A1, A2, A3, A4, A5> > >:
    public dbus_traits_result< Result5<A1, A2, A3, A4, A5>, DBusResult5<A1, A2, A3, A4, A5> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6>
    struct dbus_traits< boost::shared_ptr< Result6<A1, A2, A3, A4, A5, A6> > >:
    public dbus_traits_result< Result6<A1, A2, A3, A4, A5, A6>, DBusResult6<A1, A2, A3, A4, A5, A6> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    struct dbus_traits< boost::shared_ptr< Result7<A1, A2, A3, A4, A5, A6, A7> > >:
    public dbus_traits_result< Result7<A1, A2, A3, A4, A5, A6, A7>, DBusResult7<A1, A2, A3, A4, A5, A6, A7> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    struct dbus_traits< boost::shared_ptr< Result8<A1, A2, A3, A4, A5, A6, A7, A8> > >:
    public dbus_traits_result< Result8<A1, A2, A3, A4, A5, A6, A7, A8>, DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    struct dbus_traits< boost::shared_ptr< Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9> > >:
    public dbus_traits_result< Result9<A1, A2, A3, A4, A5, A6, A7, A8, A9>, DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9> >
{};
template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10>
    struct dbus_traits< boost::shared_ptr< Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> > >:
    public dbus_traits_result< Result10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10>, DBusResult10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> >
{};

/** ===> 10 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9, class A10>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8, A9, A10)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9, A10);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M bind(Mptr C::*method, I instance) {
        // this fails because bind() only supports up to 9 parameters, including
        // the initial this pointer
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9 /* _10 */);
    }

    static const bool asynchronous = dbus_traits< DBusResult10<A1, A2, A3, A4, A5, A6, A7, A8, A9, A10> >::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;
            typename dbus_traits<A10>::host_type a10;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                       >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9) >> Get<A10>(a10);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                  << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9) << Set<A10>(a10);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        appendNewArg<A10>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        appendNewArgForReply<A10>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 9 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7, A8, A9)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        // this fails because bind() only supports up to 9 parameters, including
        // the initial this pointer
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9);
    }

    static const bool asynchronous = DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                       >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                      << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9);

            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 9 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8, class A9>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8, A9)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8, A9);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8, _9);
    }

    static const bool asynchronous = DBusResult9<A1, A2, A3, A4, A5, A6, A7, A8, A9>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;
            typename dbus_traits<A9>::host_type a9;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >> Get<A5>(a5)
                                       >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8) >> Get<A9>(a9);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8, a9);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4) << Set<A5>(a5)
                                  << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8) << Set<A9>(a9);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        appendNewArg<A9>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        appendNewArgForReply<A9>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 8 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7, A8)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8);
    }

    static const bool asynchronous = DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4) >>
                    Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                      << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8);

            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 8 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7, class A8>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7, A8)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7, A8);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7, _8);
    }

    static const bool asynchronous = DBusResult8<A1, A2, A3, A4, A5, A6, A7, A8>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;
            typename dbus_traits<A8>::host_type a8;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                       >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7) >> Get<A8>(a8);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7, a8);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                  << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7) << Set<A8>(a8);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        appendNewArg<A8>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        appendNewArgForReply<A8>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 7 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6, class A7>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6, A7)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6, A7);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7);
    }

    static const bool asynchronous = DBusResult7<A1, A2, A3, A4, A5, A6, A7>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                       >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                      << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 7 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6, class A7>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6, A7)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6, A7);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6, _7);
    }

    static const bool asynchronous = DBusResult7<A1, A2, A3, A4, A5, A6, A7>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;
            typename dbus_traits<A7>::host_type a7;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4)
                                       >> Get<A5>(a5) >> Get<A6>(a6) >> Get<A7>(a7);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6, a7);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4)
                                  << Set<A5>(a5) << Set<A6>(a6) << Set<A7>(a7);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        appendNewArg<A7>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        appendNewArgForReply<A7>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 6 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5,
          class A6>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5, A6)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5, A6);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6);
    }

    static const bool asynchronous = DBusResult6<A1, A2, A3, A4, A5, A6>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                       >> Get<A4>(a4) >> Get<A5>(a5) >> Get<A6>(a6);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                      << Set<A4>(a4) << Set<A5>(a5) << Set<A6>(a6);

            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 6 parameters */
template <class A1, class A2, class A3, class A4, class A5,
          class A6>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5, A6)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5, A6);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5, _6);
    }

    static const bool asynchronous = DBusResult6<A1, A2, A3, A4, A5, A6>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                       >> Get<A4>(a4) >> Get<A5>(a5) >> Get<A6>(a6);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5, a6);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                  << Set<A4>(a4) << Set<A5>(a5) << Set<A6>(a6);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        appendNewArg<A6>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        appendNewArgForReply<A6>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 5 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4, class A5>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4, A5)> >
{
    typedef R (Mptr)(A1, A2, A3, A4, A5);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5);
    }

    static const bool asynchronous = DBusResult5<A1, A2, A3, A4, A5>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                       >> Get<A4>(a4) >> Get<A5>(a5);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4, a5);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                      << Set<A4>(a4) << Set<A5>(a5);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 5 parameters */
template <class A1, class A2, class A3, class A4, class A5>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4, A5)> >
{
    typedef void (Mptr)(A1, A2, A3, A4, A5);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4, _5);
    }

    static const bool asynchronous = DBusResult5<A1, A2, A3, A4, A5>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3)
                                       >> Get<A4>(a4) >> Get<A5>(a5);

                (*static_cast<M *>(data))(a1, a2, a3, a4, a5);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3)
                                  << Set<A4>(a4) << Set<A5>(a5);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        appendNewArg<A5>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        appendNewArgForReply<A5>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 4 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3, class A4>
struct MakeMethodEntry< boost::function<R (A1, A2, A3, A4)> >
{
    typedef R (Mptr)(A1, A2, A3, A4);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4);
    }

    static const bool asynchronous = DBusResult4<A1, A2, A3, A4>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4);

                r = (*static_cast<M *>(data))(a1, a2, a3, a4);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 4 parameters */
template <class A1, class A2, class A3, class A4>
struct MakeMethodEntry< boost::function<void (A1, A2, A3, A4)> >
{
    typedef void (Mptr)(A1, A2, A3, A4);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3, _4);
    }

    static const bool asynchronous = DBusResult4<A1, A2, A3, A4>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3) >> Get<A4>(a4);

                (*static_cast<M *>(data))(a1, a2, a3, a4);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3) << Set<A4>(a4);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        appendNewArg<A4>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        appendNewArgForReply<A4>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 3 arguments, 1 return value */
template <class R,
          class A1, class A2, class A3>
struct MakeMethodEntry< boost::function<R (A1, A2, A3)> >
{
    typedef R (Mptr)(A1, A2, A3);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3);
    }

    static const bool asynchronous = DBusResult3<A1, A2, A3>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3);

                r = (*static_cast<M *>(data))(a1, a2, a3);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 3 parameters */
template <class A1, class A2, class A3>
struct MakeMethodEntry< boost::function<void (A1, A2, A3)> >
{
    typedef void (Mptr)(A1, A2, A3);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2, _3);
    }

    static const bool asynchronous = DBusResult3<A1, A2, A3>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2) >> Get<A3>(a3);

                (*static_cast<M *>(data))(a1, a2, a3);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2) << Set<A3>(a3);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        appendNewArg<A3>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        appendNewArgForReply<A3>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 2 arguments, 1 return value */
template <class R,
          class A1, class A2>
struct MakeMethodEntry< boost::function<R (A1, A2)> >
{
    typedef R (Mptr)(A1, A2);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2);
    }

    static const bool asynchronous = DBusResult2<A1, A2>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2);

                r = (*static_cast<M *>(data))(a1, a2);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1) << Set<A2>(a2);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 2 parameters */
template <class A1, class A2>
struct MakeMethodEntry< boost::function<void (A1, A2)> >
{
    typedef void (Mptr)(A1, A2);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1, _2);
    }

    static const bool asynchronous = DBusResult2<A1, A2>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1) >> Get<A2>(a2);

                (*static_cast<M *>(data))(a1, a2);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1) << Set<A2>(a2);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        appendNewArg<A2>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        appendNewArgForReply<A2>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 1 argument, 1 return value */
template <class R,
          class A1>
struct MakeMethodEntry< boost::function<R (A1)> >
{
    typedef R (Mptr)(A1);
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1);
    }

    static const bool asynchronous = DBusResult1<A1>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;
            typename dbus_traits<A1>::host_type a1;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1);

                r = (*static_cast<M *>(data))(a1);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r << Set<A1>(a1);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        appendNewArgForReply<A1>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 1 parameter */
template <class A1>
struct MakeMethodEntry< boost::function<void (A1)> >
{
    typedef void (Mptr)(A1);
    typedef boost::function<void (A1)> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance, _1);
    }

    static const bool asynchronous = DBusResult1<A1>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<A1>::host_type a1;

            try {
                ExtractArgs(conn, msg) >> Get<A1>(a1);

                (*static_cast<M *>(data))(a1);

                if (asynchronous) {
                    return NULL;
                }

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) << Set<A1>(a1);
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *inArgs = g_ptr_array_new();
        appendNewArg<A1>(inArgs);
        g_ptr_array_add(inArgs, NULL);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReply<A1>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** 0 arguments, 1 return value */
template <class R>
struct MakeMethodEntry< boost::function<R ()> >
{
    typedef R (Mptr)();
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance);
    }

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                       GDBusMessage *msg, void *data)
    {
        try {
            std::auto_ptr<GDBusMessage> reply;
            typename dbus_traits<R>::host_type r;

            try {
                r = (*static_cast<M *>(data))();

                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs(reply) + r;
            } catch (...) {
                return handleException(msg);
            }
            return reply.release();
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        GPtrArray *outArgs = g_ptr_array_new();
        appendNewArgForReturn<R>(outArgs);
        g_ptr_array_add(outArgs, NULL);

        entry->name     = g_strdup(name);
        entry->in_args  = NULL;
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** ===> 0 parameter */
template <>
struct MakeMethodEntry< boost::function<void ()> >
{
    typedef void (Mptr)();
    typedef boost::function<Mptr> M;

    template <class I, class C> static M boostptr(Mptr C::*method, I instance) {
        return boost::bind(method, instance);
    }

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            (*static_cast<M *>(data))();

            GDBusMessage *reply = g_dbus_message_new_method_reply(msg);
            return reply;
        } catch (...) {
            return handleException(msg);
        }
    }

    static GDBusMethodInfo *make(const char *name)
    {
        GDBusMethodInfo *entry = g_new0(GDBusMethodInfo, 1);

        entry->name     = g_strdup(name);
        entry->in_args  = NULL;
        entry->out_args = NULL;

        entry->ref_count = 1;
        return entry;
    }
};

template <class Cb, class Ret>
struct TraitsBase
{
    typedef Cb Callback_t;
    typedef Ret Return_t;

    struct CallbackData
    {
        //only keep connection, for DBusClientCall instance is absent when 'dbus client call' returns
        //suppose connection is available in the callback handler
        const DBusConnectionPtr m_conn;
        Callback_t m_callback;
        CallbackData(const DBusConnectionPtr &conn, const Callback_t &callback)
            : m_conn(conn), m_callback(callback)
        {}
    };
};

struct VoidReturn {};

struct VoidTraits : public TraitsBase<boost::function<void (const std::string &)>, VoidReturn>
{
    typedef TraitsBase<boost::function<void (const std::string &)>, VoidReturn> base;
    typedef base::Callback_t Callback_t;
    typedef base::Return_t Return_t;

    static Return_t demarshal(DBusMessagePtr &/*reply*/, const DBusConnectionPtr &/*conn*/)
    {
        return Return_t();
    }

    static void handleMessage(DBusMessagePtr &reply, base::CallbackData *data, GError* error)
    {
        std::string error_msg;
        //unmarshal the return results and call user callback
        if (error != NULL || g_dbus_message_to_gerror(reply.get(), &error)) {
            if (boost::starts_with(error->message, "GDBus.Error:")) {
                error_msg = error->message + 12;
            } else {
                error_msg = error->message;
            }
        }
        if (data->m_callback) {
            data->m_callback(error_msg);
        }
        delete data;
        if (error != NULL) {
            g_error_free (error);
        }
    }
};

template <class R1>
struct Ret1Traits : public TraitsBase<boost::function<void (const R1 &, const std::string &)>, R1>
{
    typedef TraitsBase<boost::function<void (const R1 &, const std::string &)>, R1> base;
    typedef typename base::Callback_t Callback_t;
    typedef typename base::Return_t Return_t;

    static Return_t demarshal(DBusMessagePtr &reply, const DBusConnectionPtr &conn)
    {
        typename dbus_traits<R1>::host_type r;

        ExtractResponse(conn.get(), reply.get()) >> Get<R1>(r);
        return r;
    }

    static void handleMessage(DBusMessagePtr &reply, typename base::CallbackData *data, GError* error)
    {
        typename dbus_traits<R1>::host_type r;
        std::string error_msg;

        if (error == NULL && !g_dbus_message_to_gerror(reply.get(), &error)) {
            ExtractResponse(data->m_conn.get(), reply.get()) >> Get<R1>(r);
        } else if (boost::starts_with(error->message, "GDBus.Error:")) {
            error_msg = error->message + 12;
        } else {
            error_msg = error->message;
        }

        //unmarshal the return results and call user callback
        if (data->m_callback) {
            data->m_callback(r, error_msg);
        }
        delete data;
        // cppcheck-suppress nullPointer
        // Looks invalid: cppcheck warning: nullPointer - Possible null pointer dereference: error - otherwise it is redundant to check it against null.
        if (error != NULL) {
            g_error_free (error);
        }
    }
};

template <class R1, class R2>
struct Ret2Traits : public TraitsBase<boost::function<void (const R1 &, const R2 &, const std::string &)>, std::pair<R1, R2> >
{
    typedef TraitsBase<boost::function<void (const R1 &, const R2 &, const std::string &)>, std::pair<R1, R2> > base;
    typedef typename base::Callback_t Callback_t;
    typedef typename base::Return_t Return_t;

    static Return_t demarshal(DBusMessagePtr &reply, const DBusConnectionPtr &conn)
    {
        Return_t r;

        ExtractResponse(conn.get(), reply.get()) >> Get<R1>(r.first) >> Get<R2>(r.second);
        return r;
    }

    static void handleMessage(DBusMessagePtr &reply, typename base::CallbackData *data, GError* error)
    {
        typename dbus_traits<R1>::host_type r1;
        typename dbus_traits<R2>::host_type r2;
        std::string error_msg;

        if (error == NULL && !g_dbus_message_to_gerror(reply.get(), &error)) {
            ExtractResponse(data->m_conn.get(), reply.get()) >> Get<R1>(r1) >> Get<R2>(r2);
        } else if (boost::starts_with(error->message, "GDBus.Error:")) {
            error_msg = error->message + 12;
        } else {
            error_msg = error->message;
        }

        //unmarshal the return results and call user callback
        if (data->m_callback) {
            data->m_callback(r1, r2, error_msg);
        }
        delete data;
        // cppcheck-suppress nullPointer
        // Looks invalid: cppcheck warning: nullPointer - Possible null pointer dereference: error - otherwise it is redundant to check it against null.
        if (error != NULL) {
            g_error_free (error);
        }
    }
};

template <class R1, class R2, class R3>
struct Ret3Traits : public TraitsBase<boost::function<void (const R1 &, const R2 &, const R3 &, const std::string &)>, boost::tuple<R1, R2, R3> >
{
    typedef TraitsBase<boost::function<void (const R1 &, const R2 &, const R3 &, const std::string &)>, boost::tuple<R1, R2, R3> > base;
    typedef typename base::Callback_t Callback_t;
    typedef typename base::Return_t Return_t;

    static Return_t demarshal(DBusMessagePtr &reply, const DBusConnectionPtr &conn)
    {
        Return_t r;

        ExtractResponse(conn.get(), reply.get()) >> Get<R1>(boost::get<0>(r)) >> Get<R2>(boost::get<1>(r)) >> Get<R3>(boost::get<2>(r));
        return r;
    }

    static void handleMessage(DBusMessagePtr &reply, typename base::CallbackData *data, GError* error)
    {
        typename dbus_traits<R1>::host_type r1;
        typename dbus_traits<R2>::host_type r2;
        typename dbus_traits<R3>::host_type r3;
        std::string error_msg;

        if (error == NULL && !g_dbus_message_to_gerror(reply.get(), &error)) {
            ExtractResponse(data->m_conn.get(), reply.get()) >> Get<R1>(r1) >> Get<R2>(r2) >> Get<R3>(r3);
        } else if (boost::starts_with(error->message, "GDBus.Error:")) {
            error_msg = error->message + 12;
        } else {
            error_msg = error->message;
        }

        //unmarshal the return results and call user callback
        if (data->m_callback) {
            data->m_callback(r1, r2, r3, error_msg);
        }
        delete data;
        // cppcheck-suppress nullPointer
        // Looks invalid: cppcheck warning: nullPointer - Possible null pointer dereference: error - otherwise it is redundant to check it against null.
        if (error != NULL) {
            g_error_free (error);
        }
    }
};

template <class CallTraits>
class DBusClientCall
{
public:
    typedef typename CallTraits::Callback_t Callback_t;
    typedef typename CallTraits::Return_t Return_t;
    typedef typename CallTraits::base::CallbackData CallbackData;

protected:
    const std::string m_destination;
    const std::string m_path;
    const std::string m_interface;
    const std::string m_method;
    const DBusConnectionPtr m_conn;

    static void dbusCallback (GObject *src_obj, GAsyncResult *res, void *user_data) throw ()
    {
        try {
            CallbackData *data = static_cast<CallbackData *>(user_data);

            GError *err = NULL;
            DBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &err));
            CallTraits::handleMessage(reply, data, err);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in dbusCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in dbusCallback()");
        }
    }

    void prepare(DBusMessagePtr &msg) const
    {
        // Constructor steals reference, reset() doesn't!
        // Therefore use constructor+copy instead of reset().
        msg = DBusMessagePtr(g_dbus_message_new_method_call(m_destination.c_str(),
                                                            m_path.c_str(),
                                                            m_interface.c_str(),
                                                            m_method.c_str()));
        if (!msg) {
            throw std::runtime_error("g_dbus_message_new_method_call() failed");
        }
    }

    void send(DBusMessagePtr &msg, const Callback_t &callback) const
    {
        CallbackData *data = new CallbackData(m_conn, callback);
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  G_MAXINT, // no timeout
                                                  NULL, NULL, dbusCallback, data);
    }

    Return_t sendAndReturn(DBusMessagePtr &msg) const
    {
        GError* error = NULL;
        DBusMessagePtr reply(g_dbus_connection_send_message_with_reply_sync(m_conn.get(),
                                                                            msg.get(),
                                                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                                            G_MAXINT, // no timeout
                                                                            NULL,
                                                                            NULL,
                                                                            &error));


        if (error || g_dbus_message_to_gerror(reply.get(), &error)) {
            DBusErrorCXX(error).throwFailure(m_method);
        }
        return CallTraits::demarshal(reply, m_conn);
    }

public:
    DBusClientCall(const DBusRemoteObject &object, const std::string &method)
        :m_destination (object.getDestination()),
         m_path (object.getPath()),
         m_interface (object.getInterface()),
         m_method (method),
         m_conn (object.getConnection())
    {
    }

    GDBusConnection *getConnection() { return m_conn.get(); }
    std::string getMethod() const { return m_method; }

    Return_t operator () ()
    {
        DBusMessagePtr msg;
        prepare(msg);
        return sendAndReturn(msg);
    }

    void start(const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        send(msg, callback);
    }

    template <class A1>
    Return_t operator () (const A1 &a1) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1;
        return sendAndReturn(msg);
    }

    template <class A1>
    void start(const A1 &a1, const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1;
        send(msg, callback);
    }

    template <class A1, class A2>
    Return_t operator () (const A1 &a1, const A2 &a2) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2;
        return sendAndReturn(msg);
    }

    template <class A1, class A2>
    void start(const A1 &a1, const A2 &a2, const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2;
        send(msg, callback);
    }

    template <class A1, class A2, class A3>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5, const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
                      const A6 &a6) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
               const A6 &a6,
               const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
                      const A6 &a6, const A7 &a7) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
               const A6 &a6, const A7 &a7,
               const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
                      const A6 &a6, const A7 &a7, const A8 &a8) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
               const A6 &a6, const A7 &a7, const A8 &a8,
               const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
                      const A6 &a6, const A7 &a7, const A8 &a8, const A9 &a9) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
               const A6 &a6, const A7 &a7, const A8 &a8, const A9 &a9,
               const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9;
        send(msg, callback);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10>
    void operator () (const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
                      const A6 &a6, const A7 &a7, const A8 &a8, const A9 &a9, const A10 &a10) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
        sendAndReturn(msg);
    }

    template <class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9, class A10>
    void start(const A1 &a1, const A2 &a2, const A3 &a3, const A4 &a4, const A5 &a5,
               const A6 &a6, const A7 &a7, const A8 &a8, const A9 &a9, const A10 &a10,
               const Callback_t &callback) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg) << a1 << a2 << a3 << a4 << a5 << a6 << a7 << a8 << a9 << a10;
        send(msg, callback);
    }
};

/*
 * A DBus Client Call object handling zero or more parameter and
 * zero return value.
 */
class DBusClientCall0 : public DBusClientCall<VoidTraits>
{
public:
    DBusClientCall0 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<VoidTraits>(object, method)
    {
    }
};

/** 1 return value and 0 or more parameters */
template <class R1>
class DBusClientCall1 : public DBusClientCall<Ret1Traits<R1> >
{
public:
    DBusClientCall1 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Ret1Traits<R1> >(object, method)
    {
    }
};

/** 2 return value and 0 or more parameters */
template <class R1, class R2>
class DBusClientCall2 : public DBusClientCall<Ret2Traits<R1, R2> >
{
public:
    DBusClientCall2 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Ret2Traits<R1, R2> >(object, method)
    {
    }
};

/** 3 return value and 0 or more parameters */
template <class R1, class R2, class R3>
class DBusClientCall3 : public DBusClientCall<Ret3Traits<R1, R2, R3> >
{
public:
    DBusClientCall3 (const DBusRemoteObject &object, const std::string &method)
        : DBusClientCall<Ret3Traits<R1, R2, R3> >(object, method)
    {
    }
};

/**
 * Describes which signals are meant to be received by the callback in
 * a SignalWatch. Only available when using GDBus C++ for GIO (this
 * code here). GDBus libdbus only supports passing a DBusRemoteObject
 * to the SignalWatch constructors, which does a match based on object
 * path, interface name, and signal name.
 *
 * Traditionally, all three strings had to be given. Now if any of
 * them is empty, it is excluded from the filter entirely (any string
 * matches).
 *
 * Using this class adds the possibility to do a path prefix match or,
 * in the future, other kind of matches.
 *
 * TODO: add support for filtering by sender. Not doing so leads to
 * a situation where a malicious local process can fake signals.
 *
 * Avoid situations where different SignalWatches use exactly the same
 * filter. Creating both watches will work, but when one is destroyed
 * it might happen that the process stops receiving signals from the
 * D-Bus daemon because the watch for that signal filter was removed
 * from the connection (neither D-Bus spec nor D-Bus GIO guarantee
 * that the match is ref counted).
 */
class SignalFilter : public DBusRemoteObject
{
 public:
    enum Flags {
        SIGNAL_FILTER_NONE,
        /**
         * Normally, a path must match completely. With this flag set,
         * any signal which has the given path as prefix
         * matches.
         *
         * The path filter must not end in a slash.
         *
         * Example: "/foo/bar/xyz" matches "/foo/bar" only if
         * SIGNAL_FILTER_PATH_PREFIX is set. "/foo/barxyz" does not
         * match it in any case.
         *
         */
        SIGNAL_FILTER_PATH_PREFIX = 1<<0
    };

    /**
     * Match based on object path and interface as stored in object
     * and based on signal name as passed separately. Does a full
     * match of all unless a a string is empty, in which case
     * that criteria is ignored.
     */
    SignalFilter(const DBusRemoteObject &object,
                      const std::string &signal) :
        DBusRemoteObject(object),
        m_signal(signal),
        m_flags(SIGNAL_FILTER_NONE)
    {}

    /**
     * Full control over filtering.
     */
    SignalFilter(const DBusConnectionPtr &conn,
                 const std::string &path,
                 const std::string &interface,
                 const std::string &signal,
                 Flags flags) :
        DBusRemoteObject(conn, path, interface, ""),
        m_signal(signal),
        m_flags(flags)
    {}

    const char *getSignal() const { return m_signal.c_str(); }
    Flags getFlags() const { return Flags(m_flags); }

    /**
     * Check that current signal matches the filter.  Necessary
     * because GIO D-Bus does not know about "path_namespace" and
     * because G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE seems to disable all
     * signal filtering by GIO.
     */
    bool matches(const ExtractArgs &context) const
    {
        return
            (m_interface.empty() || m_interface == context.m_interface) &&
            (m_signal.empty() || m_signal == context.m_signal) &&
            (m_path.empty() ||
             ((m_flags & SIGNAL_FILTER_PATH_PREFIX) ?
              (strlen(context.m_path) > m_path.size() &&
               !m_path.compare(0, m_path.size(),
                               context.m_path, m_path.size()) &&
               context.m_path[m_path.size()] == '/') :
              m_path == context.m_path));
    }

 private:
    std::string m_signal;
    Flags m_flags;
};

/**
 * Helper class for builting a match rule.
 */
class Criteria : public std::list<std::string> {
 public:
    void add(const char *keyword, const char *value) {
        if (value && value[0]) {
            std::string buffer;
            buffer.reserve(strlen(keyword) + strlen(value) + 3);
            buffer += keyword;
            buffer += '=';
            buffer += '\'';
            buffer += value;
            buffer += '\'';
            push_back(buffer);
        }
    }

    std::string createMatchRule() const { return boost::join(*this, ","); }
};

/**
 * Common functionality of all SignalWatch* classes.
 * @param T     boost::function with the right signature
 */
template <class T> class SignalWatch : public SignalFilter
{
 public:
    SignalWatch(const DBusRemoteObject &object,
                const std::string &signal,
                bool = true)
    : SignalFilter(object, signal), m_tag(0), m_manualMatch(false)
    {
    }

    SignalWatch(const SignalFilter &filter)
    : SignalFilter(filter), m_tag(0), m_manualMatch(false)
    {
    }

    ~SignalWatch()
    {
        try {
            if (m_tag) {
                GDBusConnection *connection = getConnection();
                if (connection) {
                    g_dbus_connection_signal_unsubscribe(connection, m_tag);
                }
            }
            if (m_manualMatch) {
                DBusClientCall0(GDBusCXX::DBusRemoteObject(getConnection(),
                                                           "/org/freedesktop/DBus",
                                                           "org.freedesktop.DBus",
                                                           "org.freedesktop.DBus"),
                                "RemoveMatch")(m_matchRule);
            }
        } catch (...) {
            // TODO (?): log error
        }
    }

    typedef T Callback_t;
    const Callback_t &getCallback() const { return m_callback; }

 protected:
    guint m_tag;
    T m_callback;
    bool m_manualMatch;
    std::string m_matchRule;

    void activateInternal(const Callback_t &callback, GDBusSignalCallback cb)
    {
        m_callback = callback;
        m_tag = g_dbus_connection_signal_subscribe(getConnection(),
                                                   NULL,
                                                   getInterface()[0] ? getInterface() : NULL,
                                                   getSignal()[0] ? getSignal() : NULL,
                                                   (!(getFlags() & SIGNAL_FILTER_PATH_PREFIX) && getPath()[0]) ? getPath() : NULL,
                                                   NULL,
                                                   (getFlags() & SIGNAL_FILTER_PATH_PREFIX) ?
                                                   G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE :
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   cb,
                                                   this,
                                                   NULL);

        if (!m_tag) {
            throw std::runtime_error(std::string("activating signal failed: ") +
                                     "path " + getPath() +
                                     " interface " + getInterface() +
                                     " member " + getSignal());
        }
        if (getFlags() & SignalFilter::SIGNAL_FILTER_PATH_PREFIX) {
            // Need to set up match rules manually.
            Criteria criteria;
            criteria.push_back("type='signal'");
            criteria.add("interface", getInterface());
            criteria.add("member", getSignal());
            criteria.add("path_namespace", getPath());
            m_matchRule = criteria.createMatchRule();
            DBusClientCall0(GDBusCXX::DBusRemoteObject(getConnection(),
                                                       "/org/freedesktop/DBus",
                                                       "org.freedesktop.DBus",
                                                       "org.freedesktop.DBus"),
                            "AddMatch")(m_matchRule);
            m_manualMatch = true;
        }
    }
};

class SignalWatch0 : public SignalWatch< boost::function<void (void)> >
{
    typedef boost::function<void (void)> Callback_t;

 public:
    SignalWatch0(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch0(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            const Callback_t &cb = watch->getCallback();
            cb();
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (void)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1>
class SignalWatch1 : public SignalWatch< boost::function<void (const A1 &)> >
{
    typedef boost::function<void (const A1 &)> Callback_t;

 public:
    SignalWatch1(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch1(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            const Callback_t &cb = watch->getCallback();
            cb(a1);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1, typename A2>
class SignalWatch2 : public SignalWatch< boost::function<void (const A1 &, const A2 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &)> Callback_t;

 public:
    SignalWatch2(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch2(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            dbus_traits<A2>::get(context, iter, a2);
            const Callback_t &cb = watch->getCallback();
            cb(a1, a2);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

template <typename A1, typename A2, typename A3>
class SignalWatch3 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &)> Callback_t;

 public:
    SignalWatch3(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch3(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            dbus_traits<A2>::get(context, iter, a2);
            dbus_traits<A3>::get(context, iter, a3);
            const Callback_t &cb = watch->getCallback();
            cb(a1, a2, a3);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &,
                                           const A2 &,
                                           const A3 &)> >::activateInternal(callback,
                                                                            internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4>
class SignalWatch4 : public SignalWatch< boost::function<void (const A1 &, const A2 &,
                                                               const A3 &, const A4 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &)> Callback_t;

 public:
    SignalWatch4(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch4(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            dbus_traits<A2>::get(context, iter, a2);
            dbus_traits<A3>::get(context, iter, a3);
            dbus_traits<A4>::get(context, iter, a4);
            const Callback_t &cb = watch->getCallback();
            cb(a1, a2, a3, a4);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5>
class SignalWatch5 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &, const A5 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &, const A4 &, const A5 &)> Callback_t;

 public:
    SignalWatch5(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch5(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data)
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            dbus_traits<A2>::get(context, iter, a2);
            dbus_traits<A3>::get(context, iter, a3);
            dbus_traits<A4>::get(context, iter, a4);
            dbus_traits<A5>::get(context, iter, a5);
            const Callback_t &cb = watch->getCallback();
            cb(a1, a2, a3, a4, a5);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &,
                                           const A5 &)> >::activateInternal(callback, internalCallback);
    }
};

template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
class SignalWatch6 : public SignalWatch< boost::function<void (const A1 &, const A2 &, const A3 &,
                                                               const A4 &, const A5 &, const A6 &)> >
{
    typedef boost::function<void (const A1 &, const A2 &, const A3 &,
                                  const A4 &, const A5 &, const A6 &)> Callback_t;


 public:
    SignalWatch6(const DBusRemoteObject &object,
                 const std::string &signal,
                 bool = true)
        : SignalWatch<Callback_t>(object, signal)
    {
    }

    SignalWatch6(const SignalFilter &filter)
        : SignalWatch<Callback_t>(filter)
    {}

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            SignalWatch<Callback_t> *watch = static_cast< SignalWatch<Callback_t> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal);
            if (!watch->matches(context)) {
                return;
            }

            typename dbus_traits<A1>::host_type a1;
            typename dbus_traits<A2>::host_type a2;
            typename dbus_traits<A3>::host_type a3;
            typename dbus_traits<A4>::host_type a4;
            typename dbus_traits<A5>::host_type a5;
            typename dbus_traits<A6>::host_type a6;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            dbus_traits<A1>::get(context, iter, a1);
            dbus_traits<A2>::get(context, iter, a2);
            dbus_traits<A3>::get(context, iter, a3);
            dbus_traits<A4>::get(context, iter, a4);
            dbus_traits<A5>::get(context, iter, a5);
            dbus_traits<A6>::get(context, iter, a6);
            const Callback_t &cb = watch->getCallback();
            cb(a1, a2, a3, a4, a5, a6);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }

    void activate(const Callback_t &callback)
    {
        SignalWatch< boost::function<void (const A1 &, const A2 &,
                                           const A3 &, const A4 &,
                                           const A5 &, const A6 &)> >::activateInternal(callback,
                                                                                        internalCallback);
    }
};

} // namespace GDBusCXX

#endif // INCL_GDBUS_CXX_BRIDGE
