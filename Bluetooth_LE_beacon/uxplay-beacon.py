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
    print('Install PyGObject pip3 install PyGobject==3.50.0')
    raise SystemExit(1)

import platform
os_name = platform.system()
windows = 'Windows'
linux = 'Linux'
macos = 'Darwin'

advertised_port = None
advertised_address = None


def check_port(port):
    if advertised_port is None or port == advertised_port:
        return True
    else:
        return False


import importlib
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

port = None
advmin = None
advmax = None
ipv4_str = None
index = None

from typing import Optional
def setup_beacon(ipv4_str: str, port: int, advmin: Optional[int], advmax: Optional[int], index: Optional[int]) -> int:
    return 0

def beacon_on() ->bool:
    return False

def beacon_off() ->int:
    return 0

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
    global advertised_port
    advertised_port = beacon_off()
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

def main(file_path_in, ipv4_str_in, advmin_in, advmax_in, index_in):
    global ipv4_str
    global advmin
    global advmax
    global index
    global beacon_is_running
    file_path = file_path_in    
    ipv4_str = ipv4_str_in
    advmin = advmin_in
    advmax = advmax_in    
    index = index_in

    try:
        while True:
            GLib.timeout_add_seconds(1, on_timeout, file_path)
            GLib.timeout_add(200, check_pending)
            mainloop = GLib.MainLoop()
            mainloop.run()
    except KeyboardInterrupt:
        print(f'')
        if beacon_is_running:
            stop_beacon()
        print(f'Exiting ...')
        sys.exit(0)

def get_ipv4():
    if os_name is windows:
        ipv4 = socket.gethostbyname(socket.gethostname())
        return ipv4
    
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

    ble_bluez = "bluez"
    ble_winrt = "winrt"
    ble_bleuio = "bleuio"
        
    # Create an ArgumentParser object
    epilog_text = '''
    Example: python beacon.py --ipv4 192.168.1.100 --advmax 200 --path = ~/my_ble

    Optional arguments in the beacon startup file (if present) will be processed first,
    and will be overridden by any command-line entries.
    Format: one entry (key, value) (or just ble_type) per line, e.g.:
      BleuIO
      --ipv4   192.168.1.100   
    (lines startng with with # are ignored)

    '''

    parser = argparse.ArgumentParser(
        description='A program that runs an AirPlay service discovery BLE beacon.',
        epilog=epilog_text                                                                                                 ,
        formatter_class=argparse.RawTextHelpFormatter
    )

    
    home_dir = os.environ.get('HOME')
    if home_dir is None:
        home_dir = os.path.expanduser("~")
    default_file = home_dir+"/.uxplay.beacon"
    default_ipv4 = "gethostbyname"
    
    # BLE modules
    bleuio = 'BleuIO'
    winrt = 'winrt'
    bluez = 'BlueZ'
    
    # Add arguments
    parser.add_argument(
        'ble_type',
        nargs='?',
        choices=[bleuio, None],
        help=textwrap.dedent('''
        Specifies whether or not to use the module supporting the BleuIO USB dongle, or
        (if not supplied) the default native Linux (BlueZ) or Windows (winrt) modules.
        On systems other than Windows or Linux, BleuIO will be the default choice.
        ''')
    )
    
    parser.add_argument(
        '--file',
        type=str,
        default= default_file,
        help='beacon startup file (Default: ~/.uxplay.beacon).'
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
        default=default_ipv4,
        help='ipv4 address of AirPlay server (default: use gethostbyname).'
    )

    parser.add_argument(
        '--advmin',
        type=str,
        default=None, 
        help='The minimum Advertising Interval (>= 100) units=msec, (default 100, BleuZ, BleuIO only).'
    )
    parser.add_argument(
        '--advmax',
        type=str,
        default=None, 
        help='The maximum Advertising Interval (>= advmin, <= 10240) units=msec, (default 100, BlueZ, BleuIO only).'
    )

    parser.add_argument(
        '--index',
        type=str,
        default=None, 
        help='use index >= 0 to distinguish multiple AirPlay Service Discovery beacons, (default 0, BlueZ only). '
    )

    parser.add_argument(
        '--serial',
        type=str,
        default=None, 
        help='Specify port at which the BleuIO device can be found, (default None).'
    )

    # Parse the command-line arguments
    args = parser.parse_args()
    ipv4_str = None
    path = None
    serial_port = None
    ble_type = None
    config_file = None
    advmin = None
    advmax = None
    index = None

    if args.ble_type is not None:
        ble_type = args.ble_type
        
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
                    elif key == "--index":
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
            print(f'the configuration file {config_file} was not found')
            raise SystemExit(1)
        except IOError:
            print(f'IOError when reading configuration file {config_file}')
            raise SystemExit(1)
        except PermissionError:
            print('fPermissionError when trying to read configuration file {config_file}')
            raise SystemExit(1)

    if args.ble_type is not None:
        ble_type = args.ble_type
    if args.path is not None:
        path = args.path
    if args.ipv4 is not None:
        ipv4_str = args.ipv4
    if args.advmin is not None:
        advmin = args.advmin
    if args.advmax is not None:
        advmax = args.advmax
    if args.index is not None:
        index = args.index
    if args.serial is not None:
        serial_port = args.serial

    if ble_type is None:
        if os_name == windows:
            ble_type = winrt
        elif os_name == linux:
            ble_type = bluez
        else:
            ble_type = bleuio

    if ipv4_str == default_ipv4:
        ipv4_str = get_ipv4()
        if ipv4_str is None:
            print(f'Failed to obtain Server IPv4 address with gethostbyname: provide it with option --ipv4')
            raise SystemExit(1)
    
    if advmin is not None:
        if ble_type == winrt:
            advmin = None
            print(f' --advmin option is not used when ble_type = {winrt}')
    else:
        advmin = 100   #default value
        
    if advmax is not None:
        if ble_type == winrt:
            advmax = None
            print(f' --advmax option is not used when ble_type = {winrt}')
    else:
        advmax = 100   #default value
    if ble_type == winrt:
        advmin = None
        advmax = None
    
    if index is not None:
        if ble_type != bluez:
            index = None
            print(f' --index option is only used when ble_type = {winrt}')
    else:
        index = 0   #default value
    if ble_type != bluez:
        index = None

    module = f'uxplay_beacon_module_{ble_type}'
    print(f'Will use BLE module {module}.py')
    try:
        ble = importlib.import_module(module)
    except ImportError as e:
            print(f'Failed to import {module}: {e}')
            raise SystemExit(1)

    if ble_type == bleuio:
        bleuio_port = ble.find_bleuio(serial_port)
        if bleuio_port is None:
            print(f'No BleuIO devices were found')
            raise SystemExit(1)
        if serial_port is not None and bleuio_port != serial_port:
            print(f'Error: A BlueuIO device was NOT found at the port {serial_port} given as an optional argument')
            print(f'(however BleuIO devices WERE found and are listed above')
            raise SystemExit(1)
        print(f'using the {bleuio} device at  {bleuio_port}')

    if ble_type != winrt:
        advminmax = f'[advmin:advmax]={advmin}:{advmax}'
    else:
        advminmax = f''

    if ble_type == bluez:
        indx = f'index {index}'
    else:
        indx = f''        
 
    print(f'AirPlay Service-Discovery Bluetooth LE beacon: BLE file {path} {advminmax} {indx}')
    print(f'Advertising IP address {ipv4_str}')
    print(f'(Press Ctrl+C to exit)')
    setup_beacon = ble.setup_beacon
    beacon_on = ble.beacon_on
    beacon_off = ble.beacon_off
    main(path, ipv4_str, advmin, advmax, index)
