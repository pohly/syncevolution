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

typedef boost::signals2::signal<void (const std::string &)> DBusFailureSignalType;

struct ProxyCallbackTraits0
{
    typedef boost::signals2::signal<void ()> SuccessSignalType;
    typedef GDBusCXX::Result0 ResultType;
};

template <class A1>
struct ProxyCallbackTraits1
{
    typedef typename boost::signals2::signal<void (const A1 &)> SuccessSignalType;
    typedef typename GDBusCXX::Result1<A1> ResultType;
};

template <class A1, class A2>
struct ProxyCallbackTraits2
{
    typedef typename boost::signals2::signal<void (const A1 &, const A2 &)> SuccessSignalType;
    typedef typename GDBusCXX::Result2<A1, A2> ResultType;
};

template <class A1, class A2, class A3>
struct ProxyCallbackTraits3
{
    typedef typename boost::signals2::signal<void (const A1 &, const A2 &, const A3 &)> SuccessSignalType;
    typedef typename GDBusCXX::Result3<A1, A2, A3> ResultType;
};

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
 * @param T     traits class (one of ProxyCallbackTraits{0,1,2,3})
 */
template <class T> class ProxyCallbackBase
{
 public:
    typedef typename T::SuccessSignalType SuccessSignalType;
    typedef DBusFailureSignalType FailureSignalType;
    typedef typename T::ResultType ResultType;

    ProxyCallbackBase(const boost::shared_ptr<ResultType> &result) :
        m_success(new SuccessSignalType),
        m_failure(new FailureSignalType),
        m_result(result)
    {}

    /**
     * Triggered after a successful method call was reported back to
     * the original caller.
     */
    boost::shared_ptr<SuccessSignalType> m_success;

    /**
     * Triggered after a method call failure was reported back to the
     * original caller.
     */
    boost::shared_ptr<FailureSignalType> m_failure;

 protected:
    boost::shared_ptr<ResultType> m_result;
};

class ProxyCallback0 : public ProxyCallbackBase<ProxyCallbackTraits0>
{
 public:
    ProxyCallback0(const boost::shared_ptr<GDBusCXX::Result0> &result) :
        ProxyCallbackBase<ProxyCallbackTraits0> (result)
    {}

    void operator () (const std::string &error)
    {
        if (!error.empty()) {
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
};

template <class A1>
class ProxyCallback1 : public ProxyCallbackBase<ProxyCallbackTraits1<A1> >
{
 public:
    ProxyCallback1(const boost::shared_ptr<GDBusCXX::Result1<A1> > &result) :
        ProxyCallbackBase<ProxyCallbackTraits1<A1> >(result)
    {}

    void operator () (const A1 &a1,
                      const std::string &error)
    {
        // using this-> notation, because otherwise compiler says
        // that m_{failure,success,result} is undefined.
        if (!error.empty()) {
            // TODO: split error into bus name + description and relay exactly that
            // (see Chris' code for that)
            this->m_result->failed(GDBusCXX::dbus_error("org.syncevolution.gdbuscxx.Exception",
                                                        error));
            (*(this->m_failure))(error);
        } else {
            this->m_result->done(a1);
            (*(this->m_success))(a1);
        }
    }
};

template <class A1, class A2>
class ProxyCallback2 : public ProxyCallbackBase<ProxyCallbackTraits2<A1, A2> >
{
 public:
    ProxyCallback2(const boost::shared_ptr<GDBusCXX::Result2<A1, A2> > &result) :
        ProxyCallbackBase<ProxyCallbackTraits2<A1, A2> >(result)
    {}

    void operator () (const A1 &a1, const A2 &a2,
                      const std::string &error)
    {
        // using this-> notation, because otherwise compiler says
        // that m_{failure,success,result} is undefined.
        if (!error.empty()) {
            // TODO: split error into bus name + description and relay exactly that
            // (see Chris' code for that)
            this->m_result->failed(GDBusCXX::dbus_error("org.syncevolution.gdbuscxx.Exception",
                                                        error));
            (*(this->m_failure))(error);
        } else {
            this->m_result->done(a1, a2);
            (*(this->m_success))(a1, a2);
        }
    }
};

template <class A1, class A2, class A3>
  class ProxyCallback3 : public ProxyCallbackBase<ProxyCallbackTraits3<A1, A2, A3> >
{
 public:
    ProxyCallback3(const boost::shared_ptr<GDBusCXX::Result3<A1, A2, A3> > &result) :
        ProxyCallbackBase<ProxyCallbackTraits3<A1, A2, A3> >(result)
    {}

    void operator () (const A1 &a1, const A2 &a2, const A3 &a3,
                      const std::string &error)
    {
        // using this-> notation, because otherwise compiler says
        // that m_{failure,success,result} is undefined.
        if (!error.empty()) {
            // TODO: split error into bus name + description and relay exactly that
            // (see Chris' code for that)
            this->m_result->failed(GDBusCXX::dbus_error("org.syncevolution.gdbuscxx.Exception",
                                                        error));
            (*(this->m_failure))(error);
        } else {
            this->m_result->done(a1, a2, a3);
            (*(this->m_success))(a1, a2, a3);
        }
    }
};

SE_END_CXX

#endif // DBUS_PROXY_H
