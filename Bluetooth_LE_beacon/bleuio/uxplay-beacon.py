#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# a standalone python-3.6 or later bleuio-based  AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, March 2026


# **** This implementation requires a blueio dongle https://bleuio.com/bluetooth-low-energy-usb-ssd005.php
# This device has a self-contained bluetooth LE stack packaged as a usb serial modem.
# It is needed on macOS because macOS does not permit users to send manufacturer-specific BLE advertisements
# with its native BlueTooth stack.    It works also on linux and windows.

import gi
try:
    from gi.repository import GLib
except ImportError:
    print(f'ImportError: failed to import GLib')
    print(f'Install PyGObject ("pip3 install PyGobject==3.50.0")')
    raise SystemExit(1)

try:
    import serial
    from serial.tools import list_ports
except ImportError as e:
    print(f'ImportError: {e}, failed to import required serial port support')
    print(f'install pyserial')
    raise SystemExit(1)

advertised_port = None
advertised_address = None
serial_port = None
advertisement_parameters = None
airplay_advertisement = None


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

def setup_beacon(ipv4_str, port, advmin, advmax):
    global advertised_port
    global advertised_address
    global airplay_advertisement
    global advertisement_parameters
    
    #  set up advertising message:
    assert port > 0
    assert port <= 65535
    import ipaddress
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

    
def beacon_on():
    global airplay_advertisement
    global advertisement_parameters
    global serial_port
    success = False
    try:
        print(f'Connecting to BleuIO dongle on {serial_port} ....')
        with serial.Serial(serial_port, 115200, timeout = 1) as ser:
            print(f'Connection established')
            #Start advertising
            response = send_at_command(ser, "AT+ADVDATA=" +  airplay_advertisement)
            #print(response)
            response = send_at_command(ser, "AT+ADVSTART=" + advertisement_parameters)
            #print(f'{response}')
            print(f'AirPlay Service Discovery advertising started, port = {advertised_port} ip address = {advertised_address}')
            success = True
    except serial.SerialException as e:
        print(f"beacon_on: Serial port error: {e}")
        raise SystemExit(1)
    except Exception as e:
        print(f"beacon_on: An unexpected error occurred: {e}")
        raise SystemExit(1)
    finally:
        ser.close()
    return success
    
def beacon_off():
    global advertisement_parameters
    global airplay_advertisement
    global advertised_port
    global advertised_address
    global serial_port
     # Stop advertising
    try:
        with serial.Serial(serial_port, 115200, timeout = 1) as ser:
            response = send_at_command(ser, "AT+ADVSTOP")
            #print(f'{response}')
            print(f'AirPlay Service-Discovery beacon advertisement stopped')
            airplay_advertisement = None
            advertised_Port = None
            advertised_address = None
            advertisement_parameters = None
            resullt = True
    except serial.SerialException as e:
        print(f"beacon_off: Serial port error: {e}")
        raise SystemExit(1)
    except Exception as e:
        print(f"beacon_off: An unexpected error occurred: {e}")
        raise SystemExit(1)
    finally:
        ser.close()
            
#==generic code (non-dbus) below here =============

def check_port(port):
    if advertised_port is None or port == advertised_port:
        return True
    else:
        return False

import argparse
import textwrap
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
serial_port = None

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
    setup_beacon(ipv4_str, port, advmin, advmax)
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

def find_bleuio(serial_port_in):
    serial_ports = list(list_ports.comports())
    count = 0
    serial_port_found = False
    serial_port = None
    TARGET_VID = 0x2DCF   # used by BleuIO and BleuIO Pro
    if serial_port_in is not None:
        for p in serial_ports:
            if p.vid is None:
                continue
            if p.vid == TARGET_VID and p.device == serial_port_in:
                return serial_port_in
    for p in serial_ports:
        if p.vid is not None and p.vid == TARGET_VID:
            count+=1
            if count == 1:
                serial_port = p.device
            print(f'=== detected BlueuIO {count}. port: {p.device} desc: {p.description} hwid: {p.hwid}')
            
    if count>1:
        print(f'warning: {count} BleueIO devices were found, the first found will be used')
        print(f'(to override this choice, specify "--serial_port=..." in optional arguments')

    return serial_port

def main(file_path, ipv4_str_in, advmin_in, advmax_in, serial_port_in):
    global ipv4_str
    global advmin
    global advmax
    global serial_port
    ipv4_str = ipv4_str_in
    advmin = advmin_in
    advmax = advmax_in
    serial_port = serial_port_in

    try:
        while True:
            GLib.timeout_add_seconds(1, on_timeout, file_path)
            GLib.timeout_add(200, check_pending)
            mainloop = GLib.MainLoop()
            mainloop.run()
    except KeyboardInterrupt:
        print('\nExiting ...')
        sys.exit(0)
        

#check AdvInterval
def check_adv_intrvl(min, max):
    if not (100 <= min):
        raise ValueError('advmin was smaller than 100 msecs')
    if not (max >= min):
        raise ValueError('advmax  was smaller than advmin')
    if not (max <= 10240):
        raise ValueError('advmax was larger than 10240 msecs')

#get_ipv4 
def get_ipv4():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ipv4 = s.getsockname()[0]
        s.close()
    except socket.error as e:
        print(f'socket error {e}, will try to get ipv4 with gethostbyname');
        ipv4 = None
    if (ipv4 is not None and ipv4 != "127.0.0.1"):
        return ipv4
    ipv4 = socket.gethostbyname(socket.gethostname())
    if ipv4 == "127.0.1.1": # Debian systems /etc/hosts entry
        try:
            ipv4 = socket.gethostbyname(socket.gethostname()+".local")
        except socket_error:
            print(f'failed to obtain local ipv4 address: enter it with option --ipv4 ... ')
            raise SystemExit(1)
    return ipv4

if __name__ == '__main__':

    if not sys.version_info >= (3,6):
        print(f'uxplay-beacon.py requires Python 3.6 or higher')


    # Create an ArgumentParser object

    epilog_text = '''
    Example: python beacon.py --ipv4 192.168.1.100 --advmax 200 --path = ~/my_ble

    Optional arguments in the beacon startup file (if present) will be processed first.
    and will be overridden by any command-line entries.
    Format: one entry (key, value) per line, e.g.:
      --ipv4   192.168.1.100   
    (lines startng with with # are ignored)

    '''
    
    parser = argparse.ArgumentParser(
        description='A program that runs an AirPlay service discovery BLE beacon on a BleuIO USB device.',
        epilog=epilog_text,
        formatter_class=argparse.RawTextHelpFormatter
     )

    home_dir = os.environ.get('HOME')
    if home_dir is None:
        home_dir = os.path.expanduser("~")
    default_file = home_dir+"/.uxplay.beacon"  

    # Add arguments
    parser.add_argument(
        '--file',
        type=str,
        default=default_file,
        help='beacon startup file (default:  ~/.uxplay.beacon)'
    )
 

    parser.add_argument(
        '--path',
        type=str,
        default= home_dir + "/.uxplay.ble", 
        help='path to AirPlay server BLE beacon information file (default: ~/.uxplay.ble).'
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
        help='The minimum Advertising Interval (>= 100) units=msec, (default 100)'
    )
    parser.add_argument(
        '--advmax',
        type=str,
        default="0", 
        help='The maximum Advertising Interval (>= advmin, <= 10240) units=msec, (default 100)'
    )

    parser.add_argument(
        '--serial',
        type=str,
        default=None, 
        help='Specify port at which the BleuIO device can be found, (default None)'
    )

    # Parse the command-line arguments
    args = parser.parse_args()
    ipv4_str = None
    path = None
    advmin  = int(100)
    advmax  = int(100)
    serial_port = None
    
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
                    elif key == "--advmin":
                        if value.isdigit():
                            advmin = int(value)
                        else:
                             print(f'Invalid config file input (--advmin) {value} in {args.file}')
                             raise SystemExit(1)
                    elif key == "--advmax":
                        if value.isdigit():
                            advmax = int(value)
                        else:
                             print(f'Invalid config file input (--advmax) {value} in {args.file}')
                             raise SystemExit(1)
                    elif key == "--serial":
                        if not os.path.isfile(value):
                            print(f'specified serial_port {value} is not a valid path to a serial port')
                            raise SystemExit(1)
                        serial_port = value
                        
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

    if args.serial is not None:
        serial_port = args.serial

    try:
        check_adv_intrvl(advmin, advmax)
    except ValueError as e:
        print(f'Error: {e}')
        raise SystemExit(1) 

    bleuio_port = find_bleuio(serial_port)
    if bleuio_port is None:
        print(f'No BleuIO devices were found')
        raise SystemExit(1)
    if serial_port is not None and bleuio_port != serial_port:
        print(f'Error: A BlueuIO device was NOT found at the port {serial_port} given as an optional argument')
        print(f'(however BleuIO devices WERE found and are listed above')
        raise SystemExit(1)
    
    print(f'using {bleuio_port} as the BleuIO device')

    print(f'AirPlay Service-Discovery Bluetooth LE beacon: BLE file {args.path}, advmin:advmax {advmin}:{advmax} BleueIO port:{bleuio_port}')
    print(f'(Press Ctrl+C to exit)')
    main(args.path, ipv4_str, advmin, advmax, bleuio_port)
