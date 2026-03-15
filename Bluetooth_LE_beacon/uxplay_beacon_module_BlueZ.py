# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# BlueZ/D-Bus (Linux) module for a standalone python-3.6 AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, March 2026
    
try:
    import dbus
    import dbus.exceptions
    import dbus.mainloop.glib
    import dbus.service
except ImportError as e:
    print(f"ImportError: {e}, failed to import required dbus components")
    print(f"install the python3 dbus package")
    raise SystemExit(1)

ad_manager = None
airplay_advertisement = None
advertised_port = None
advertised_address = None

BLUEZ_SERVICE_NAME = 'org.bluez'
LE_ADVERTISING_MANAGER_IFACE = 'org.bluez.LEAdvertisingManager1'
DBUS_OM_IFACE = 'org.freedesktop.DBus.ObjectManager'
DBUS_PROP_IFACE = 'org.freedesktop.DBus.Properties'

LE_ADVERTISEMENT_IFACE = 'org.bluez.LEAdvertisement1'

class InvalidArgsException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.freedesktop.DBus.Error.InvalidArgs'

class NotSupportedException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.NotSupported'

class NotPermittedException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.NotPermitted'

class InvalidValueLengthException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.InvalidValueLength'

class FailedException(dbus.exceptions.DBusException):
    _dbus_error_name = 'org.bluez.Error.Failed'

class AirPlay_Service_Discovery_Advertisement(dbus.service.Object):
    PATH_BASE = '/org/bluez/airplay_service_discovery_advertisement'

    def __init__(self, bus, index):
        self.path = self.PATH_BASE + str(index)
        self.bus = bus
        self.manufacturer_data = None
        self.min_intrvl = 0
        self.max_intrvl = 0
        dbus.service.Object.__init__(self, bus, self.path)

    def get_properties(self):
        properties = dict()
        properties['Type'] = 'broadcast'
        if self.manufacturer_data is not None:
            properties['ManufacturerData'] = dbus.Dictionary(
                self.manufacturer_data, signature='qv')
        if self.min_intrvl > 0:
            properties['MinInterval'] = dbus.UInt32(self.min_intrvl)
        if self.max_intrvl > 0:
            properties['MaxInterval'] = dbus.UInt32(self.max_intrvl)
        return {LE_ADVERTISEMENT_IFACE: properties}

    def get_path(self):
        return dbus.ObjectPath(self.path)

    def add_manufacturer_data(self, manuf_code, manuf_data):
        if not self.manufacturer_data:
            self.manufacturer_data = dbus.Dictionary({}, signature='qv')
        self.manufacturer_data[manuf_code] = dbus.Array(manuf_data, signature='y')

    def set_min_intrvl(self, min_intrvl):
        if self.min_intrvl == 0:
            self.min_intrvl = 100
        self.min_intrvl = max(min_intrvl, 100)
            
    def set_max_intrvl(self, max_intrvl):
        if self.max_intrvl == 0:
            self.max_intrvl = 100
        self.max_intrvl = max(max_intrvl, 100)

    @dbus.service.method(DBUS_PROP_IFACE,
                         in_signature='s',
                         out_signature='a{sv}')

    def GetAll(self, interface):
        if interface != LE_ADVERTISEMENT_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[LE_ADVERTISEMENT_IFACE]

    @dbus.service.method(LE_ADVERTISEMENT_IFACE,
                         in_signature='',
                         out_signature='')

    def Release(self):
        print(f'{self.path}: Released!')

class AirPlayAdvertisement(AirPlay_Service_Discovery_Advertisement):

    def __init__(self, bus, index, ipv4_str, port, min_intrvl, max_intrvl):
        AirPlay_Service_Discovery_Advertisement.__init__(self, bus, index)
        assert port > 0
        assert port <= 65535
        mfg_data = bytearray([0x09, 0x08, 0x13, 0x30]) # Apple Data Unit type 9 (Airplay), length 8, flags 0001 0011, seed 30
        import ipaddress
        ipv4_address = ipaddress.ip_address(ipv4_str)
        ipv4 = bytearray(ipv4_address.packed)
        mfg_data.extend(ipv4)
        port_bytes = port.to_bytes(2, 'big')
        mfg_data.extend(port_bytes)
        self.add_manufacturer_data(0x004c, mfg_data)
        self.set_min_intrvl(min_intrvl)
        self.set_max_intrvl(max_intrvl)

def register_ad_cb():
    print(f'AirPlay Service_Discovery Advertisement ({advertised_address}:{advertised_port}) registered')

def register_ad_error_cb(error):
    print(f'register_ad: {error}')
    global ad_manager
    global advertised_port
    global advertised_address
    ad_manager = None
    advertised_port = None
    advertised_address = None

def find_adapter(bus):
    remote_om = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, '/'),
                               DBUS_OM_IFACE)
    objects = remote_om.GetManagedObjects()
    for o, props in objects.items():
        if LE_ADVERTISING_MANAGER_IFACE in props:
            return o
    return None

from typing import Optional
def setup_beacon(ipv4_str :str, port :int, advmin :int, advmax :int, index :int ) ->int:
    global ad_manager
    global airplay_advertisement
    global advertised_address
    global advertised_port
    advertised_port = port
    advertised_address = ipv4_str
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)    
    bus = dbus.SystemBus()    
    adapter = find_adapter(bus)
    if not adapter:
        print(f'LEAdvertisingManager1 interface not found')
        return False
    adapter_props = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, adapter),
                                   "org.freedesktop.DBus.Properties")
    adapter_props.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(1))
    ad_manager = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, adapter),
                                LE_ADVERTISING_MANAGER_IFACE)
    airplay_advertisement = AirPlayAdvertisement(bus, index, ipv4_str, port, advmin, advmax)
    return True
    
def beacon_on() ->Optional[int]:
    global airplay_advertisement
    ad_manager.RegisterAdvertisement(airplay_advertisement.get_path(), {},
                                     reply_handler=register_ad_cb,
                                     error_handler=register_ad_error_cb)
    # if registration error occurs, advertised_port is set to None by callback
    if advertised_port is None:   
        airplay_advertisement = None
    return advertised_port
    
def beacon_off():
    global ad_manager
    global airplay_advertisement
    global advertised_port
    global advertised_address
    if ad_manager is not None:
        ad_manager.UnregisterAdvertisement(airplay_advertisement)
        print(f'AirPlay Service-Discovery beacon advertisement unregistered')
        ad_manager = None
    if airplay_advertisement is not None:
        dbus.service.Object.remove_from_connection(airplay_advertisement)
        airplay_advertisement = None
    advertised_Port = None
    advertised_address = None

print(f'loaded uxplay_beacon_module_BlueZ ')
