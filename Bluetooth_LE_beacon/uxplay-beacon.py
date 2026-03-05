#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# a standalone python-3.6 or later DBus-based  AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, October 2025

import sys
if not sys.version_info >= (3,6):
    print("uxplay-beacon.py requires Python 3.6 or higher")
    
import gi
try:
    from gi.repository import GLib
except ImportError as e:
    print(f'ImportError: {e}, failed to import GLib from Python GObject Introspection Library ("gi")')
    printf("Install PyGObject ('pip3 install PyGobject==3.50.0')")
    raise SystemExit(1)
    
import platform
os_name = platform.system()

import ipaddress

advertised_port = None
advertised_address = None

#========= bluez  (dbus)  implementation for Linux ===================
if os_name == "Linux":
    ad_manager = None
    airplay_advertisement = None
    
    try:
        import dbus
        import dbus.exceptions
        import dbus.mainloop.glib
        import dbus.service
    except ImportError as e:
        print(f"ImportError: {e}, failed to import required dbus components")
        printf("install the python3 dbus package")
        raise SystemExit(1)
                
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


    BLUEZ_SERVICE_NAME = 'org.bluez'
    LE_ADVERTISING_MANAGER_IFACE = 'org.bluez.LEAdvertisingManager1'
    DBUS_OM_IFACE = 'org.freedesktop.DBus.ObjectManager'
    DBUS_PROP_IFACE = 'org.freedesktop.DBus.Properties'

    LE_ADVERTISEMENT_IFACE = 'org.bluez.LEAdvertisement1'

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

    def beacon_setup_bluez(ipv4_str, port):
        global ad_manager
        global airplay_advertisement
        global advertised_address
        global advertised_port
        global advmin
        global advmax
        global index

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
    
    def beacon_on_bluez():
        global airplay_advertisement
        ad_manager.RegisterAdvertisement(airplay_advertisement.get_path(), {},
                                         reply_handler=register_ad_cb,
                                         error_handler=register_ad_error_cb)
        if ad_manager is None:
            airplay_advertisement = None
            return  False
        else:
            return True
        
    def beacon_off_bluez():
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

#============== winrt implementation for Windows  =====================================
if os_name == "Windows":
    
    # Import WinRT APIs

    try:
        import winrt.windows.foundation.collections
    except ImportError:
        print(f"ImportError from winrt-Windows.Foundation.Collections")
        print(f"Install with 'pip install winrt-Windows.Foundation.Collections'")
        raise SystemExit(1)

    try:
        import winrt.windows.devices.bluetooth.advertisement as ble_adv
    except ImportError:
        print(f"ImportError from winrt-Windows.Devices.Bluetooth.Advertisement")
        print(f"Install with 'pip install winrt-Windows.Devices.Bluetooth.Advertisement'")
        raise SystemExit(1)

    try:
        import winrt.windows.storage.streams as streams
    except ImportError:
        print(f"ImportError from winrt-Windows.Storage.Streams")
        print(f"Install with 'pip install winrt-Windows.Storage.Streams'")
        raise SystemExit(1)

    import struct
    import asyncio

    #global variables used by winrt.windows.devices.bluetooth.advertisement code
    publisher = None
    
    def on_status_changed(sender, args):
        global publisher
        print(f"Publisher status change to: {args.status.name}")
        if args.status.name == "STOPPED":
            publisher = None

    def create_airplay_service_discovery_advertisement_publisher(ipv4_str, port):
        assert port > 0
        assert port <= 65535
        mfg_data = bytearray([0x09, 0x08, 0x13, 0x30]) # Apple Data Unit type 9 (Airplay), length 8, flags 0001 0011, seed 30
        ipv4_address = ipaddress.ip_address(ipv4_str)
        ipv4 = bytearray(ipv4_address.packed)     
        mfg_data.extend(ipv4)
        port_bytes = port.to_bytes(2, 'big')
        mfg_data.extend(port_bytes)
        writer = streams.DataWriter()
        writer.write_bytes(mfg_data)
        manufacturer_data = ble_adv.BluetoothLEManufacturerData()
        manufacturer_data.company_id = 0x004C   #Apple
        manufacturer_data.data = writer.detach_buffer()
        advertisement = ble_adv.BluetoothLEAdvertisement()
        advertisement.manufacturer_data.append(manufacturer_data)
        global publisher
        global advertised_port
        global advertised_address
        publisher = ble_adv.BluetoothLEAdvertisementPublisher(advertisement)
        advertised_port = port
        advertised_address = ipv4_str
        publisher.add_status_changed(on_status_changed)

    async def publish_advertisement():
        global advertised_port
        global advertised_address
        try:
            publisher.start()
            print(f"AirPlay Service_Discovery Advertisement ({advertised_address}:{advertised_port}) registered")
        except Exception as e:
            print(f"Failed to start Publisher: {e}")
            print(f"Publisher Status: {publisher.status.name}")
            advertised_address = None
            advertised_port = None


    def beacon_setup_winrt(ipv4_str, port):
        create_airplay_service_discovery_advertisement_publisher(ipv4_str, port)
    

    def beacon_on_winrt():
        try:
            asyncio.run( publish_advertisement())
            return True
        except Exception as e:
            print(f"Failed to start publisher: {e}")
            global publisher
            publisher = None
            return False

    
    def beacon_off_winrt():
        publisher.stop()
        global advertised_port
        global advertised_address
        advertised_port = None
        advertised_address = None

        

#====== bleuio (PySerial) implementation for BleuIO usb-serial device ===================

serial_port = None
airplay_advertisement = None
advertisement_parameters = None  

# import pyserial for BleuIO usb-serial device
try:
    import serial
    from serial.tools import list_ports
except ImportError as e:
    print(f"ImportError: {e}, failed to import required serial port support")
    printf("install pyserial")
    raise SystemExit(1)

# --- Serial Communication Helper Functions ---
def send_at_command(serial_port, command):
    # Sends an AT command and reads the response.
    serial_port.write(f"{command}\r\n".encode('utf-8'))
    time.sleep(0.1) # Give the dongle a moment to respond
    response = ""
    while serial_port.in_waiting:
        response += serial_port.readline().decode('utf-8')
    response_without_empty_lines = os.linesep.join(
        [line for line in response.splitlines() if line]
    )
    return response_without_empty_lines    

def beacon_setup_bleuio(ipv4_str, port):
    global advertised_port
    global advertised_address
    global airplay_advertisement
    global advertisement_parameters
    global advmin
    global advmax
    
    #  set up advertising message:
    assert port > 0
    assert port <= 65535
    ipv4_address = ipaddress.ip_address(ipv4_str)
    port_bytes = port.to_bytes(2, 'big')
    data = bytearray([0xff, 0x4c, 0x00]) # ( 3 bytes) type manufacturer_specific 0xff, manufacturer id Apple 0x004c
    data.extend(bytearray([0x09, 0x08, 0x13, 0x30])) #  (4 bytes) Apple Data Unit type 9 (Airplay),  Apple data length 8, Apple flags 0001 0011, seed 30
    data.extend(bytearray(ipv4_address.packed))  # (4 bytes) ipv4 address
    data.extend(port_bytes) # (2 bytes) port
    length = len(data)   # 13 bytes                                                                                                                                 
    adv_data = bytearray([length])   # first byte of message data unit is length of meaningful data that follows (0x0d = 13)
    adv_data.extend(data)
    airplay_advertisement = ':'.join(format(b,'02x') for b in adv_data)
    advertisement_parameters = "0;" + str(advmin) + ";" + str(advmax) + ";0;"  # non-connectable mode, min ad internal, max ad interval, time = unlimited
    advertised_address = ipv4_str
    advertised_port = port

def beacon_on_bleuio():
    global airplay_advertisement
    global advertisement_parameters
    global serial_port
    try:
        print("Connecting to BleuIO dongle on ", serial_port, "....")
        with serial.Serial(serial_port, 115200, timeout = 1) as ser:
            print ("Connection established")
            #Start advertising
            response = send_at_command(ser, "AT+ADVDATA=" +  airplay_advertisement)
            print(response)
            response = send_at_command(ser, "AT+ADVSTART=" + advertisement_parameters)
            print(response)
            print("AirPlay Service Discovery advertising started, port = ", advertised_port, "ip address = ", advertised_address)
            
            return True
    except serial.SerialException as e:
        print(f"beacon_on: Serial port error: {e}")
        return  False
    except Exception as e:
        print(f"beacon_on: An unexpected error occurred: {e}")
        return  False
    
def beacon_off_bleuio():
    global advertisement_parameters
    global airplay_advertisement
    global advertised_port
    global advertised_address
    global serial_port
    
    # Stop advertising
    try:
        with serial.Serial(serial_port, 115200, timeout = 1) as ser:
            response = send_at_command(ser, "AT+ADVSTOP")
            print(response)
            print("AirPlay Service-Discovery beacon advertisement stopped")
            airplay_advertisement = None
            advertised_port = None
            advertised_address = None
            advertisement_parameters = None
    except serial.SerialException as e:
        print(f"beacon_off: Serial port error: {e}")
    except Exception as e:
        print(f"beacon_ff: An unexpected error occurred: {e}")
    
#==generic code below here =============

def check_port(port):
    if advertised_port is None or port == advertised_port:
        return True
    else:
        return False

import argparse
import textwrap
import os
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
    if ble_type == "bluez" :
        beacon_setup_bluez(ipv4_str, port)
        beacon_is_running = beacon_on_bluez()
    elif ble_type == "winrt" :
        beacon_setup_winrt(ipv4_str, port)
        beacon_is_running = beacon_on_winrt()
    elif ble_type == "bleuio" :
        beacon_setup_bleuio(ipv4_str, port)
        beacon_is_running = beacon_on_bleuio()
        

def stop_beacon():
    global beacon_is_running
    if ble_type == "bluez":
        beacon_off_bluez()
    elif ble_type == "winrt":
        beacon_off_winrt()
    elif ble_type == "bleuio":
        beacon_off_bleuio()
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

def main(ble_type_in, file_path_in, ipv4_str_in, advmin_in, advmax_in, index_in, serial_port_in):
    global ipv4_str
    global advmin
    global advmax
    global index
    global ble_type
    global serial_port
    global dbus
    
    ipv4_str = ipv4_str_in
    advmin = advmin_in
    advmax = advmax_in    
    index = index_in
    ble_type = ble_type_in
    serial_port = serial_port_in
    file_path = file_path_in

    try:
        while True:
            GLib.timeout_add_seconds(1, on_timeout, file_path)
            GLib.timeout_add(200, check_pending)
            mainloop = GLib.MainLoop()
            mainloop.run()
    except KeyboardInterrupt:
        print(f'\nExiting ...')
        sys.exit(0)

        
#check AdvInterval
def check_adv_intrvl(min, max):
    if not (100 <= min):
        raise ValueError('advmin was smaller than 100 msecs')
    if not (max >= min):
        raise ValueError('advmax  was smaller than advmin')
    if not (max <= 10240):
        raise ValueError('advmax was larger than 10240 msecs')

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

def select_ble_implementation():

    if os_name == "Windows":
        print('Operating System is detected as "Windows": selecting ble_type = winrt')
        return "winrt"
    elif os_name == "Linux":
        print('Operating System is detected as "Linux": selecting ble_type = bleuz')        
        return "bluez"
    elif os_name == "Darwin":
        print('Operating System is detected as "', os_name,'" (macOS)')
    else:
        print('Operating System is detected as "', os_name,'"')
        print('only Bluetooth-LE on the the BleuIO usb-serial device is supported: selecting ble_type = blueio ')        
        return "bleuio"


if __name__ == '__main__':

    ble_bluez = "bluez"
    ble_winrt = "winrt"
    ble_bleuio = "bleuio"
        
    # Create an ArgumentParser object
    parser = argparse.ArgumentParser(
        description='A program that runs an AirPlay service discovery BLE beacon.',
        epilog='Example: python beacon.py bluez --ipv4 "192.168.1.100" --path "/home/user/ble" --advmin 100 --advmax 100"',
        formatter_class=argparse.RawTextHelpFormatter
    )
    
    home_dir = os.path.expanduser("~")
    default_file = home_dir+"/.uxplay.beacon"
    
    # Add arguments
    parser.add_argument(
        'ble_type',
        nargs='?',
        choices=['bluez', 'winrt', 'bleuio', None],
        help=textwrap.dedent('''
        Specifies one of three Bluetooth-LE implementations: (can be omitted if provided in a startup file)
          bluez:  only for Linux, uses the official Linux Bluetooth stack BlueZ (default on Linux).
          winrt:  only for Windows, uses the native Windows Bluetooth stack (default on Windows).
          bleuio: for the BleuIO usb-serial dongle with its own BlueTooth stack (LE only), on any OS (e.g. macOS)
        ''')
    )
    
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
        '--advmin',
        type=str,
        default="0", 
        help='The minimum Advertising Interval (>= 100) units=msec, (default 100).\n***NOT VALID FOR ble_type = winrt '
    )
    parser.add_argument(
        '--advmax',
        type=str,
        default="0", 
        help='The maximum Advertising Interval (>= advmin, <= 10240) units=msec, (default 100). \n***NOT VALID FOR ble_type = winrt '
    )

    parser.add_argument(
        '--index',
        type=str,
        default="0", 
        help='use index >= 0 to distinguish multiple AirPlay Service Discovery beacons, (default 0).\n***ONLY VALID FOR ble_type = bluez '
    )

    parser.add_argument(
        '--serial',
        type=str,
        default=None, 
        help='Specify port at which the BleuIO device can be found, (default None)\n***ONLY VALID FOR ble_type = bleuio'
    )

    # Parse the command-line arguments
    args = parser.parse_args()
    ipv4_str = None
    path = None
    advmin  = int(100)
    advmax  = int(100)
    index = int(0)
    serial_port = None
    ble_type = None
    config_file = None
    advminmax = False
    haveindex = False

    if args.ble_type is not None:
        ble_type = args.ble_type
    else:
        ble_type = select_ble_implementation()

    if args.file != default_file:
        if os.path.isfile(args.file):
            config_file =  args.file
        else:
            print ("optional argument --file ", args.file, "does not point to a valid file")
            raise SystemExit(1)

    if config_file is None and  os.path.isfile(default_file):
        config_file = default_file
        
    if config_file is not None:
        print("Read uxplay-beacon.py configuration file ", config_file)
        try:
            with open(config_file, 'r')  as file:
                for line in file:
                    stripped_line = line.strip()
                    if stripped_line.startswith('#'):
                        continue
                    parts = stripped_line.partition(" ")
                    part0 = parts[0]
                    part2 = parts[2]
                    key = part0.strip()
                    value = part2.strip()
                    if value == "":
                        if key  != ble_bluez and key != ble_winrt and key != ble_bleuio:
                            print('invalid line "',stripped_line,'" in configuration file ',config_file)
                            raise SystemExit(1)
                        else:
                            if ble_type is None:
                                ble_type = stripped_line
                            continue
                    elif key == "--path":
                        path = value
                    elif key == "--ipv4":
                        ipv4_str = value
                    elif key == "--advmin":
                        advminmax = True
                        if value.isdigit():
                            advmin = int(value)
                        else:
                            print(f'Invalid config file input (--advmin) {value} in {args.file}')
                            raise SystemExit(1)
                    elif key == "--advmax":
                        advminmax = True
                        if value.isdigit():
                            advmax = int(value)
                        else:
                            print(f'Invalid config file input (--advmax) {value} in {args.file}')
                            raise SystemExit(1)
                    elif key == "--index":
                        haveindex = True
                        if value.isdigit():
                            index = int(value)
                        else:
                            print(f'Invalid config file input (--index) {value} in {args.file}')
                            raise SystemExit(1)
                    elif key == "--serial":
                        serial_port = value                
                    else:
                        print(f'Unknown key "{key}" in config file {args.file}')
                        raise SystemExit(1)
        except FileNotFoundError:
            printf("the configuration file ", config_file, "was not found")
            raise SystemExit(1)
        except IOError:
            printf("IOError when reading configuration file ", config_file)
            raise SystemExit(1)
        except PermissionError:
            printf("PermissionError when trying to read configuration file ", config_file)
            raise SystemExit(1)


    # reject input that is incompatible with ble_type

    if ble_type is None:
        print('Error: ble_type (',  ble_bluez, ',', ble_winrt,', or ', ble_bleuio ,') must be specified\n(on the command line or in a configuration file)')
        raise SystemExit(1)

    if ble_type == ble_winrt and os_name != "Windows":
        print ("Error: ble_type",ble_winrt,"is only available on Windows")
        raise SystemExit(1)

    if ble_type == ble_bluez and os_name != "Linux":
        print ("Error: ble_type", ble_bluez,"is only available on Linux")
        raise SystemExit(1)

    if advminmax or args.advmin != '0' or args.advmax !=  '0':
        if ble_type == ble_winrt:
            print('Error: --advmin, --advmax are not valid options when ble_type = ', ble_winrt)
            raise SystemExit(1)

    if haveindex or args.index != '0':
        if ble_type != ble_bluez:
            print('Error: --index is not a valid option unless ble_type = ', ble_bluez)
            raise SystemExit(1)

    if serial_port is not None or args.serial is not None:
        if ble_type != ble_bleuio:
            print('Error: --index is not a valid option unless ble_type = ', ble_bleuio)
            raise SystemExit(1)

        
        
   # parse command line non-positional entries
   
    if args.path is not None:
        path = args.path
   
    if args.ipv4 == "use gethostbyname":
        if (ipv4_str is None):
            ipv4_str = get_ipv4()
        else:
            ipv4_str = args.ipv4

    if args.advmin != "0":
        if args.advmin.isdigit():
            advmin = int(args.advmin)
        else:
            print(f'Invalid input (advmin) {args.advmin}')
            raise SystemExit(1)
        
    if args.advmax != "0":
        if args.advmax.isdigit():
            advmax = int(args.advmax)
        else:
            print(f'Invalid input (advmin) {args.advmin}')
            raise SystemExit(1)
        
    if args.index != "0":
        if args.index.isdigit():
            index = int(args.index)
        else:
            print(f'Invalid input (advmin) {args.advmin}')
            raise SystemExit(1)

    if args.serial is not None: 
        serial_port = args.serial
        if not os.path.isfile(serial_port):
            printf("specified BleuIO device serial port ", serial_port, " is not a valid serial port") 
            raise SystemExit(1)
        
    if index <  0:
        raise ValueError('index was negative (forbidden)')

        
    try:
        check_adv_intrvl(advmin, advmax)
    except ValueError as e:
        print(f'Error: {e}')
        raise SystemExit(1)

    if ble_type == ble_bleuio:
        serial_ports = list(list_ports.comports())
        count = 0
        serial_port_found = False
        for p in serial_ports:
            if "BleuIO" not in p.description:
                continue
            count+=1
            if serial_port is None:
                serial_port = p.device
            if serial_port == p.device:
                serial_port_found = True
            print ("=== detected BlueuIO port ", count,': ', p.description, p.device)
            
        if serial_port is not None and serial_port_found is False:
            print("The serial port ", serial_port, " specified as an optional argument is not a detected  BleuIO device")
            raise SystemExit(1)
        
        if serial_port_found is False:
            print("No BleuIO device was found: stopping")
            print("If a BleuIO device is in fact present, you can specify its port with the \"--serial=...\" option.")
            raise SystemExit(1)

        if count>1:
            print("warning: ", count, " BleueIO devices were found, the first found will be used")
            print("(to override this choice, specify \"--serial_port=...\"in optional arguments")
            
        print( "using ", serial_port, " as the BleuIO device")
    
    
    print(f'AirPlay Service-Discovery Bluetooth LE beacon (',ble_type,'): using BLE file',path,'advmin:advmax =',advmin,':',advmax,'index =',index)
    print(f'(Press Ctrl+C to exit)')
    main(ble_type, path, ipv4_str, advmin, advmax, index, serial_port)
 
