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

SE_END_CXX
