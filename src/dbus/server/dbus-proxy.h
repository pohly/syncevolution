/*
 * Copyright (C) 2012 Intel Corporation
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
 * Utility code for relaying D-Bus method calls and signals from
 * syncevo-dbus-server to syncevo-dbus-helper.
 */

#ifndef DBUS_PROXY_H
#define DBUS_PROXY_H

#include <string>
#include <boost/shared_ptr.hpp>
#include <boost/signals2.hpp>

#include <gdbus-cxx.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

typedef boost::signals2::signal<void ()> DBusSuccessSignal_t;
typedef boost::signals2::signal<void (const std::string &)> DBusFailureSignal_t;

/**
 * Use this class in syncevo-dbus-server as callback for asynchronous
 * method calls to syncevo-dbus-helper. Once it gets the reply from
 * syncevo-dbus-helper (successful or otherwise), it will finish the
 * pending method call.
 *
 * It is possible to hook into the reply processing by connecting
 * to the signal(s) provided by the class. At the moment, only
 * a single signal is provided, with the error string as parameter.
 * More signals could be added as needed.
 *
 * The signals have to be shared pointers because ProxyCallback
 * must be copyable, which boost::signals aren't.
 *
 * Exceptions thrown while processing the reply will be logged by
 * the GDBus C++ bindings, but because they happen inside the main
 * event loop, they cannot be propagated to the upper layers.
 *
 * @param R     some kind of GDBusCXX::Result0/1/2/3
 */
template <class R> class ProxyCallback
{
 public:
    ProxyCallback(const boost::shared_ptr<R> &result) :
        m_success(new DBusSuccessSignal_t),
        m_failure(new DBusFailureSignal_t),
        m_result(result)
    {}

    void operator () (const std::string &error);

    /**
     * Triggered after a successful method call was reported back to
     * the original caller.
     */
    boost::shared_ptr< DBusSuccessSignal_t > m_success;

    /**
     * Triggered after a method call failure was reported back to the
     * original caller.
     */
    boost::shared_ptr< DBusFailureSignal_t > m_failure;

 private:
    boost::shared_ptr<R> m_result;
};

template <class R> void ProxyCallback<R>::operator () (const std::string &error)
{
    if (error.empty()) {
        // TODO: split error into bus name + description and relay exactly that
        // (see Chris' code for that)
        m_result->failed(GDBusCXX::dbus_error("org.syncevolution.gdbuscxx.Exception",
                                             error));
        (*m_failure)(error);
    } else {
        m_result->done();
        (*m_success)();
    }
}

/**
 * utility method for creating a callback of the right type
 */
template <class R> class ProxyCallback<R> MakeProxyCallback(const boost::shared_ptr<R> &result)
{
    return ProxyCallback<R>(result);
}

SE_END_CXX

#endif // RESOURCE_H
