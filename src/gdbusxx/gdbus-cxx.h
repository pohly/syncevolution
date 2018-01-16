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

#ifndef INCL_GDBUS_CXX
#define INCL_GDBUS_CXX

#include <string>
#include <stdexcept>
#include <functional>

#include <boost/noncopyable.hpp>

namespace GDBusCXX {

/**
 * An exception class which can be thrown to create
 * specific D-Bus exception on the bus.
 */
class dbus_error : public std::runtime_error
{
public:
    /**
     * @param dbus_name     the D-Bus error name, like "org.example.error.Invalid"
     * @param what          a more detailed description
     */
    dbus_error(const std::string &dbus_name, const std::string &what) :
        std::runtime_error(what),
        m_dbus_name(dbus_name)
        {}
    ~dbus_error() throw() {}

    const std::string &dbusName() const { return m_dbus_name; }

private:
    std::string m_dbus_name;
};

/**
 * Special parameter type that identifies a D-Bus bus address. A string in practice.
 */
class Caller_t : public std::string
{
 public:
    Caller_t() {}
    template <class T> Caller_t(T val) : std::string(val) {}
    template <class T> Caller_t &operator = (T val) { assign(val); return *this; }
};

/**
 * Special parameter type that identifies the path in a D-Bus message. A string in practice.
 */
class Path_t : public std::string
{
 public:
    Path_t() {}
    template <class T> Path_t(T val) : std::string(val) {}
    template <class T> Path_t &operator = (T val) { assign(val); return *this; }
};

/**
 * Special parameter type that identifies the interface in a D-Bus message. A string in practice.
 */
class Interface_t : public std::string
{
 public:
    Interface_t() {}
    template <class T> Interface_t(T val) : std::string(val) {}
    template <class T> Interface_t &operator = (T val) { assign(val); return *this; }
};

/**
 * Special parameter type that identifies the member of an interface
 * (= signal or method) in a D-Bus message. A string in practice.
 */
class Member_t : public std::string
{
 public:
    Member_t() {}
    template <class T> Member_t(T val) : std::string(val) {}
    template <class T> Member_t &operator = (T val) { assign(val); return *this; }
};

class Watch;

/**
 * Call object which needs to be called with the results
 * of an asynchronous method call. So instead of
 * "int foo()" one would implement
 * "void foo(Result<int> > *r)"
 * and after foo has returned, call r->done(res). Use const
 * references as type for complex results.
 *
 * A Result instance cannot be copied and only called once.
 */
class ResultBase : private boost::noncopyable
{
 public:
    virtual ~ResultBase() {}

    /** report failure to caller */
    virtual void failed(const dbus_error &error) = 0;

    /**
     * Calls the given callback once when the peer that the result
     * would be delivered to disconnects.  The callback will also be
     * called if the peer is already gone by the time that the watch
     * is requested.
     *
     * Alternatively a method can ask to get called with a life Watch
     * by specifying "const std::shared_ptr<Watch> &" as parameter
     * and then calling its setCallback().
     */
    virtual Watch *createWatch(const std::function<void (void)> &callback) = 0;
};
template<typename ...A> class Result : public ResultBase
{
 public:
    /** tell caller that we are done */
    virtual void done(A... args) = 0;
};

} // namespace GDBusCXX

#endif // INCL_GDBUS_CXX
