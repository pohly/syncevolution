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

#include "connman-client.h"
#include "server.h"
#include "presence-status.h"

SE_BEGIN_CXX

ConnmanClient::ConnmanClient(Server &server):
    DBusRemoteObject(!strcmp(getEnv("DBUS_TEST_CONNMAN", ""), "none") ?
                     nullptr : /* simulate missing ConnMan */
                     GDBusCXX::dbus_get_bus_connection(!strcmp(getEnv("DBUS_TEST_CONNMAN", ""), "session") ?
                                                       "SESSION" : /* use our own ConnMan stub */
                                                       "SYSTEM" /* use real ConnMan */,
                                                       nullptr, true, nullptr),
                     "/", "net.connman.Manager", "net.connman", true),
    m_available(false),
    m_server(server),
    m_propertyChanged(*this, "PropertyChanged")
{
    if (getConnection()) {
        typedef std::map <std::string, boost::variant<std::string> > PropDict;
        GDBusCXX::DBusClientCall<PropDict>  getProp(*this,"GetProperties");

        auto getPropCb = [this] (const std::map <std::string,
                                 boost::variant<std::string> >& props, const string &error) {
            if (!error.empty()) {
                m_available = false;
                if (error == "org.freedesktop.DBus.Error.ServiceUnknown") {
                    // ensure there is still first set of singal set in case of no
                    // connman available
                    m_server.getPresenceStatus().updatePresenceStatus (true, PresenceStatus::HTTP_TRANSPORT);
                    SE_LOG_DEBUG(NULL, "No connman service available %s", error.c_str());
                    return;
                }
                SE_LOG_DEBUG(NULL, "error in connmanCallback %s", error.c_str());
                return;
            }

            m_available = true;
            typedef std::pair <std::string, boost::variant<std::string> > element;
            bool httpPresence = false;
            for (element entry: props) {
                // just check online state, we don't care how about the underlying technology
                if (entry.first == "State") {
                    std::string state = boost::get<std::string>(entry.second);
                    httpPresence = state == "online";
                    break;
                }
            }

            //now delivering the signals
            m_server.getPresenceStatus().updatePresenceStatus (httpPresence, PresenceStatus::HTTP_TRANSPORT);
        };
        getProp.start(getPropCb);

        auto propertyChanged = [this] (const std::string &name,
                                       const boost::variant<std::vector<std::string>, std::string> &prop) {
            if (name == "State") {
                std::string state = boost::get<std::string>(prop);
                m_server.getPresenceStatus().updatePresenceStatus(state == "online",
                                                                  PresenceStatus::HTTP_TRANSPORT);
            }
        };
        m_propertyChanged.activate(propertyChanged);
    }else{
        SE_LOG_DEBUG(NULL, "DBus connection setup for connman failed");
    }
}

SE_END_CXX
