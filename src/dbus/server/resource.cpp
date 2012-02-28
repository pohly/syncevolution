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

void Resource::printStatus(const std::string &error,
                           const std::string &name,
                           const std::string &method)
{
    if (error.empty()) {
        SE_LOG_INFO(NULL, NULL, "%s.%s call succeeded.", name.c_str(), method.c_str());
    } else {
        SE_LOG_ERROR(NULL, NULL, "%s.%s call failed: %s", name.c_str(), method.c_str(), error.c_str());
        //throwExceptionFromString(error);
    }
}

SE_END_CXX
