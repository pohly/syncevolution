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

#ifndef BLUEZ_MANAGER_H
#define BLUEZ_MANAGER_H

#include <string>

#include "gdbus/gdbus-cxx-bridge.h"

#include <syncevo/declarations.h>

SE_BEGIN_CXX

class Server;

/**
 * Query bluetooth devices from org.bluez
 * The basic workflow is:
 * 1) get default adapter from bluez by calling 'DefaultAdapter' method of org.bluez.Manager
 * 2) get all devices of the adapter by calling 'ListDevices' method of org.bluez.Adapter
 * 3) iterate all devices and get properties for each one by calling 'GetProperties' method of org.bluez.Device.
 *    Then check its UUIDs whether it contains sync services and put it in the sync device list if it is
 *
 * To track changes of devices dynamically, here also listen signals from bluez:
 * org.bluez.Manager - DefaultAdapterChanged: default adapter is changed and thus have to get its devices
 *                                            and update sync device list
 * org.bluez.Adapter - DeviceCreated, DeviceRemoved: device is created or removed and device list is updated
 * org.bluez.Device - PropertyChanged: property is changed and device information is changed and tracked
 *
 * This class is to manage querying bluetooth devices from org.bluez. Also
 * it acts a proxy to org.bluez.Manager.
 */
class BluezManager : public GDBusCXX::DBusRemoteObject {
public:
    BluezManager(Server &server);

    virtual const char *getDestination() const {return "org.bluez";}
    virtual const char *getPath() const {return "/";}
    virtual const char *getInterface() const {return "org.bluez.Manager";}
    virtual DBusConnection *getConnection() const {return m_bluezConn.get();}
    bool isDone() { return m_done; }

private:
    class BluezDevice;

    /**
     * This class acts a proxy to org.bluez.Adapter.
     * Call methods of org.bluez.Adapter and listen signals from it
     * to get devices list and track its changes
     */
    class BluezAdapter: public GDBusCXX::DBusRemoteObject
    {
     public:
        BluezAdapter (BluezManager &manager, const std::string &path);

        virtual const char *getDestination() const {return "org.bluez";}
        virtual const char *getPath() const {return m_path.c_str();}
        virtual const char *getInterface() const {return "org.bluez.Adapter";}
        virtual DBusConnection *getConnection() const {return m_manager.getConnection();}
        void checkDone(bool forceDone = false)
        {
            if(forceDone || m_devReplies >= m_devNo) {
                m_devReplies = m_devNo = 0;
                m_manager.setDone(true);
            } else {
                m_manager.setDone(false);
            }
        }

        std::vector<boost::shared_ptr<BluezDevice> >& getDevices() { return m_devices; }

     private:
        /** callback of 'ListDevices' signal. Used to get all available devices of the adapter */
        void listDevicesCb(const std::vector<GDBusCXX::DBusObject_t> &devices, const std::string &error);

        /** callback of 'DeviceRemoved' signal. Used to track a device is removed */
        void deviceRemoved(const GDBusCXX::DBusObject_t &object);

        /** callback of 'DeviceCreated' signal. Used to track a new device is created */
        void deviceCreated(const GDBusCXX::DBusObject_t &object);

        BluezManager &m_manager;
        /** the object path of adapter */
        std::string m_path;
        /** the number of device for the default adapter */
        int m_devNo;
        /** the number of devices having reply */
        int m_devReplies;

        /** all available devices */
        std::vector<boost::shared_ptr<BluezDevice> > m_devices;

        /** represents 'DeviceRemoved' signal of org.bluez.Adapter*/
        GDBusCXX::SignalWatch1<GDBusCXX::DBusObject_t> m_deviceRemoved;
        /** represents 'DeviceAdded' signal of org.bluez.Adapter*/
        GDBusCXX::SignalWatch1<GDBusCXX::DBusObject_t> m_deviceAdded;

        friend class BluezDevice;
    };

    /**
     * This class acts a proxy to org.bluez.Device.
     * Call methods of org.bluez.Device and listen signals from it
     * to get properties of device and track its changes
     */
    class BluezDevice: public GDBusCXX::DBusRemoteObject
    {
     public:
        typedef std::map<std::string, boost::variant<std::vector<std::string>, std::string > > PropDict;
        typedef std::map<uint32_t, std::string> ServiceDict;

        BluezDevice (BluezAdapter &adapter, const std::string &path);

        virtual const char *getDestination() const {return "org.bluez";}
        virtual const char *getPath() const {return m_path.c_str();}
        virtual const char *getInterface() const {return "org.bluez.Device";}
        virtual DBusConnection *getConnection() const {return m_adapter.m_manager.getConnection();}
        std::string getMac() { return m_mac; }

        /**
         * check whether the current device has sync service if yes,
         * put it in the adapter's sync devices list
         */
        void checkSyncService(const std::vector<std::string> &uuids);

        /**
         * check whether the current device has the PnP Information service.
         */
        bool hasPnpInfoService(const std::vector<std::string> &uuids);

     private:
        /** callback of 'GetProperties' method. The properties of the device is gotten */
        void getPropertiesCb(const PropDict &props, const std::string &error);

        /** callback of 'DiscoverServices' method. The serve detals are retrieved */
        void discoverServicesCb(const ServiceDict &serviceDict, const std::string &error);

        /** callback of 'PropertyChanged' signal. Changed property is tracked */
        void propertyChanged(const std::string &name, const boost::variant<std::vector<std::string>, std::string> &prop);

        BluezAdapter &m_adapter;
        /** the object path of the device */
        std::string m_path;
        /** name of the device */
        std::string m_name;
        /** mac address of the device */
        std::string m_mac;
        /** whether the calling of 'GetProperties' is returned */
        bool m_reply;

        typedef GDBusCXX::SignalWatch2<std::string, boost::variant<std::vector<std::string>, std::string> > PropertySignal;
        /** represents 'PropertyChanged' signal of org.bluez.Device */
        PropertySignal m_propertyChanged;

        friend class BluezAdapter;
    };

    /*
     * check whether the data is generated. If errors, force initilization done
     */
    void setDone(bool done) { m_done = done; }

    /** callback of 'DefaultAdapter' method to get the default bluetooth adapter  */
    void defaultAdapterCb(const GDBusCXX::DBusObject_t &adapter, const std::string &error);

    /** callback of 'DefaultAdapterChanged' signal to track changes of the default adapter */
    void defaultAdapterChanged(const GDBusCXX::DBusObject_t &adapter);

    Server &m_server;
    GDBusCXX::DBusConnectionPtr m_bluezConn;
    boost::shared_ptr<BluezAdapter> m_adapter;

    /** represents 'DefaultAdapterChanged' signal of org.bluez.Adapter*/
    GDBusCXX::SignalWatch1<GDBusCXX::DBusObject_t> m_adapterChanged;

    /** flag to indicate whether the calls are all returned */
    bool m_done;
};

SE_END_CXX

// The key is the VendorID and the value the Vendor name.  This list
// was obtained from
// http://www.bluetooth.org/Technical/AssignedNumbers/identifiers.htm
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

// The keys are of the form "VendorID_ProductID", and the value
// "Vendor Model". The VendorID needs to be used as a prefix because
// ProductIDs are only unique per vender. Currently we have no list of
// product IDs from vendors so we add them as we find them.
#define PRODUCTS_MAP                            \
    ("0x0001_0x00e7", "Nokia 5230")

#endif // BLUEZ_MANAGER_H
