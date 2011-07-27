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

#include "bluez-manager.h"
#include "server.h"

#include <boost/assign/list_of.hpp>

using namespace GDBusCXX;

SE_BEGIN_CXX

BluezManager::BluezManager(Server &server) :
    m_server(server),
    m_adapterChanged(*this, "DefaultAdapterChanged")
{
    m_bluezConn = b_dbus_setup_bus(DBUS_BUS_SYSTEM, NULL, true, NULL);
    if(m_bluezConn) {
        m_done = false;
        DBusClientCall1<DBusObject_t> getAdapter(*this, "DefaultAdapter");
        getAdapter(boost::bind(&BluezManager::defaultAdapterCb, this, _1, _2 ));
        m_adapterChanged.activate(boost::bind(&BluezManager::defaultAdapterChanged, this, _1));
    } else {
        m_done = true;
    }
}

void BluezManager::defaultAdapterChanged(const DBusObject_t &adapter)
{
    m_done = false;
    //remove devices that belong to this original adapter
    if(m_adapter) {
        BOOST_FOREACH(boost::shared_ptr<BluezDevice> &device, m_adapter->getDevices()) {
            m_server.removeDevice(device->getMac());
        }
    }
    string error;
    defaultAdapterCb(adapter, error);
}

void BluezManager::defaultAdapterCb(const DBusObject_t &adapter, const string &error)
{
    if(!error.empty()) {
        SE_LOG_DEBUG (NULL, NULL, "Error in calling DefaultAdapter of Interface org.bluez.Manager: %s", error.c_str());
        m_done = true;
        return;
    }
    m_adapter.reset(new BluezAdapter(*this, adapter));
}

BluezManager::BluezAdapter::BluezAdapter(BluezManager &manager, const string &path)
    : m_manager(manager), m_path(path), m_devNo(0), m_devReplies(0),
      m_deviceRemoved(*this,  "DeviceRemoved"), m_deviceAdded(*this, "DeviceCreated")
{
    DBusClientCall1<std::vector<DBusObject_t> > listDevices(*this, "ListDevices");
    listDevices(boost::bind(&BluezAdapter::listDevicesCb, this, _1, _2));
    m_deviceRemoved.activate(boost::bind(&BluezAdapter::deviceRemoved, this, _1));
    m_deviceAdded.activate(boost::bind(&BluezAdapter::deviceCreated, this, _1));
}

void BluezManager::BluezAdapter::listDevicesCb(const std::vector<DBusObject_t> &devices, const string &error)
{
    if(!error.empty()) {
        SE_LOG_DEBUG (NULL, NULL, "Error in calling ListDevices of Interface org.bluez.Adapter: %s", error.c_str());
        checkDone(true);
        return;
    }
    m_devNo = devices.size();
    BOOST_FOREACH(const DBusObject_t &device, devices) {
        boost::shared_ptr<BluezDevice> bluezDevice(new BluezDevice(*this, device));
        m_devices.push_back(bluezDevice);
    }
    checkDone();
}

void BluezManager::BluezAdapter::deviceRemoved(const DBusObject_t &object)
{
    string address;
    std::vector<boost::shared_ptr<BluezDevice> >::iterator devIt;
    for(devIt = m_devices.begin(); devIt != m_devices.end(); ++devIt) {
        if(boost::equals((*devIt)->getPath(), object)) {
            address = (*devIt)->m_mac;
            if((*devIt)->m_reply) {
                m_devReplies--;
            }
            m_devNo--;
            m_devices.erase(devIt);
            break;
        }
    }
    m_manager.m_server.removeDevice(address);
}

void BluezManager::BluezAdapter::deviceCreated(const DBusObject_t &object)
{
    m_devNo++;
    boost::shared_ptr<BluezDevice> bluezDevice(new BluezDevice(*this, object));
    m_devices.push_back(bluezDevice);
}

BluezManager::BluezDevice::BluezDevice (BluezAdapter &adapter, const string &path)
    : m_adapter(adapter), m_path(path), m_reply(false), m_propertyChanged(*this, "PropertyChanged")
{
    DBusClientCall1<PropDict> getProperties(*this, "GetProperties");
    getProperties(boost::bind(&BluezDevice::getPropertiesCb, this, _1, _2));

    m_propertyChanged.activate(boost::bind(&BluezDevice::propertyChanged, this, _1, _2));
}


void BluezManager::BluezDevice::checkSyncService(const std::vector<std::string> &uuids)
{
    static const char * SYNCML_CLIENT_UUID = "00000002-0000-1000-8000-0002ee000002";
    bool hasSyncService = false;
    Server &server = m_adapter.m_manager.m_server;
    BOOST_FOREACH(const string &uuid, uuids) {
        //if the device has sync service, add it to the device list
        if(boost::iequals(uuid, SYNCML_CLIENT_UUID)) {
            hasSyncService = true;
            if(!m_mac.empty()) {
                SyncConfig::DeviceDescription deviceDesc(m_mac, m_name,
                                                         SyncConfig::MATCH_FOR_SERVER_MODE);
                server.addDevice(deviceDesc);
                if(hasPnpInfoService(uuids)) {
                    // TODO: Get the actual manufacturer and device ids.
                    DBusClientCall1<ServiceDict> discoverServices(*this,
                                                                  "DiscoverServices");
                    static const std::string PNP_INFO_UUID("0x1200");
                    discoverServices(PNP_INFO_UUID,
                                     boost::bind(&BluezDevice::discoverServicesCb,
                                                 this, _1, _2));
                }
            }
            break;
        }
    }
    // if sync service is not available now, possible to remove device
    if(!hasSyncService && !m_mac.empty()) {
        server.removeDevice(m_mac);
    }
}

bool BluezManager::BluezDevice::hasPnpInfoService(const std::vector<std::string> &uuids)
{
    static const char * DEVICE_ID_UUID = "00001200-0000-1000-8000-00805f9b34fb";
    BOOST_FOREACH(const string &uuid, uuids) {
        //if the device has teh PnP Infomation service, add it to the Deviec
        if(boost::iequals(uuid, DEVICE_ID_UUID))
            return true;
    }
    return false;
}

/*
 * Parse the XML-formatted service record.
 */
bool extractValuefromServiceRecord(const std::string &serviceRecord,
                                   const std::string &attributeId,
                                   std::string &attributeValue)
{
    // Find atribute
    size_t pos  = serviceRecord.find(attributeId);

    // Only proceed if the attribute id was found.
    if(pos != std::string::npos)
    {
        pos = serviceRecord.find("value", pos + attributeId.size());
        pos = serviceRecord.find("\"", pos) + 1;
        int valLen = serviceRecord.find("\"", pos) - pos;
        attributeValue = serviceRecord.substr(pos, valLen);
        return true;
    }

    return false;
}

void BluezManager::BluezDevice::discoverServicesCb(const ServiceDict &serviceDict,
                                                   const string &error)
{
    static std::map<std::string, std::string> VENDORS =
        boost::assign::map_list_of VENDORS_MAP;
    static std::map<std::string, std::string> PRODUCTS =
        boost::assign::map_list_of PRODUCTS_MAP;

    ServiceDict::const_iterator iter = serviceDict.begin();

    if(iter != serviceDict.end())
    {
        std::string serviceRecord = (*iter).second;

        std::string manId;
        std::string devId;
        if(!serviceRecord.empty())
        {
            extractValuefromServiceRecord(serviceRecord, "0x0201", manId);
            extractValuefromServiceRecord(serviceRecord, "0x0202", devId);
            SE_LOG_INFO(NULL, NULL, "%s[%d]: Vendor: %s, Device: %s",
                        __FILE__, __LINE__, VENDORS[manId].c_str(), PRODUCTS[manId + "_" + devId].c_str());
        }
    }
}

void BluezManager::BluezDevice::getPropertiesCb(const PropDict &props, const string &error)
{
    m_adapter.m_devReplies++;
    m_reply = true;
    if(!error.empty()) {
        SE_LOG_DEBUG (NULL, NULL, "Error in calling GetProperties of Interface org.bluez.Device: %s", error.c_str());
    } else {
        PropDict::const_iterator it = props.find("Name");
        if(it != props.end()) {
            m_name = boost::get<string>(it->second);
        }
        it = props.find("Address");
        if(it != props.end()) {
            m_mac = boost::get<string>(it->second);
        }

        PropDict::const_iterator uuids = props.find("UUIDs");
        if(uuids != props.end()) {
            const std::vector<std::string> uuidVec = boost::get<std::vector<std::string> >(uuids->second);
            checkSyncService(uuidVec);
        }
    }
    m_adapter.checkDone();
}

void BluezManager::BluezDevice::propertyChanged(const string &name,
                                                const boost::variant<vector<string>, string> &prop)
{
    Server &server = m_adapter.m_manager.m_server;
    if(boost::iequals(name, "Name")) {
        m_name = boost::get<std::string>(prop);
        SyncConfig::DeviceDescription device;
        if(server.getDevice(m_mac, device)) {
            device.m_fingerprint = m_name;
            server.updateDevice(m_mac, device);
        }
    } else if(boost::iequals(name, "UUIDs")) {
        const std::vector<std::string> uuidVec = boost::get<std::vector<std::string> >(prop);
        checkSyncService(uuidVec);
    } else if(boost::iequals(name, "Address")) {
        string mac = boost::get<std::string>(prop);
        SyncConfig::DeviceDescription device;
        if(server.getDevice(m_mac, device)) {
            device.m_deviceId = mac;
            server.updateDevice(m_mac, device);
        }
        m_mac = mac;
    }
}

SE_END_CXX
