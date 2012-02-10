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

#include "resource.h"
#include "exceptions.h"

SE_BEGIN_CXX

bool Resource::setResult(const std::string &error)
{
    if(!error.empty()) {
        m_result = false;
        m_resultError.assign(error);
    } else {
        m_result = true;
        m_resultError.clear();
    }

    return m_result;
}

/**
 * Determine and throw appropriate exception based on returned error string
 */
void Resource::throwExceptionFromString(const std::string &errorString)
{
    size_t pos = errorString.find_first_of(':');
    if(pos == std::string::npos) {
        return;
    }

    std::string exName(errorString.substr(0, pos));
    std::string msg(errorString.substr(pos + 2)); // Don't include colon nor following space.

    if(boost::iequals(exName, "org.syncevolution.NoSuchConfig")) {
        SE_THROW_EXCEPTION(NoSuchConfig, msg);
    } else if (boost::iequals(exName, "org.syncevolution.NoSuchSource")) {
        SE_THROW_EXCEPTION(NoSuchSource, msg);
    } else if (boost::iequals(exName, "org.syncevolution.InvalidCall")) {
        SE_THROW_EXCEPTION(InvalidCall, msg);
    } else if (boost::iequals(exName, "org.syncevolution.SourceUnusable")) {
        SE_THROW_EXCEPTION(SourceUnusable, msg);
    } else {
        SE_THROW_EXCEPTION(DBusSyncException, msg);
    }
}

void Resource::waitForReply(gint timeout)
{
    m_result = true;
    guint counter (0);
    const guint counter_max(10);

    // sleeps ten times for one tenth of given timeout
    while(!methodInvocationDone()) {
        if (counter == counter_max) {
            replyInc();
            m_result = false;
            return;
        }
        g_usleep(timeout * 100);
        g_main_context_iteration(g_main_context_default(), TRUE);
        ++counter;
    }
}

void Resource::genericErrorHandler(const std::string &error, const std::string &method)
{
    if (error.empty()) {
        SE_LOG_INFO(NULL, NULL, "%s.%s successfull.", m_resourceName.c_str(), method.c_str());
    } else {
        throwExceptionFromString(error);
    }
}

SE_END_CXX
