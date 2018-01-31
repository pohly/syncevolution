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
#include <glib-object.h>

#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include <type_traits>

#include <boost/intrusive_ptr.hpp>
#include <boost/variant.hpp>
#include <boost/variant/get.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/join.hpp>
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

struct GDBusMessageUnref
{
    void operator () (GDBusMessage *ptr) const { g_object_unref(ptr); }
};
typedef std::unique_ptr<GDBusMessage, GDBusMessageUnref> GDBusMessageUnique;

namespace {
    // Helper function for concatenating an arbitrary number of strings.
    template<typename A1, typename ...A> std::string concat(const A1 &a1, A... a) { return std::string(a1) + concat(a...); }
}

/**
 * Simple unique_ptr for GVariant.
 */
class GVariantCXX : boost::noncopyable
{
    GVariant *m_var;
 public:
    /** takes over ownership */
    GVariantCXX(GVariant *var = nullptr) : m_var(var) {}
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
    GVariantIterCXX(GVariantIter *var = nullptr) : m_var(var) {}
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

    DBusConnectionPtr(const DBusConnectionPtr &other) :
      boost::intrusive_ptr<GDBusConnection>(other),
      m_name(other.m_name)
      {}

    DBusConnectionPtr & operator = (const DBusConnectionPtr &other)
    {
        *static_cast<boost::intrusive_ptr<GDBusConnection> *>(this) = static_cast<const boost::intrusive_ptr<GDBusConnection> &>(other);
        m_name = other.m_name;
        return *this;
    }

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

    typedef std::function<void ()> Disconnect_t;
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
                      const std::function<void (bool)> &obtainedCB) const;
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
    DBusErrorCXX(GError *error = nullptr)
    : m_error(error)
    {
    }
    DBusErrorCXX(const DBusErrorCXX &dbus_error)
    : m_error(nullptr)
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
                nullptr);
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
                                          DBusErrorCXX *err);

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
     * explicitly in the callback. Message processing is delayed on the new
     * connection, so the callback can set up objects and then must undelay
     * the connection.
     */
    typedef std::function<void (DBusServerCXX &, DBusConnectionPtr &)> NewConnection_t;

    /**
     * Start listening for new connections. Mimics the libdbus DBusServer API, but
     * underneath sets up a single connection via pipes. The caller must fork
     * the process which calls dbus_get_bus_connection() before entering the main
     * event loop again because that is when the DBusServerCXX will finish
     * the connection setup (close child fd, call newConnection).
     *
     * All errors are reported via exceptions, not "err".
     */
    static std::shared_ptr<DBusServerCXX> listen(const NewConnection_t &newConnection, DBusErrorCXX *err);

    /**
     * address used by the server
     */
    std::string getAddress() const { return m_address; }

 private:
    DBusServerCXX(const std::string &address);
    DBusConnectionPtr m_connection;
    NewConnection_t m_newConnection;
    guint m_connectionIdle;
    int m_childfd;
    std::string m_address;

    static gboolean onIdleOnce(gpointer custom);
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
     * asking to be passed a "std::shared_ptr<Result*>" parameter.
     * The dbus_traits for those parameters have "asynchronous" set to
     * true, which skips all processing after calling the method.
     */
    static const bool asynchronous = false;
};

/**
 * A combination of different types have their signature concatenated
 * and are asynchronous if any individual type is asynchronous.
 */
template <typename ...C> struct dbus_traits_many {};
template<typename C1, typename ...C> struct dbus_traits_many<C1, C...> {
    static std::string getSignature() { return dbus_traits<C1>::getSignature() + dbus_traits_many<C...>::getSignature(); }
    static const bool asynchronous = dbus_traits<C1>::asynchronous || dbus_traits_many<C...>::asynchronous;
};
template<> struct dbus_traits_many<> {
    static std::string getSignature() { return ""; }
    static const bool asynchronous = false;
};

/**
 * Append a varying number of parameters as result to the
 * message, using AppendRetvals(msg) << res1 << res2 << ...
 * or AppendRetvals(msg).append(res1, res2, ...).
 *
 * Types can be anything that has a dbus_traits, including
 * types which are normally recognized as input parameters in D-Bus
 * method calls.
 */
class AppendRetvals : private boost::noncopyable {
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

    template<typename A1, typename ...A> void append(A1 a1, A... args) {
        dbus_traits<A1>::append(m_builder, a1);
        append(args...);
    }
    void append() {}
};


/**
 * Append a varying number of method parameters as result to the reply
 * message, using AppendArgs(msg) << Set<A1>(res1) << Set<A2>(res2) << ...;
 */
class AppendArgs : private boost::noncopyable {
    GDBusMessage *m_msg;

 public:
    // public because several other helper classes need access
    GVariantBuilder m_builder;

    AppendArgs(const GDBusMessageUnique &msg) {
        m_msg = msg.get();
        if (!m_msg) {
            throw std::runtime_error("nullptr GDBusMessage reply");
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
    template<class A> AppendArgs & operator += (const A &a) {
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
struct ExtractArgs : private boost::noncopyable {
    // always set
    GDBusConnection *m_conn;
    GVariantIter m_iter;

    // only set when handling a method call
    GDBusMessage **m_msg;

    // only set when m_msg is nullptr (happens when handling signal)
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
    /** constructor for parsing a method invocation message, which must not be nullptr */
    ExtractArgs(GDBusConnection *conn, GDBusMessage *&msg);

    /** constructor for parsing signal parameters */
    ExtractArgs(GDBusConnection *conn,
                const char *sender,
                const char *path,
                const char *interface,
                const char *signal,
                GVariant *params);

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
                                           G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
        } else {
            if (!msg) {
                throwFailure(m_signal, "g_dbus_message_new_signal()", nullptr);
            }

            GError *error = nullptr;
            if (!g_dbus_connection_send_message(m_object.getConnection(), msg.get(),
                                                G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, &error)) {
                throwFailure(m_signal, "g_dbus_connection_send_message()", error);
            }
        }
    }
};

template<bool optional = false> class EmitSignal0 : private EmitSignalHelper<optional>
{
 public:
    EmitSignal0(const DBusObject &object,
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
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", nullptr);
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

void appendArgInfo(GPtrArray *pa, const std::string &type);

template<typename ...A> struct AppendSignature;
 template<typename A1, typename ...A> struct AppendSignature<A1, A...> {
    static void appendArgs(GPtrArray *pa) {
        // "in" direction
        appendArgInfo(pa, dbus_traits<A1>::getSignature());
        AppendSignature<A...>::appendArgs(pa);
    }
    static void appendArgsForReply(GPtrArray *pa) {
        // "out" direction
        appendArgInfo(pa, dbus_traits<A1>::getReply());
        AppendSignature<A...>::appendArgsForReply(pa);
    }
    static void appendArgsForReturn(GPtrArray *pa) {
        // "out" direction, type must not be skipped
        appendArgInfo(pa, dbus_traits<A1>::getType());
        AppendSignature<A...>::appendArgsForReturn(pa);
    }
};
template<> struct AppendSignature<> {
    static void appendArgs(GPtrArray *pa) {}
    static void appendArgsForReply(GPtrArray *pa) {}
    static void appendArgsForReturn(GPtrArray *pa) {}
};

template <bool optional, typename ...A>
class EmitSignalBase : private EmitSignalHelper<optional>
{
 public:
    EmitSignalBase(const DBusObject &object,
                   const std::string &signal) :
        EmitSignalHelper<optional>(object, signal)
    {}

    typedef void result_type;

    void operator () (A... args)
    {
        DBusMessagePtr msg(g_dbus_message_new_signal(EmitSignalHelper<optional>::m_object.getPath(),
                                                     EmitSignalHelper<optional>::m_object.getInterface(),
                                                     EmitSignalHelper<optional>::m_signal.c_str()));
        if (!msg) {
            if (optional) {
                return;
            }
            throwFailure(EmitSignalHelper<optional>::m_signal, "g_dbus_message_new_signal()", nullptr);
        }
        AppendRetvals(msg).append(args...);
        EmitSignalHelper<optional>::sendMsg(msg);
    }

    GDBusSignalInfo *makeSignalEntry() const
    {
        GDBusSignalInfo *entry = g_new0(GDBusSignalInfo, 1);

        GPtrArray *args = g_ptr_array_new();
        AppendSignature<A...>::appendArgs(args);
        g_ptr_array_add(args, nullptr);

        entry->name = g_strdup(EmitSignalHelper<optional>::m_signal.c_str());
        entry->args = (GDBusArgInfo **)g_ptr_array_free (args, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};
template<typename ...A> using EmitSignal = EmitSignalBase<false, A...>;
template<typename ...A> using EmitSignalOptional = EmitSignalBase<true, A...>;

struct FunctionWrapperBase {
    void* m_func_ptr;
    FunctionWrapperBase(void* func_ptr)
    : m_func_ptr(func_ptr)
    {}

    virtual ~FunctionWrapperBase() {}
};

template<typename M>
struct FunctionWrapper : public FunctionWrapperBase {
    FunctionWrapper(std::function<M>* func_ptr)
    : FunctionWrapperBase(reinterpret_cast<void*>(func_ptr))
    {}

    virtual ~FunctionWrapper() {
        delete reinterpret_cast<std::function<M>*>(m_func_ptr);
    }
};

struct MethodHandler
{
    typedef GDBusMessage *(*MethodFunction)(GDBusConnection *conn, GDBusMessage *msg, void *data);
    typedef std::shared_ptr<FunctionWrapperBase> FuncWrapper;
    typedef std::pair<MethodFunction, FuncWrapper > CallbackPair;
    typedef std::map<const std::string, CallbackPair > MethodMap;
    static MethodMap m_methodMap;
    static std::function<void (void)> m_callback;

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
        // Set to nullptr, just to be sure we remember that it is gone.
        // cppcheck-suppress uselessAssignmentPtrArg
        invocation = nullptr;

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

        GError *error = nullptr;
        g_dbus_connection_send_message(connection,
                                       reply,
                                       G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                       nullptr,
                                       &error);
        g_object_unref(reply);
        // Cannot throw an exception, glib event loop won't know what to do with it;
        // pretend that the problem didn't happen.
        if (error != nullptr) {
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
// with additional nullptr check. The methods themselves crash on nullptr,
// which happens when appending the terminating nullptr to m_methods/m_signals
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
    typedef std::function<void (void)> Callback_t;

    DBusObjectHelper(const DBusConnectionPtr &conn,
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

        typedef MakeMethodEntry< std::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));

        std::function<M> *func = new std::function<M>(entry_type::boostptr(method, instance));
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

        typedef MakeMethodEntry< std::function<M> > entry_type;
        g_ptr_array_add(m_methods, entry_type::make(name));

        std::function<M> *func = new std::function<M>(function);
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
        // method and signal array must be nullptr-terminated.
        if (m_connId) {
            throw std::logic_error("This object was already activated.");
        }
        if (m_methods->len &&
            m_methods->pdata[m_methods->len - 1] != nullptr) {
            g_ptr_array_add(m_methods, nullptr);
        }
        if (m_signals->len &&
            m_signals->pdata[m_signals->len - 1] != nullptr) {
            g_ptr_array_add(m_signals, nullptr);
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
                                                     nullptr,
                                                     nullptr);
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
        if (var == nullptr || !g_variant_type_equal(g_variant_get_type(var), VariantTraits::getVariantType())) {
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

/** runtime detection of integer representation */
template<typename I, bool issigned, size_t bytes> struct dbus_traits_integer_switch {};
template<typename I> struct dbus_traits_integer_switch<I, true, 2> :
    public basic_marshal< I, VariantTypeInt16 >
{
    static std::string getType() { return "n"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<typename I> struct dbus_traits_integer_switch<I, false, 2> :
    public basic_marshal< I, VariantTypeUInt16 >
{
    static std::string getType() { return "q"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<typename I> struct dbus_traits_integer_switch<I, true, 4> :
    public basic_marshal< I, VariantTypeInt32 >
{
    static std::string getType() { return "i"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<typename I> struct dbus_traits_integer_switch<I, false, 4> :
    public basic_marshal< I, VariantTypeUInt32 >
{
    static std::string getType() { return "u"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<typename I> struct dbus_traits_integer_switch<I, true, 8> :
    public basic_marshal< I, VariantTypeInt64 >
{
    static std::string getType() { return "x"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};
template<typename I> struct dbus_traits_integer_switch<I, false, 8> :
    public basic_marshal< I, VariantTypeUInt64 >
{
    static std::string getType() { return "t"; }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
};

template<typename I> struct dbus_traits_integer : public dbus_traits_integer_switch<I, std::is_signed<I>::value, sizeof(I)> {};

// Some of these types may have the same underlying representation, but they are
// still considered different types by the compiler and thus we must have dbus_traits
// for all of them.
template<> struct dbus_traits<signed short> : public dbus_traits_integer<signed short> {};
template<> struct dbus_traits<unsigned short> : public dbus_traits_integer<unsigned short> {};
template<> struct dbus_traits<signed int> : public dbus_traits_integer<signed int> {};
template<> struct dbus_traits<unsigned int> : public dbus_traits_integer<unsigned int> {};
template<> struct dbus_traits<signed long> : public dbus_traits_integer<signed long> {};
template<> struct dbus_traits<unsigned long> : public dbus_traits_integer<unsigned long> {};

// Needed for int64_t and uint64. Not used internally, but occurs in
// external D-Bus APIs (for example, Bluez5 obexd). The assumption here
// is that "long long" is a valid type. If that assumption doesn't hold
// on some platform, then we need a configure check and ifdefs here.
template<> struct dbus_traits<signed long long> : public dbus_traits_integer<signed long long> {};
template<> struct dbus_traits<unsigned long long> : public dbus_traits_integer<unsigned long long> {};

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
        if (var == nullptr || !g_variant_type_equal(g_variant_get_type(var), VariantTypeBoolean::getVariantType())) {
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
        if (var == nullptr || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_STRING)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        const char *str = g_variant_get_string(var, nullptr);
        value = str;
    }

    static void append(GVariantBuilder &builder, const std::string &value)
    {
        // g_variant_new_string() will log an assertion and/or return nullptr
        // (as in FDO #90118) when the string contains non-UTF-8 content.
        // We must check in advance to avoid the assertion, even if that
        // means duplicating the check (once here and once inside g_variant_new_string().
        //
        // Strictly speaking, this is something that the caller should
        // have checked for, but as this should only happen for
        // invalid external data (like broken iCalendar 2.0 events,
        // see FDO #90118) and the only reasonable error handling in
        // SyncEvolution would consist of filtering the data, so it is
        // less intrusive overall to do that here: a question mark
        // substitutes all invalid bytes.
        const char *start = value.c_str(),
            *end = value.c_str() + value.size();
        const gchar *invalid;
        bool valid = g_utf8_validate(start, end - start, &invalid);
        GVariant *tmp;
        if (valid) {
            tmp = g_variant_new_string(value.c_str());
        } else {
            std::string buffer;
            buffer.reserve(value.size());
            while (true) {
                if (valid) {
                    buffer.append(start, end - start);
                    // Empty string is valid, so we end up here in all cases.
                    break;
                } else {
                    buffer.append(start, invalid - start);
                    buffer.append("?");
                    start = invalid + 1;
                }
                valid = g_utf8_validate(start, end - start, &invalid);
            }
            tmp = g_variant_new_string(buffer.c_str());
        }
        g_variant_builder_add_value(&builder, tmp);
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
        if (var == nullptr || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_OBJECT_PATH)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }
        const char *objPath = g_variant_get_string(var, nullptr);
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
            nullptr;
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
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
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
 * a std::tuple - maps to D-Bus struct
 */
template<typename ...A> struct dbus_traits< std::tuple<A...> > : public dbus_traits_base
{
    static std::string getContainedType()
    {
        return concat(dbus_traits<A...>::getType());
    }
    static std::string getType()
    {
        return "(" + getContainedType() + ")";
    }
    static std::string getSignature() {return getType(); }
    static std::string getReply() { return ""; }
    typedef std::tuple<A...> host_type;
    typedef const std::tuple<A...> &arg_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &t)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter tupIter;
        g_variant_iter_init(&tupIter, var);
        get_all(context, tupIter, t);
    }

    static void append(GVariantBuilder &builder, arg_type t)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        append_all(builder, t);
        g_variant_builder_close(&builder);
    }

 private:
    // Compile-time recursion for each tuple element.

    template<std::size_t i = 0, typename... Tp>
        static typename std::enable_if<i == sizeof...(Tp), void>::type
        get_all(ExtractArgs &context, GVariantIter &tupIter, std::tuple<Tp...> &t) {}
    template<std::size_t i = 0, typename... Tp>
        static typename std::enable_if<i < sizeof...(Tp), void>::type
        get_all(ExtractArgs &context, GVariantIter &tupIter, std::tuple<Tp...> &t) {
        dbus_traits<decltype(std::get<i>(t))>::get(context, tupIter, std::get<i>(t));
        get_all<i + 1, Tp...>(t);
    }

    template<std::size_t i = 0, typename... Tp>
        static typename std::enable_if<i == sizeof...(Tp), void>::type
        append_all(GVariantBuilder &builder, const std::tuple<Tp...>& t) {}
    template<std::size_t i = 0, typename... Tp>
        static typename std::enable_if<i < sizeof...(Tp), void>::type
        append_all(GVariantBuilder &builder, const std::tuple<Tp...>& t) {
        dbus_traits<decltype(std::get<i>(t))>::append(builder, std::get<i>(t));
        append_all<i + 1, Tp...>(t);
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
        std::pair<size_t, const V *>(0, nullptr)
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
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
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
                                                            nullptr, nullptr // no need to free data
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
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        GVariantIter contIter;
        GVariantCXX child;
        g_variant_iter_init(&contIter, var);
        while((child = g_variant_iter_next_value(&contIter)) != nullptr) {
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

        for(auto it = dict.begin();
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
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_ARRAY)) {
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

        for(auto it = array.begin();
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
namespace {
    template<typename V, typename ...M> struct get_one_type;
    template<typename V, typename M1, typename ...M> struct get_one_type<V, M1, M...> {
        static void get(ExtractArgs &context, GVariantIter &varIter, const char *type, V &value) {
            if (dbus_traits<M1>::getSignature() == type) {
                M1 val;
                dbus_traits<M1>::get(context, varIter, val);
                value = val;
            } else {
                // try next type
                get_one_type<V, M...>::get(context, varIter, type, value);
            }
        }
    };
    template<typename V> struct get_one_type<V> {
        static void get(ExtractArgs &context, GVariantIter &varIter, const char *type, V &value) {
            // If we end up here, nothing that was passed to us via D-Bus matches
            // any of the types that can be stored in the boost::variant.
            // We simply ignore this and continue with an empty variant.
        }
    };
}


template <class ...M> struct dbus_traits <boost::variant <M...> >
{
    static std::string getType() { return "v"; }
    static std::string getSignature() { return getType(); }
    static std::string getReply() { return ""; }

    typedef boost::variant<M...> host_type;
    typedef const boost::variant<M...> &arg_type;

    static void append(GVariantBuilder &builder, const boost::variant<M...> &value)
    {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(getType().c_str()));
        boost::apply_visitor(append_visitor(builder), value);
        g_variant_builder_close(&builder);
    }

    static void get(ExtractArgs &context, GVariantIter &iter, boost::variant<M...> &value)
    {
        GVariantCXX var(g_variant_iter_next_value(&iter));
        if (var == nullptr || !g_variant_type_equal(g_variant_get_type(var), G_VARIANT_TYPE_VARIANT)) {
            throw std::runtime_error("g_variant failure " GDBUS_CXX_SOURCE_INFO);
        }

        // Determine actual type contained in the variant.
        GVariantIter varIter;
        g_variant_iter_init(&varIter, var);
        GVariantCXX varVar(g_variant_iter_next_value(&varIter));
        const char *type = g_variant_get_type_string(varVar);
        // Reset the iterator so that the call to dbus_traits<V>::get() will get the right variant;
        g_variant_iter_init(&varIter, var);
        // Now extract it.
        get_one_type<boost::variant<M...>, M...>::get(context, varIter, type, value);
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
        if (var == nullptr) {
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
 * The base class of type V of a struct K, followed by another dbus_member
 * or dbus_member_single to end the chain
 */
template<class K, class V, class M> struct dbus_base
{
    static std::string getType()
    {
        return dbus_traits<V>::getType() + M::getType();
    }
    typedef V host_type;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, K &val)
    {
        dbus_traits<V>::get(context, iter, val);
        M::get(context, iter, val);
    }

    static void append(GVariantBuilder &builder, const K &val)
    {
        dbus_traits<V>::append(builder, val);
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
        if (var == nullptr || !g_variant_type_is_subtype_of(g_variant_get_type(var), G_VARIANT_TYPE_TUPLE)) {
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

static inline GDBusMessage *handleException(GDBusMessage *&callerMsg)
{
    // We provide a reply to the message. Clear the "msg" variable
    // in our caller's context to make it as done.
    GDBusMessage *msg = callerMsg;
    callerMsg = nullptr;

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
    std::function<void (void)> m_callback;
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
          const std::function<void (void)> &callback = {});
    ~Watch();

    /**
     * Changes the callback triggered by this Watch.  If the watch has
     * already fired, the callback is invoked immediately.
     */
    void setCallback(const std::function<void (void)> &callback);

    /**
     * Starts watching for disconnect of that peer
     * and also checks whether it is currently
     * still around.
     */
    void activate(const char *peer);
};

void getWatch(ExtractArgs &context, std::shared_ptr<Watch> &value);

/**
 * pseudo-parameter: not part of D-Bus signature,
 * but rather extracted from message attributes
 */
template <> struct dbus_traits< std::shared_ptr<Watch> >  : public dbus_traits_base
{
    static std::string getType() { return ""; }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return ""; }

    static void get(ExtractArgs &context,
                    GVariantIter &iter, std::shared_ptr<Watch> &value) { getWatch(context, value); }
    static void append(GVariantBuilder &builder, const std::shared_ptr<Watch> &value) {}

    typedef std::shared_ptr<Watch> host_type;
    typedef const std::shared_ptr<Watch> &arg_type;
};

/**
 * implementation class for D-Bus results,
 * keeps references to required objects and provides the
 * failed() and done() methods (pure virtual in the public Result API class)
 */
template<typename ...A> class DBusResult : virtual public Result<A...>
{
 protected:
    DBusConnectionPtr m_conn;     /**< connection via which the message was received */
    DBusMessagePtr m_msg;         /**< the method invocation message */
    bool m_haveOwnership;         /**< this class is responsible for sending a method reply */
    bool m_replied;               /**< a response was sent */

    void sendMsg(const DBusMessagePtr &msg)
    {
        m_replied = true;
        GError *error = nullptr;
        if (!g_dbus_connection_send_message(m_conn.get(), msg.get(),
                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, &error)) {
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

    virtual Watch *createWatch(const std::function<void (void)> &callback)
    {
        std::unique_ptr<Watch> watch(new Watch(m_conn, callback));
        watch->activate(g_dbus_message_get_sender(m_msg.get()));
        return watch.release();
    }

    virtual void done(A... args)
    {
        DBusMessagePtr reply(g_dbus_message_new_method_reply(m_msg.get()));
        if (!reply) {
            throw std::runtime_error("no GDBusMessage");
        }
        AppendRetvals(reply).append(args...);
        sendMsg(reply);
    }

    static std::string getSignature() { return dbus_traits_many<A...>::getSignature(); }
    static const bool asynchronous = dbus_traits_many<A...>::asynchronous;
 };

/**
 * Helper class for constructing a DBusResult: while inside the
 * initial method call handler, we have a try/catch block which will
 * reply to the caller. Once we leave that block, this class here
 * destructs and transfers the responsibility for sending a reply to
 * the DBusResult instance.
 */
template <class DBusR> class DBusResultGuard : public std::shared_ptr<DBusR>
{
    GDBusMessage **m_msg;
 public:
     DBusResultGuard() : m_msg(nullptr) {}
    ~DBusResultGuard() throw ()
    {
        DBusR *result = std::shared_ptr<DBusR>::get();
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
        std::shared_ptr<DBusR>::reset(new DBusR(context.m_conn, context.m_msg ? *context.m_msg : nullptr));
    }
};

template <typename ...A>
struct dbus_traits< std::shared_ptr<Result<A...> > >
{
    static std::string getType() { return DBusResult<A...>::getSignature(); }
    static std::string getSignature() { return ""; }
    static std::string getReply() { return getType(); }

    typedef DBusResultGuard< DBusResult<A...> > host_type;
    typedef std::shared_ptr< Result<A...> > &arg_type;
    static const bool asynchronous = true;

    static void get(ExtractArgs &context,
                    GVariantIter &iter, host_type &value)
    {
        value.initDBusResult(context);
    }
};

namespace {
    // Call function with values from a tuple as parameters.
    // std::index_sequence is C++14
    template<typename F, typename T, std::size_t ...I> auto apply_impl(F &&f, T &t, std::index_sequence<I...>) {
	return std::forward<F>(f)(std::get<I>(t)...);
    }
    template<typename F, typename ...T> auto apply(F &&f, std::tuple<T...> &t) {
	return apply_impl(std::forward<F>(f), t, std::index_sequence_for<T...>());
    }

    // Convert functions parameters between tuple and D-Bus message.
    template<typename T, int i, typename ...A> struct args;
    template<typename T, int i, typename A1, typename ...A> struct args<T, i, A1, A...> {
        // Input parameters.
        static void get(ExtractArgs &ea, T &t) {
            ea >> Get<A1>(std::get<i>(t));
            args<T, i + 1, A...>::get(ea, t);
        }
        // Output parameters.
        static void set(AppendArgs &aa, T &t) {
            aa << Set<A1>(std::get<i>(t));
            args<T, i + 1, A...>::set(aa, t);
        }
    };
    template<typename T, int i> struct args<T, i> {
        static void get(ExtractArgs &ea, T &t) {}
        static void set(AppendArgs &aa, T &t) {}
    };
}

/** return value */
template <typename R, typename ...A>
struct MakeMethodEntry< std::function<R (A...)> >
{
    typedef R (Mptr)(A...);
    typedef std::function<Mptr> M;

    template <class I, class C> static auto boostptr(Mptr C::*method, I instance) {
        return [method, instance] (A... a) {
            return (instance->*method)(a...);
        };
    }

    static const bool asynchronous = dbus_traits_many<A...>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            GDBusMessageUnique reply;
            typedef std::tuple<typename dbus_traits<A>::host_type...> T;
            R r;
            T t;

            try {
                // Extract all input parameters.
                ExtractArgs ea(conn, msg);
                args<T, 0, A...>::get(ea, t);

                r = apply(*static_cast<M *>(data), t);
                if (asynchronous) {
                    return nullptr;
                }

                // Send response with all output parameters.
                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs aa(reply);
                aa += r;
                args<T, 0, A...>::set(aa, t);
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
        AppendSignature<A...>::appendArgs(inArgs);
        g_ptr_array_add(inArgs, nullptr);

        GPtrArray *outArgs = g_ptr_array_new();
        AppendSignature<R>::appendArgsForReturn(outArgs);
        AppendSignature<A...>::appendArgsForReply(outArgs);
        g_ptr_array_add(outArgs, nullptr);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

/** no return value */
template <typename ...A>
struct MakeMethodEntry< std::function<void (A...)> >
{
    typedef void (Mptr)(A...);
    typedef std::function<Mptr> M;

    template <class I, class C> static auto boostptr(Mptr C::*method, I instance) {
        return [method, instance] (A... a) {
            return (instance->*method)(a...);
        };
    }

    static const bool asynchronous = dbus_traits_many<A...>::asynchronous;

    static GDBusMessage *methodFunction(GDBusConnection *conn,
                                        GDBusMessage *msg, void *data)
    {
        try {
            GDBusMessageUnique reply;
            typedef std::tuple<typename dbus_traits<A>::host_type...> T;
            T t;

            try {
                // Extract all input parameters.
                ExtractArgs ea(conn, msg);
                args<T, 0, A...>::get(ea, t);

                apply(*static_cast<M *>(data), t);
                if (asynchronous) {
                    return nullptr;
                }

                // Send response with all output parameters.
                reply.reset(g_dbus_message_new_method_reply(msg));
                AppendArgs aa(reply);
                args<T, 0, A...>::set(aa, t);
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
        AppendSignature<A...>::appendArgs(inArgs);
        g_ptr_array_add(inArgs, nullptr);

        GPtrArray *outArgs = g_ptr_array_new();
        AppendSignature<A...>::appendArgsForReply(outArgs);
        g_ptr_array_add(outArgs, nullptr);

        entry->name     = g_strdup(name);
        entry->in_args  = (GDBusArgInfo **)g_ptr_array_free(inArgs,  FALSE);
        entry->out_args = (GDBusArgInfo **)g_ptr_array_free(outArgs, FALSE);

        entry->ref_count = 1;
        return entry;
    }
};

template<typename ...R> struct DBusClientCallReturnType;
// no return value: return void, with an empty tuple as buffer
template<> struct DBusClientCallReturnType<>
{
    typedef std::tuple<> Buffer_t;
    typedef void Return_t;
    static const void returnResult(const Buffer_t &result) {}
};
// single return value is not wrapped in a tuple.
template<typename R1> struct DBusClientCallReturnType<R1>
{
    typedef std::tuple<R1> Buffer_t;
    typedef R1 Return_t;
    static const Return_t &returnResult(const Buffer_t &result) { return std::get<0>(result); }
};
// two return values: std::pair
template<typename R1, typename R2> struct DBusClientCallReturnType<R1, R2>
{
    typedef std::tuple<R1, R2> Buffer_t;
    typedef std::pair<R1, R2> Return_t;
    static const Return_t returnResult(const Buffer_t &result) { return Return_t(std::get<0>(result), std::get<1>(result)); }
};
// General case: at least three return values in a tuple
template<typename R1, typename R2, typename R3, typename ...R> struct DBusClientCallReturnType<R1, R2, R3, R...>
{
    typedef std::tuple<R1, R2, R3, R...> Buffer_t;
    typedef Buffer_t Return_t;
    static const Return_t &returnResult(const Buffer_t &result) { return result; }
};

template<typename ...R> class DBusClientCall
{
    typedef std::function<void (R..., const std::string &)> Callback_t;
    typedef typename DBusClientCallReturnType<R...>::Return_t Return_t;
    typedef typename DBusClientCallReturnType<R...>::Buffer_t Buffer_t;

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

    const std::string m_destination;
    const std::string m_path;
    const std::string m_interface;
    const std::string m_method;
    const DBusConnectionPtr m_conn;

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
        // We store a copy of the callback on the heap for use in a plain-C callback.
        CallbackData *data = new CallbackData(m_conn, callback);
        auto c_callback = [] (GObject *src_obj, GAsyncResult *res, void *user_data) noexcept {
            try {
                CallbackData *data = static_cast<CallbackData *>(user_data);

                GError *error = nullptr;
                DBusMessagePtr reply(g_dbus_connection_send_message_with_reply_finish(data->m_conn.get(), res, &error));
                Buffer_t r;
                std::string error_msg;

                if (error == nullptr && !g_dbus_message_to_gerror(reply.get(), &error)) {
                    // unmarshal the return results into tuple
                    GDBusMessage *replyPtr = reply.get();
                    ExtractArgs ea(data->m_conn.get(), replyPtr);
                    args<Buffer_t, 0, R...>::get(ea, r);
                } else if (boost::starts_with(error->message, "GDBus.Error:")) {
                    error_msg = error->message + 12;
                } else {
                    error_msg = error->message;
                }

                if (data->m_callback) {
                    // call user callback with tuple values + error message
                    apply([&error_msg, data] (R... r) { data->m_callback(r..., error_msg); }, r);
                }
                delete data;
                // cppcheck-suppress nullPointer
                // Looks invalid: cppcheck warning: nullPointer - Possible null pointer dereference: error - otherwise it is redundant to check it against null.
                if (error != nullptr) {
                    g_error_free (error);
                }
            } catch (const std::exception &ex) {
                g_error("unexpected exception caught in dbusCallback(): %s", ex.what());
            } catch (...) {
                g_error("unexpected exception caught in dbusCallback()");
            }
        };
        g_dbus_connection_send_message_with_reply(m_conn.get(), msg.get(), G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                  G_MAXINT, // no timeout
                                                  nullptr, nullptr, c_callback, data);
    }

    Return_t sendAndReturn(DBusMessagePtr &msg) const
    {
        GError* error = nullptr;
        DBusMessagePtr reply(g_dbus_connection_send_message_with_reply_sync(m_conn.get(),
                                                                            msg.get(),
                                                                            G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                                            G_MAXINT, // no timeout
                                                                            nullptr,
                                                                            nullptr,
                                                                            &error));


        if (error || g_dbus_message_to_gerror(reply.get(), &error)) {
            DBusErrorCXX(error).throwFailure(m_method);
        }

        Buffer_t r;
        GDBusMessage *replyPtr = reply.get();
        ExtractArgs ea(m_conn.get(), replyPtr);
        args<Buffer_t, 0, R...>::get(ea, r);

        return DBusClientCallReturnType<R...>::returnResult(r);
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

    template<typename ...A> void start(const Callback_t &callback, const A &... args) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg).append(args...);
        send(msg, callback);
    }

    template<typename ...A> Return_t operator () (const A  &... args) const
    {
        DBusMessagePtr msg;
        prepare(msg);
        AppendRetvals(msg).append(args...);
        return sendAndReturn(msg);
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

template <typename ...A> class SignalWatch : public SignalFilter
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
                DBusClientCall<>(GDBusCXX::DBusRemoteObject(getConnection(),
                                                           "/org/freedesktop/DBus",
                                                           "org.freedesktop.DBus",
                                                           "org.freedesktop.DBus"),
                                "RemoveMatch")(m_matchRule);
            }
        } catch (...) {
            // TODO (?): log error
        }
    }

    typedef std::function<void (A...)> Callback_t;
    const Callback_t &getCallback() const { return m_callback; }

    void activate(const Callback_t &callback)
    {
        m_callback = callback;
        m_tag = g_dbus_connection_signal_subscribe(getConnection(),
                                                   nullptr,
                                                   getInterface()[0] ? getInterface() : nullptr,
                                                   getSignal()[0] ? getSignal() : nullptr,
                                                   (!(getFlags() & SIGNAL_FILTER_PATH_PREFIX) && getPath()[0]) ? getPath() : nullptr,
                                                   nullptr,
                                                   (getFlags() & SIGNAL_FILTER_PATH_PREFIX) ?
                                                   G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE :
                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                   internalCallback,
                                                   this,
                                                   nullptr);

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
            DBusClientCall<>(GDBusCXX::DBusRemoteObject(getConnection(),
                                                       "/org/freedesktop/DBus",
                                                       "org.freedesktop.DBus",
                                                       "org.freedesktop.DBus"),
                            "AddMatch")(m_matchRule);
            m_manualMatch = true;
        }
    }

 private:
    guint m_tag;
    Callback_t m_callback;
    bool m_manualMatch;
    std::string m_matchRule;

    static void internalCallback(GDBusConnection *conn,
                                 const gchar *sender,
                                 const gchar *path,
                                 const gchar *interface,
                                 const gchar *signal,
                                 GVariant *params,
                                 gpointer data) throw ()
    {
        try {
            auto watch = static_cast< SignalWatch<A...> *>(data);
            ExtractArgs context(conn, sender, path, interface, signal, params);
            if (!watch->matches(context)) {
                return;
            }

            typedef std::tuple<typename dbus_traits<A>::host_type...> T;
            T t;

            GVariantIter iter;
            g_variant_iter_init(&iter, params);
            args<T, 0, A...>::get(context, t);
            apply(watch->m_callback, t);
        } catch (const std::exception &ex) {
            g_error("unexpected exception caught in internalCallback(): %s", ex.what());
        } catch (...) {
            g_error("unexpected exception caught in internalCallback()");
        }
    }
};

} // namespace GDBusCXX

#endif // INCL_GDBUS_CXX_BRIDGE
