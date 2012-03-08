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
#include "dbus-callbacks.h"

SE_BEGIN_CXX

void Resource::printStatus(const std::string &error,
                           const std::string &name,
                           const std::string &method)
{
    printStatusWithCallback(error, name, method, boost::function<void()>(nullCb));
}

void Resource::printStatusWithCallback(const std::string &error,
                                       const std::string &name,
                                       const std::string &method,
                                       const boost::function<void()> &callback)
{
    if (error.empty()) {
        SE_LOG_INFO(NULL, NULL, "%s.%s call succeeded.", name.c_str(), method.c_str());
        callback();
    } else {
        SE_LOG_ERROR(NULL, NULL, "%s.%s call failed: %s", name.c_str(), method.c_str(), error.c_str());
    }
}

SE_END_CXX
