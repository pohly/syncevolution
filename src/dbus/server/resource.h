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

#ifndef RESOURCE_H
#define RESOURCE_H

#include <string>

#include <boost/bind.hpp>

#include <syncevo/SmartPtr.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

class Server;

/**
 * Anything that can be owned by a client, like a connection
 * or session.
 */
class Resource {
public:
    enum Priority {
        PRI_CMDLINE = -10,
        PRI_DEFAULT = 0,
        PRI_CONNECTION = 10,
        PRI_AUTOSYNC = 20
    };

    Resource(Server &server, const std::string &resourceName) :
        m_server(server),
        m_priority(PRI_DEFAULT),
        m_isRunning(false),
        m_result(true),
        m_resourceName(resourceName),
        m_replyTotal(0),
        m_replyCounter(0) {}
    virtual ~Resource() {}

    Priority getPriority() { return m_priority; }
    void setPriority(Priority priority) { m_priority = priority; }

    bool getIsRunning() { return m_isRunning; }

    // This base class always assumes concurrent syncing is not
    // possible. Override this in ConnectionResource and
    // SessionResource if you want to enable running concurrent syncs.
    virtual bool canRunConcurrently(boost::shared_ptr<Resource> resource) { return false; }

protected:
    Server &m_server;

    Priority m_priority;
    bool m_isRunning;

    // Status of most recent dbus call to helper
    bool m_result;
    std::string m_resultError;
    bool setResult(const std::string &error);
    std::string m_resourceName;

    // the number of total dbus calls
    unsigned int m_replyTotal;
    // the number of returned dbus calls
    unsigned int m_replyCounter;

    /** whether the dbus call(s) has/have completed */
    bool methodInvocationDone() { return m_replyTotal == m_replyCounter; }

    /** set the total number of replies we must wait */
    void resetReplies(int total = 1) { m_replyTotal = total; m_replyCounter = 0; }
    void replyInc() { m_replyCounter++; }
    void waitForReply(int timeout = 100 /*ms*/);

    // Determine and throw appropriate exception based on returned error string
    static void throwExceptionFromString(const std::string &errorString);


    // static, so we don't have to track the instance of resource.
    static void printStatus(const std::string &error,
                            const std::string &name,
                            const std::string &method);

    template <class R>
    void defaultConnectToSuccess(R &proxyCallback, const std::string &method)
    {
        proxyCallback.m_success->connect(typename R::SuccessSignalType::slot_type(&Resource::printStatus,
                                                                                  std::string(),
                                                                                  m_resourceName,
                                                                                  method));
    }

    template <class R>
    void defaultConnectToFailure(R &proxyCallback, const std::string &method)
    {
        proxyCallback.m_failure->connect(typename R::FailureSignalType::slot_type(&Resource::printStatus,
                                                                                  _1,
                                                                                  m_resourceName,
                                                                                  method));
    }

    template <class R>
    void defaultConnectToBoth(R &proxyCallback, const std::string &method)
    {
        defaultConnectToSuccess(proxyCallback, method);
        defaultConnectToFailure(proxyCallback, method);
    }
};

SE_END_CXX

#endif // RESOURCE_H
