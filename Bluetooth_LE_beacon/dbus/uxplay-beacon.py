#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# a standalone python-3.6 or later DBus-based  AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, October 2025

import gi
try:
    from gi.repository import GLib
except ImportError as e:
    print(f'ImportError: {e}, failed to import GLib from Python GObject Introspection Library ("gi")')
    printf("install the python3 gi package")
    raise SystemExit(1)
    
try:
    import dbus
    import dbus.exceptions
    import dbus.mainloop.glib
    import dbus.service
except ImportError as e:
    print(f"ImportError: {e}, failed to import required dbus components")
    printf("install the python3 dbus package")
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
    print(f'Failed to register advertisement: {error}')
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


def setup_beacon(ipv4_str, port, advmin, advmax, index):
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
        return
    adapter_props = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, adapter),
                                   "org.freedesktop.DBus.Properties")

    adapter_props.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(1))

    ad_manager = dbus.Interface(bus.get_object(BLUEZ_SERVICE_NAME, adapter),
                                LE_ADVERTISING_MANAGER_IFACE)
    airplay_advertisement = AirPlayAdvertisement(bus, index, ipv4_str, port, advmin, advmax)
    
def beacon_on():
    global airplay_advertisement
    ad_manager.RegisterAdvertisement(airplay_advertisement.get_path(), {},
                                     reply_handler=register_ad_cb,
                                     error_handler=register_ad_error_cb)
    if ad_manager is None:
        airplay_advertisement = None
        return  False
    else:
        return True
    
def beacon_off():
    global ad_manager
    global airplay_advertisement
    global advertised_port
    global advertised_address
    ad_manager.UnregisterAdvertisement(airplay_advertisement)
    print(f'AirPlay Service-Discovery beacon advertisement unregistered')
    ad_manager = None
    dbus.service.Object.remove_from_connection(airplay_advertisement)
    airplay_advertisement = None
    advertised_Port = None
    advertised_address = None


#==generic code (non-dbus) below here =============

def check_port(port):
    if advertised_port is None or port == advertised_port:
        return True
    else:
        return False

import argparse
import os
import sys
import struct
import socket
import time
try:
    import psutil
except ImportError as e:
    print(f'ImportError {e}: failed to import psutil')
    print(f' install the python3 psutil package')
    raise SystemExit(1)

# global variables
beacon_is_running = False
beacon_is_pending_on = False
beacon_is_pending_off = False

port = int(0)
advmin = int(100)
advmax = int(100)
ipv4_str = "ipv4_address"
index = int(0)

def start_beacon():
    global beacon_is_running
    global port
    global ipv4_str
    global advmin
    global advmax
    global index
    setup_beacon(ipv4_str, port, advmin, advmax, index)
    beacon_is_running = beacon_on()

def stop_beacon():
    global beacon_is_running
    beacon_off()
    beacon_is_running = False

def pid_is_running(pid):
    return psutil.pid_exists(pid)

def check_process_name(pid, pname):
    try:
        process = psutil.Process(pid)
        if process.name().find(pname,0) == 0:
            return True
        else:
            return False
    except psutil.NoSuchProcess:
        return False

def check_pending():
    global beacon_is_pending_on
    global beacon_is_pending_off
    if beacon_is_running:
        if beacon_is_pending_off:
            stop_beacon()
            beacon_is_pending_off = False
    else:
        if beacon_is_pending_on:
            start_beacon()
            beacon_is_pending_on = False
    return True
            

def check_file_exists(file_path):
    global port
    global beacon_is_pending_on
    global beacon_is_pending_off
    pname = "process name unread"
    if os.path.isfile(file_path):
        test = True
        try:
            with open(file_path, 'rb') as file:
                data = file.read(2)
                port = struct.unpack('<H', data)[0]
                data = file.read(4)
                pid = struct.unpack('<I', data)[0]
                if not pid_is_running(pid):
                    file.close()
                    test = False
                if test:
                    data = file.read()
                    file.close()
                    pname = data.split(b'\0',1)[0].decode('utf-8')
                    last_element_of_pname = os.path.basename(pname)
                    test = check_process_name(pid, last_element_of_pname)
        except IOError:
            test = False
        except FileNotFoundError:
            test = False
        if test:
            if not beacon_is_running:
                beacon_is_pending_on = True
            else:
                if not check_port(port):
                    # uxplay is active, and beacon is running but is advertising a different port, so shut it down
                    beacon_is_pending_off = True
        else:
            print(f'Orphan beacon file exists, but process pid {pid} ({pname}) is no longer active')
            try:
                os.remove(file_path)
                print(f'Orphan beacon file "{file_path}" deleted successfully.')
            except FileNotFoundError:
                print(f'File "{file_path}" not found.')
            except PermissionError as e:
                print(f'Permission Errror {e}: cannot delete  "{file_path}".')
            if beacon_is_running:
                beacon_is_pending_off = True
    
    else:    #BLE file does not exist
        if beacon_is_running:
            beacon_is_pending_off = True

def on_timeout(file_path):
    check_file_exists(file_path)
    return True


def process_input(value):
    try:
        my_integer = int(value)
        return my_integer
    except ValueError:
        print(f'Error: could not convert "{value}" to integer: {my_integer}')
        return None


#check AdvInterval
def check_adv_intrvl(min, max):
    if not (100 <= min):
        raise ValueError('AdvMin was smaller than 100 msecs')
    if not (max >= min):
        raise ValueError('AdvMax  was smaller than AdvMin')
    if not (max <= 10240):
        raise ValueError('AdvMax was larger than 10240 msecs')
    

def main(file_path, ipv4_str_in, advmin_in, advmax_in, index_in):
    global ipv4_str
    global advmin
    global advmax
    global index 
    ipv4_str = ipv4_str_in
    advmin = advmin_in
    advmax = advmax_in    
    index = index_in

    try:
        while True:
            try:
                check_adv_intrvl(advmin, advmax)
            except ValueError as e:
                print(f'Error: {e}')
                raise SystemExit(1)      
            
            GLib.timeout_add_seconds(1, on_timeout, file_path)
            GLib.timeout_add(200, check_pending)
            mainloop = GLib.MainLoop()
            mainloop.run()
    except KeyboardInterrupt:
        print(f'\nExiting ...')
        sys.exit(0)
        

def get_ipv4():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ipv4 = s.getsockname()[0]
        s.close()
    except socket.error as e:
        print("socket error {e}, will try to get ipv4 with gethostbyname");
        ipv4 = None
    if (ipv4 is not None and ipv4 != "127.0.0.1"):
        return ipv4
    ipv4 = socket.gethostbyname(socket.gethostname())
    if ipv4 == "127.0.1.1": # Debian systems /etc/hosts entry
        try:
            ipv4 = socket.gethostbyname(socket.gethostname()+".local")
        except socket_error:
            print(f"failed to obtain local ipv4 address: enter it with option --ipv4 ... ")
            raise SystemExit(1)
    return ipv4

if __name__ == '__main__':


    if not sys.version_info >= (3,6):
        print("uxplay-beacon.py requires Python 3.6 or higher")
    
    # Create an ArgumentParser object
    parser = argparse.ArgumentParser(
        description='A program that runs an AirPlay service discovery BLE beacon.',
        epilog='Example: python beacon.py --ipv4 "192.168.1.100" --path "/home/user/ble" --AdvMin 100 --AdvMax 100"'
    )

    home_dir = os.path.expanduser("~")
    default_file = home_dir+"/.uxplay.beacon"  
    # Add arguments
    parser.add_argument(
        '--file',
        type=str,
        default= default_file,
        help='beacon startup file (optional): one entry (key, value) per line, e.g. --ipv4 192.168.1.100, (lines startng with with # are ignored)'
    )
    
    parser.add_argument(
        '--path',
        type=str,
        default= home_dir + "/.uxplay.ble", 
        help='path to AirPlay server BLE beacon information file (default: ~/.uxplay.ble)).'
    )
    parser.add_argument(
        '--ipv4',
        type=str,
        default='use gethostbyname',
        help='ipv4 address of AirPlay server (default: use gethostbyname).'
    )

    parser.add_argument(
        '--AdvMin',
        type=str,
        default="0", 
        help='The minimum Advertising Interval (>= 100) units=msec, default 100)'
    )
    parser.add_argument(
        '--AdvMax',
        type=str,
        default="0", 
        help='The maximum Advertising Interval (>= AdvMin, <= 10240) units=msec, default 100)'
    )

    parser.add_argument(
        '--index',
        type=str,
        default="0", 
        help='use index >= 0 to distinguish multiple AirPlay Service Discovery beacons, default 0)'
    )

    # Parse the command-line argunts
    args = parser.parse_args()
    ipv4_str = None
    path = None
    advmin  = int(100)
    advmax  = int(100)
    index = int(0)
    
    if args.file:
        if os.path.exists(args.file):
            print(f'Using config file: {args.file}')
            with open(args.file, 'r')  as file:
                for line in file:
                    stripped_line = line.strip()
                    if stripped_line.startswith('#'):
                        continue
                    parts = stripped_line.partition(" ")
                    part0 = parts[0]
                    part2 = parts[2]
                    key = part0.strip()
                    value = part2.strip()
                    if key == "--path":
                        path = value
                    elif key == "--ipv4":
                        ipv4_str = value
                    elif key == "--AdvMin":
                        if value.isdigit():
                            advmin = int(value)
                        else:
                             print(f'Invalid config file input (--AdvMin) {value} in {args.file}')
                             raise SystemExit(1)
                    elif key == "--AdvMax":
                        if value.isdigit():
                            advmax = int(value)
                        else:
                             print(f'Invalid config file input (--AdvMax) {value} in {args.file}')
                             raise SystemExit(1)
                    elif key == "--index":
                        if value.isdigit():
                            index = int(value)
                        else:
                             print(f'Invalid config file input (--index) {value} in {args.file}')
                             raise SystemExit(1)
                    else:
                        print(f'Unknown key "{key}" in config file {args.file}')
                        raise SystemExit(1)
        else:
            if args.file != default_file:
                print(f"configuration file {args.file} not found")
                raise SystemExit(1)

    if args.ipv4 == "use gethostbyname":
        if (ipv4_str is None):
            ipv4_str = get_ipv4()
    else:
        ipv4_str = args.ipv4

    if args.AdvMin != "0":
        if args.AdvMin.isdigit():
            advmin = int(args.AdvMin)
        else:
            print(f'Invalid input (AdvMin) {args.AdvMin}')
            raise SystemExit(1)
        
    if args.AdvMax != "0":
        if args.AdvMax.isdigit():
            advmax = int(args.AdvMax)
        else:
            print(f'Invalid input (AdvMin) {args.AdvMin}')
            raise SystemExit(1)
        
    if args.index != "0":
        if args.index.isdigit():
            index = int(args.index)
        else:
            print(f'Invalid input (AdvMin) {args.AdvMin}')
            raise SystemExit(1)
    if index <  0:  
        raise ValueError('index was negative (forbidden)')
    
    print(f'AirPlay Service-Discovery Bluetooth LE beacon: using BLE file {args.path}, advmin:advmax {advmin}:{advmax} index:{index}')
    print(f'(Press Ctrl+C to exit)')
    main(args.path, ipv4_str, advmin, advmax, index)
 
