#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# a standalone python-3.6 or later AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, October 2025

import sys
if not sys.version_info >= (3,6):
    print("uxplay-beacon.py requires Python 3.6 or higher")

import importlib
import argparse
import textwrap
import os
import struct
import socket
import time
import platform
import ipaddress

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
advertised_port = None
port = None
advmin = None
advmax = None
ipv4_str = None
index = None
windows = 'Windows'
linux = 'Linux'
os_name = platform.system()
mainloop = None

# BLE modules
BLEUIO = 'BleuIO'
WINRT = 'winrt'
BLUEZ = 'BlueZ'
HCI = 'HCI'

# external functions that must be supplied by loading a module:
from typing import Optional
def setup_beacon(ipv4_str: str, port: int, advmin: Optional[int], advmax: Optional[int], index: Optional[int]) -> bool:
    return False

def beacon_on() ->Optional[int]:
    return None

def beacon_off():
    return

def find_device(device: Optional[str]) -> Optional[str]:
    return None

#internal functions
def exit(err_text):
    print(err_text)
    raise SystemExit(1)

def start_beacon():
    global beacon_is_running
    global port
    global ipv4_str
    global advmin
    global advmax
    global index
    if beacon_is_running:
        exit('code error, should not happen')
    setup_beacon(ipv4_str, port, advmin, advmax, index)
    advertised_port = beacon_on()
    beacon_is_running = advertised_port is not None
    if not beacon_is_running:
        exit('Failed to start beacon:\ngiving up, check Bluetooth adapter')
        
def stop_beacon():
    global beacon_is_running
    global advertised_port
    beacon_off()
    advertised_port = None
    beacon_is_running = False

def pid_is_running(pid):
    return psutil.pid_exists(pid)

def check_port(port):
    if advertised_port is None or port == advertised_port:
        return True
    else:
        return False

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
    check_pending()
    return True

def main(file_path_in, ipv4_str_in, advmin_in, advmax_in, index_in):
    global ipv4_str
    global advmin
    global advmax
    global index
    file_path = file_path_in    
    ipv4_str = ipv4_str_in
    advmin = advmin_in
    advmax = advmax_in    
    index = index_in

    try:
        while True:
            if mainloop is not None:
                # the BleuZ module is being used, needs a GLib mainloop
                GLib.timeout_add_seconds(1, on_timeout, file_path)
                mainloop.run()
            else:
                on_timeout(file_path)
                time.sleep(1)
                
    except KeyboardInterrupt:
        if mainloop is not None:
            mainloop.quit()  # "just in case, but often redundant, if  GLib's SIGINT handler aready quit the loop"
        print(f'')
        if beacon_is_running:
            stop_beacon()
        print(f'Exiting ...')
        sys.exit(0)

def is_valid_ipv4(ipv4_str):
    try:
        ipaddress.IPv4Address(ipv4_str)
        return True
    except ipaddress.AddressValueError:
        return False
        
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
            exit("failed to obtain local ipv4 address: enter it with option --ipv4 ... ")
    return ipv4

def parse_params():

    # Create an ArgumentParser object
    epilog_text = '''
    Example: python beacon.py --ipv4 192.168.1.100 --advmax 200 --path = ~/my_ble

    Optional arguments in the beacon startup file (if present) will be processed first,
    and will be overridden by any command-line entries.
    Format: one entry (key, value) (or just ble_type) per line, e.g.:
      BleuIO
      --ipv4   192.168.1.100   
    (lines starting with with # are ignored)
    '''

    parser = argparse.ArgumentParser(
        description='A program that runs an AirPlay service discovery BLE beacon.',
        epilog=epilog_text                                                                                                 ,
        formatter_class=argparse.RawTextHelpFormatter
    )

    home_dir = os.environ.get('HOME')
    if home_dir is None:
        home_dir = os.path.expanduser("~")
    default_config_file = home_dir+"/.uxplay.beacon"

    optional_modules = [BLEUIO, HCI]
    
    # Add arguments
    parser.add_argument(
        'ble_type',
        nargs='?',
        choices=optional_modules +  [None],
        help=textwrap.dedent('''
        Allows choice of alternative Bluetooth implementations, supporting the BleuIO
        USB Bluetooth LE serial device, and direct access to the Bluetooth Host
        Controller Interface (HCI, Linux/*BSD only). (If not supplied) the default
        native Linux (BlueZ) or Windows (winrt) modules will be used as appropriate.
        On systems other than Windows or Linux, BleuIO will be the default choice.
        The HCI module requires elevated privileges to be granted to users.
        ''')
    )
    
    parser.add_argument(
        '--file',
        type=str,
        default=None,
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
        default=None,
        help='ipv4 address of AirPlay server (default: use gethostbyname).'
    )

    parser.add_argument(
        '--advmin',
        type=int,
        default=None, 
        help='The minimum Advertising Interval (>= 100) units=msec, (default 100, BlueZ, BleuIO only).'
    )
    parser.add_argument(
        '--advmax',
        type=int,
        default=None, 
        help='The maximum Advertising Interval (>= advmin, <= 10240) units=msec, (default 100, BlueZ, BleuIO only).'
    )

    parser.add_argument(
        '--index',
        type=int,
        default=None, 
        help='use index >= 0 to distinguish multiple AirPlay Service Discovery beacons, (default 0, BlueZ only). '
    )

    parser.add_argument(
        '--device',
        type=str,
        default=None, 
        help='Specify an address for a required device (default None, automatic detection will be attempted).'
    )

    # script input arguments
    config_file = None
    ble_type = None
    path = None
    ipv4_str = None
    advmin = None
    advmax = None
    index = None
    device_address = None

    #parse command line
    args = parser.parse_args()

    # look for a configuration file
    if args.file is not None:
        if os.path.isfile(args.file):
            config_file =  args.file
        else:
            err = f'optional argument --file "{args.file}" does not point to a valid file'
            exit(err)
    if config_file is None and  os.path.isfile(default_config_file):
        config_file = default_config_file

    # read configuration file,if present
    if config_file is not None:
        print("Read uxplay-beacon.py configuration file ", config_file)
        try:
            with open(config_file, 'r')  as file:
                for line in file:
                    if line.startswith('#'):
                        continue
                    err = f'Invalid line "{line}" in configuration file'                    
                    stripped_line = line.strip()
                    parts = stripped_line.partition(" ")
                    key = parts[0].strip()
                    value = parts[2].strip()
                    if value == "":
                        if not key in optional_modules:
                            exit(err)
                        ble_type = key
                        continue
                    if key == "--path":
                        path = value
                        continue
                    elif key == "--ipv4":
                        if not is_valid_ipv4(value):
                            print(f'{value} is not a valid IPv4 address')
                            exit(err)
                        ipv4_str = value
                        continue
                    elif key == "--advmin":
                        if not value.isdigit():
                            exit(err)
                        advmin = int(value)
                        continue
                    elif key == "--advmax":
                        if not value.isdigit():
                            exit(err)
                        advmax = int(value)
                        continue
                    elif key == "--index":
                        if not value.isdigit():
                            exit(err)
                        index = int(value)
                        continue
                    elif key == '--device':
                        device_address = value
                        continue
                    else:    
                        exit(err)
        except FileNotFoundError:
            err = f'the configuration file {config_file} was not found'
        except PermissionError:
            err = f'PermissionError when trying to read configuration file {config_file}'
        except IOError:
            err = f'IOError when reading configuration file {config_file}'
        finally:
            exit(err)
            
    # overwrite configuration file entries with command line entries
    if args.ble_type is not None:
        ble_type = args.ble_type
    if args.path is not None:
        path = args.path
    if args.ipv4 is not None:
        ipv4_str = args.ipv4
        if not is_valid_ipv4(ipv4_str):
            err = f'{ipv4_str} is not a valid IPv4 address'
            exit(err)
    if args.advmin is not None:
        advmin = args.advmin
    if args.advmax is not None:
        advmax = args.advmax
    if args.index is not None:
        index = args.index
    if args.device is not None:
        device_address = args.device

    # determine which Bluetooth LE module will be used
    if ble_type is None:
        if os_name == windows:
            ble_type = WINRT
        elif os_name == linux:
            ble_type = BLUEZ
        else:
            ble_type = BLEUIO

    # IPV4 address
    if ipv4_str is None:
        ipv4_str = get_ipv4()
        if ipv4_str is None:
            exit('Failed to obtain Server IPv4 address with gethostbyname: provide it with option --ipv4')

    #AdvMin, AdvMax
    if advmin is not None:
        if ble_type == WINRT:
            advmin = None
            print(f' --advmin option is not used when ble_type = {ble_type}')
    elif ble_type != WINRT:
        advmin = 100   #default value        
    if advmax is not None:
        if ble_type == WINRT:
            advmax = None
            print(f' --advmax option is not used when ble_type = {ble_type}')
    elif ble_type != WINRT:
        advmax = 100   #default value

    #index (BLEUZ only)
    if index is not None:
        if ble_type != BLUEZ:
            index = None
            print(f' --index option is not used when ble_type = {ble_type}')
    elif ble_type == BLUEZ:
        index = 0   #default value

    #device_address (BLEUIO, HCI only)   
    if device_address is not None:
        if ble_type == BLUEZ or ble_type == WINRT:
            device_address = None
            print(f' --device option is not used when ble_type = {ble_type}')

    return [ble_type, path, ipv4_str, advmin, advmax, index, device_address]

if __name__ == '__main__':
    #global mainloop

    #parse input options
    [ble_type, path, ipv4_str, advmin, advmax, index, device_address] = parse_params()

    if ble_type == BLUEZ:
        # a GLib mainloop is required by the BlueZ module
        import gi
        try:
            from gi.repository import GLib
            mainloop = GLib.MainLoop()
        except ImportError as e:
            print(f'ImportError: {e}, failed to import GLib from Python GObject Introspection Library ("gi")')
            print('Install PyGObject pip3 install PyGobject==3.50.0')
            exit('You may need to use pip option "--break-system-packages" (disregard the warning)')

    # import module for chosen ble_type
    module = f'uxplay_beacon_module_{ble_type}'
    print(f'Will use BLE module {module}.py')
    try:
        ble = importlib.import_module(module)
    except ImportError as e:
            err =f'Failed to import {module}: {e}'
            exit(err)
    setup_beacon = ble.setup_beacon
    beacon_on = ble.beacon_on
    beacon_off = ble.beacon_off

    need_device = False    
    if ble_type == BLEUIO or ble_type == HCI:
        # obtain serial port for BleuIO device, or a  Bluetooth >= 4.0 HCI device  for HCI module
        find_device = ble.find_device
        need_device = True

    if need_device:
        use_device  = find_device(device_address)
        if use_device is None:
            err = f'No devices  needed for BLE type {ble_type} were found'
            exit(err)
        if device_address is not None and use_device != device_address:
            print(f'Error: A required device was NOT found at  {device_address} given as an optional argument')
            exit('(Note: required devices WERE found and are listed above)')
        print(f'using the required device found at {use_device}')
    else:
        #start beacon as test to see if Bluetooth is available, (WINRT and BLUEZ)
        test = None
        # initial test to see if Bluetooth is available
        setup_beacon(ipv4_str, 1, advmin, advmax, index)
        test = beacon_on()
        beacon_off()
        if test is not None:
            print(f"test passed ({ble_type}")
            
    advminmax = f''
    indx = f''        
    if ble_type != WINRT:
        advminmax = f'[advmin:advmax]={advmin}:{advmax}'
    if ble_type == BLUEZ:
        indx = f'index {index}'    
    print(f'AirPlay Service-Discovery Bluetooth LE beacon: BLE file {path} {advminmax} {indx}')
    print(f'Advertising IP address {ipv4_str}')
    print(f'(Press Ctrl+C to exit)')
    main(path, ipv4_str, advmin, advmax, index)
