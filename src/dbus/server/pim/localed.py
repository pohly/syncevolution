import dbus.service
import os

class Localed(dbus.service.Object):
    """a fake localed systemd implementation"""

    LOCALED_INTERFACE = "org.freedesktop.locale1"
    LOCALED_NAME = "org.freedesktop.locale1"
    LOCALED_PATH = "/org/freedesktop/locale1"
    LOCALED_LOCALE = "Locale"

    def __init__(self, bus=None):
        if bus is None:
            bus = dbus.SystemBus()
        bus_name = dbus.service.BusName(self.LOCALED_NAME, bus=bus)
        dbus.service.Object.__init__(self, bus_name, self.LOCALED_PATH)

        locale = []
        for name in (
            "LANG",
            "LC_CTYPE",
            "LC_NUMERIC",
            "LC_TIME",
            "LC_COLLATE",
            "LC_MONETARY",
            "LC_MESSAGES",
            "LC_PAPER",
            "LC_NAME",
            "LC_ADDRESS",
            "LC_TELEPHONE",
            "LC_MEASUREMENT",
            "LC_IDENTIFICATION",
            ):
            value = os.environ.get(name, None)
            if value != None:
                locale.append('%s=%s' % (name, value))

        self.properties = {
            self.LOCALED_LOCALE : locale,
            }

    def SetLocale(self, locale, invalidate=False):
        self.properties[self.LOCALED_LOCALE] = locale
        if invalidate:
            self.PropertiesChanged(self.LOCALED_INTERFACE,
                                   { },
                                   [ self.LOCALED_LOCALE ])
        else:
            self.PropertiesChanged(self.LOCALED_INTERFACE,
                                   { self.LOCALED_LOCALE: locale },
                                   [])

    # Properties
    @dbus.service.method(dbus.PROPERTIES_IFACE,
                         in_signature='ss',
                         out_signature='v')
    def Get(self, interface_name, property_name):
        return self.GetAll(interface_name)[property_name]

    @dbus.service.method(dbus.PROPERTIES_IFACE,
                         in_signature='s',
                         out_signature='a{sv}')
    def GetAll(self, interface_name):
        if interface_name == self.LOCALED_INTERFACE:
            return self.properties
        else:
            raise dbus.exceptions.DBusException(
                'org.syncevolution.UnknownInterface',
                'The fake localed object does not implement the %s interface'
                % interface_name)

    @dbus.service.signal(dbus.PROPERTIES_IFACE,
                         signature='sa{sv}as')
    def PropertiesChanged(self, interface_name, changed_properties,
                          invalidated_properties):
        pass
