#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#----------------------------------------------------------------
# a standalone python-3.6 or later winrt-based  AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, October 2025

import gi
try:
    from gi.repository import GLib
except ImportError:
    print(f"ImportError: failed to import GLib")
    raise SystemExit(1)

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
import ipaddress
import asyncio

#global variables used by winrt.windows.devices.bluetooth.advertisement code
publisher = None
advertised_port = None
advertised_address = None

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

    
def setup_beacon(ipv4_str, port):
    create_airplay_service_discovery_advertisement_publisher(ipv4_str, port)
    
def beacon_on():
    try:
        asyncio.run( publish_advertisement())
        return True
    except Exception as e:
        print(f"Failed to start publisher: {e}")
        global publisher
        publisher = None
        return False

    
def beacon_off():
    publisher.stop()
    global advertised_port
    global advertised_address
    advertised_port = None
    advertised_address = None

#==generic code (non-winrt) below here =============

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
    print(f"ImportError {e}: failed to import psutil")
    print(f'Install *-python-psutil (e.g.,"pacman -S  mingw-w64-ucrt-x86_64-python-psutil")') 
    raise SystemExit(1)
    
# global variables
beacon_is_running = False
beacon_is_pending_on = False
beacon_is_pending_off = False

port = int(0)
ipv4_str = "ipv4_address"

def start_beacon():
    global beacon_is_running
    setup_beacon(ipv4_str, port)
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


def main(file_path, ipv4_str_in):
    global ipv4_str
    ipv4_str = ipv4_str_in
 
    try:
        while True:
            GLib.timeout_add_seconds(1, on_timeout, file_path)
            GLib.timeout_add(200, check_pending)
            mainloop = GLib.MainLoop()
            mainloop.run()
    except KeyboardInterrupt:
        print(f'\nExiting ...')
        sys.exit(0)
        

if __name__ == '__main__':
    if not sys.version_info >= (3,6):
        print("uxplay-beacon.py requires Python 3.6 or higher")
    
    # Create an ArgumentParser object
    parser = argparse.ArgumentParser(
        description='A program (for MS Windows systems only) that runs an AirPlay service discovery BLE beacon.',
        epilog='Example: python beacon.py --ipv4 "192.168.1.100" --path "/home/user/ble"'
    )

    home_dir = os.environ.get("HOME")
    default_file = home_dir+"/.uxplay.beacon"
    print(f"homedir = {home_dir}")
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

    # Parse the command-line argunts
    args = parser.parse_args()
    ipv4_str = None
    path = None
    
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
                    else:
                        print(f'Unknown key "{key}" in config file {args.file}')
                        raise SystemExit(1)
        else:
            if (args.file != default_file):
                print(f'configuration file {args.file} not found')
                raise SystemExit(1)

    if args.ipv4 == "use gethostbyname":
        if (ipv4_str is None):
            ipv4_str = socket.gethostbyname(socket.gethostname())
    else:
        ipv4_str = args.ipv4

    print(f'AirPlay Service-Discovery Bluetooth LE beacon: using BLE file {args.path}')
    print(f'(Press Ctrl+C to exit)')
    main(args.path, ipv4_str)
 
