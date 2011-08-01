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

#include <algorithm>

#include <boost/assign/list_of.hpp>

using namespace GDBusCXX;

/*
 * Define the Vendor and Product lookup tables
 *
 * The key is the VendorID and the value the Vendor name.  This list
 * was obtained from
 * http://www.bluetooth.org/Technical/AssignedNumbers/identifiers.htm
 */
#define VENDORS_MAP                                             \
    ("0x0000", "Ericsson Technology Licensing")                 \
    ("0x0001", "Nokia Mobile Phones")                           \
    ("0x0002", "Intel Corp.")                                   \
    ("0x0003", "IBM Corp.")                                     \
    ("0x0004", "Toshiba Corp.")                                 \
    ("0x0005", "3Com")                                          \
    ("0x0006", "Microsoft")                                     \
    ("0x0007", "Lucent")                                        \
    ("0x0008", "Motorola")                                      \
    ("0x0009", "Infineon Technologies AG")                      \
    ("0x000A", "Cambridge Silicon Radio")                       \
    ("0x000B", "Silicon Wave")                                  \
    ("0x000C", "Digianswer A/S")                                \
    ("0x000D", "Texas Instruments Inc.")                        \
    ("0x000E", "Parthus Technologies Inc.")                     \
    ("0x000F", "Broadcom Corporation")                          \
    ("0x0010", "Mitel Semiconductor")                           \
    ("0x0011", "Widcomm, Inc.")                                 \
    ("0x0012", "Zeevo, Inc.")                                   \
    ("0x0013", "Atmel Corporation")                             \
    ("0x0014", "Mitsubishi Electric Corporation")               \
    ("0x0015", "RTX Telecom A/S")                               \
    ("0x0016", "KC Technology Inc.")                            \
    ("0x0017", "Newlogic")                                      \
    ("0x0018", "Transilica, Inc.")                              \
    ("0x0019", "Rohde & Schwarz GmbH & Co. KG")                 \
    ("0x001A", "TTPCom Limited")                                \
    ("0x001B", "Signia Technologies, Inc.")                     \
    ("0x001C", "Conexant Systems Inc.")                         \
    ("0x001D", "Qualcomm")                                      \
    ("0x001E", "Inventel")                                      \
    ("0x001F", "AVM Berlin")                                    \
    ("0x0020", "BandSpeed, Inc.")                               \
    ("0x0021", "Mansella Ltd")                                  \
    ("0x0022", "NEC Corporation")                               \
    ("0x0023", "WavePlus Technology Co., Ltd.")                 \
    ("0x0024", "Alcatel")                                       \
    ("0x0025", "Philips Semiconductors")                        \
    ("0x0026", "C Technologies")                                \
    ("0x0027", "Open Interface")                                \
    ("0x0028", "R F Micro Devices")                             \
    ("0x0029", "Hitachi Ltd")                                   \
    ("0x002A", "Symbol Technologies, Inc.")                     \
    ("0x002B", "Tenovis")                                       \
    ("0x002C", "Macronix International Co. Ltd.")               \
    ("0x002D", "GCT Semiconductor")                             \
    ("0x002E", "Norwood Systems")                               \
    ("0x002F", "MewTel Technology Inc.")                        \
    ("0x0030", "ST Microelectronics")                           \
    ("0x0031", "Synopsys")                                      \
    ("0x0032", "Red-M (Communications) Ltd")                    \
    ("0x0033", "Commil Ltd")                                    \
    ("0x0034", "Computer Access Technology Corporation (CATC)") \
    ("0x0035", "Eclipse (HQ Espana) S.L.")                      \
    ("0x0036", "Renesas Technology Corp.")                      \
    ("0x0037", "Mobilian Corporation")                          \
    ("0x0038", "Terax")                                         \
    ("0x0039", "Integrated System Solution Corp.")              \
    ("0x003A", "Matsushita Electric Industrial Co., Ltd.")      \
    ("0x003B", "Gennum Corporation")                            \
    ("0x003C", "Research In Motion")                            \
    ("0x003D", "IPextreme, Inc.")                               \
    ("0x003E", "Systems and Chips, Inc")                        \
    ("0x003F", "Bluetooth SIG, Inc")                            \
    ("0x0040", "Seiko Epson Corporation")                       \
    ("0x0041", "Integrated Silicon Solution Taiwan, Inc.")      \
    ("0x0042", "CONWISE Technology Corporation Ltd")            \
    ("0x0043", "PARROT SA")                                     \
    ("0x0044", "Socket Mobile")                                 \
    ("0x0045", "Atheros Communications, Inc.")                  \
    ("0x0046", "MediaTek, Inc.")                                \
    ("0x0047", "Bluegiga")                                      \
    ("0x0048", "Marvell Technology Group Ltd.")                 \
    ("0x0049", "3DSP Corporation")                              \
    ("0x004A", "Accel Semiconductor Ltd.")                      \
    ("0x004B", "Continental Automotive Systems")                \
    ("0x004C", "Apple, Inc.")                                   \
    ("0x004D", "Staccato Communications, Inc.")                 \
    ("0x004E", "Avago Technologies")                            \
    ("0x004F", "APT Licensing Ltd.")                            \
    ("0x0050", "SiRF Technology, Inc.")                         \
    ("0x0051", "Tzero Technologies, Inc.")                      \
    ("0x0052", "J&M Corporation")                               \
    ("0x0053", "Free2move AB")                                  \
    ("0x0054", "3DiJoy Corporation")                            \
    ("0x0055", "Plantronics, Inc.")                             \
    ("0x0056", "Sony Ericsson Mobile Communications")           \
    ("0x0057", "Harman International Industries, Inc.")         \
    ("0x0058", "Vizio, Inc.")                                   \
    ("0x0059", "Nordic Semiconductor ASA")                      \
    ("0x005A", "EM Microelectronic-Marin SA")                   \
    ("0x005B", "Ralink Technology Corporation")                 \
    ("0x005C", "Belkin International, Inc.")                    \
    ("0x005D", "Realtek Semiconductor Corporation")             \
    ("0x005E", "Stonestreet One, LLC")                          \
    ("0x005F", "Wicentric, Inc.")                               \
    ("0x0060", "RivieraWaves S.A.S")                            \
    ("0x0061", "RDA Microelectronics")                          \
    ("0x0062", "Gibson Guitars")                                \
    ("0x0063", "MiCommand Inc.")                                \
    ("0x0064", "Band XI International, LLC")                    \
    ("0x0065", "Hewlett-Packard Company")                       \
    ("0x0066", "9Solutions Oy")                                 \
    ("0x0067", "GN Netcom A/S")                                 \
    ("0x0068", "General Motors")                                \
    ("0x0069", "A&D Engineering, Inc.")                         \
    ("0x006A", "MindTree Ltd.")                                 \
    ("0x006B", "Polar Electro OY")                              \
    ("0x006C", "Beautiful Enterprise Co., Ltd.")                \
    ("0x006D", "BriarTek, Inc.")                                \
    ("0x006E", "Summit Data Communications, Inc.")              \
    ("0x006F", "Sound ID")                                      \
    ("0x0070", "Monster, LLC")                                  \
    ("0x0071", "connectBlue AB")

/*
 * The keys are of the form "VendorID_ProductID", and the value
 * "Vendor Model". The VendorID needs to be used as a prefix because
 * ProductIDs are only unique per vender. Currently we have no list of
 * product IDs from vendors so we add them as we find them.
 */
#define PRODUCTS_MAP                            \
    ("0x0001_0x00e7", "Nokia 5230")

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

/**
 * check whether the current device has the PnP Information attribute.
 */
static bool hasPnpInfoService(const std::vector<std::string> &uuids)
{
    // The UUID that indicates the PnPInformation attribute is available.
    static const char * PNPINFOMATION_ATTRIBUTE_UUID = "00001200-0000-1000-8000-00805f9b34fb";

    // Note: GetProperties appears to return this list sorted which binary_search requires.
    if(std::binary_search(uuids.begin(), uuids.end(), PNPINFOMATION_ATTRIBUTE_UUID))
        return true;

    return false;
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

        if(!serviceRecord.empty())
        {
            static const std::string SOURCE_ATTRIBUTE_ID("0x0205");
            std::string sourceId;
            extractValuefromServiceRecord(serviceRecord, SOURCE_ATTRIBUTE_ID, sourceId);

            // A sourceId of 0x001 indicates that the vendor ID was
            // assigned by the Bluetooth SIG.
            // TODO: A sourceId of 0x002, means the vendor id was assigned by
            // the USB Implementor's forum. We do nothing in this case but
            // should do that look up as well.
            if(!boost::iequals(sourceId, "0x0001"))
                return;

            std::string vendorId;
            std::string productId;
            static const std::string VENDOR_ATTRIBUTE_ID ("0x0201");
            static const std::string PRODUCT_ATTRIBUTE_ID("0x0202");
            extractValuefromServiceRecord(serviceRecord, VENDOR_ATTRIBUTE_ID,  vendorId);
            extractValuefromServiceRecord(serviceRecord, PRODUCT_ATTRIBUTE_ID, productId);

            Server &server = m_adapter.m_manager.m_server;
            SyncConfig::DeviceDescription devDesc;
            if (server.getDevice(m_mac, devDesc))
            {
                devDesc.m_pnpInformation =
                    boost::shared_ptr<SyncConfig::PnpInformation>(
                        new SyncConfig::PnpInformation(vendorId,  productId));
                server.updateDevice(m_mac, devDesc);
            }

            // FIXME: Remove this. Just for testing.
            server.getDevice(m_mac, devDesc);
            if(devDesc.m_pnpInformation)
                SE_LOG_INFO(NULL, NULL, "%s[%d]: Vendor: %s, Device: %s",
                            __FILE__, __LINE__,
                            VENDORS [devDesc.m_pnpInformation->m_manufacturerId].c_str(),
                            PRODUCTS[devDesc.m_pnpInformation->m_manufacturerId + "_" +
                                     devDesc.m_pnpInformation->m_deviceId].c_str());
            else
                SE_LOG_INFO(NULL, NULL, "%s[%d]: %s", __FILE__, __LINE__, "Oops!");
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
