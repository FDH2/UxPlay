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

    

def setup_beacon(ipv4_str, port, advmin, advmax, index):
    print(f"setup_beacon port {ipv4_str}:{port} [{advmin}:{advmax}] ({index})")
    
def beacon_on():
    print(f"beacon_on")
    return True
    
def beacon_off():
    print(f"beacon off")
    
#==generic code (non-dbus) below here =============

import argparse
import os
import sys
import psutil
import struct
import socket

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
    global beacon_is_running
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
    global beacon_is_running
    global beacon_is_pending_on
    global beacon_is_pending_off

    if os.path.exists(file_path):
        with open(file_path, 'rb') as file:
            data = file.read(2)
            port = struct.unpack('<H', data)[0]
            data = file.read(4)
            pid = struct.unpack('<I', data)[0]
            data = file.read()
            pname = data.split(b'\0',1)[0].decode('utf-8')
            last_element_of_pname = os.path.basename(pname)
            test = check_process_name(pid, last_element_of_pname)
            if test == True:
                if not beacon_is_running:
                    beacon_is_pending_on = True
            else:
                if beacon_is_running:
                    print(f'orphan beacon file {file_path} exists, but process {pname} (pid {pid}) is no longer active')
                    # PermissionError prevents deletion of orphan beacon files in Windows systems
                    beacon_is_pending_off = True
    else:
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
            
            GLib.timeout_add_seconds(5, on_timeout, file_path)
            GLib.timeout_add_seconds(1, check_pending)
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
        description='A program that runs an AirPlay service discovery BLE beacon.',
        epilog='Example: python beacon.py --ipv4 "192.168.1.100" --path "/home/user/ble" --AdvMin 100 --AdvMax 100"'
    )

    home_dir = os.environ.get("HOME")
    print(f"homedir = {home_dir}")
    # Add arguments
    parser.add_argument(
        '--file',
        type=str,
        default= home_dir + "/.uxplay.beacon", 
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
        print(f'Using config file: {args.file}')
        if os.path.exists(args.file):
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

    if args.ipv4 == "use gethostbyname":
        if (ipv4_str is None):
            ipv4_str = socket.gethostbyname(socket.gethostname())
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
 
