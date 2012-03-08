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

#include "server-read-operations.h"
#include "server.h"

SE_BEGIN_CXX

ServerReadOperations::ServerReadOperations(const std::string &config_name, Server &server) :
    ReadOperations(config_name), m_server(server)
{}

void ServerReadOperations::getConfigs(bool getTemplates, std::vector<std::string> &configNames)
{
    if (getTemplates) {
        SyncConfig::DeviceList devices;

        // get device list from dbus server, currently only bluetooth devices
        m_server.getDeviceList(devices);

        // also include server templates in search
        devices.push_back(SyncConfig::DeviceDescription("", "", SyncConfig::MATCH_FOR_CLIENT_MODE));

        //clear existing templates
        clearPeerTempls();

        SyncConfig::TemplateList list = SyncConfig::getPeerTemplates(devices);
        std::map<std::string, int> numbers;
        BOOST_FOREACH(const boost::shared_ptr<SyncConfig::TemplateDescription> peer, list) {
            //if it is not a template for device
            if(peer->m_deviceName.empty()) {
                configNames.push_back(peer->m_templateId);
            } else {
                string templName = "Bluetooth_";
                templName += peer->m_deviceId;
                templName += "_";
                std::map<std::string, int>::iterator it = numbers.find(peer->m_deviceId);
                if(it == numbers.end()) {
                    numbers.insert(std::make_pair(peer->m_deviceId, 1));
                    templName += "1";
                } else {
                    it->second++;
                    stringstream seq;
                    seq << it->second;
                    templName += seq.str();
                }
                configNames.push_back(templName);
                addPeerTempl(templName, peer);
            }
        }
    } else {
        SyncConfig::ConfigList list = SyncConfig::getConfigs();
        BOOST_FOREACH(const SyncConfig::ConfigList::value_type &server, list) {
            configNames.push_back(server.first);
        }
    }
}

SE_END_CXX
