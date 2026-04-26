# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# HCI_Linux (uses sudo hciconfig):  module for a standalone python-3.6 or later AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 

# this requires that users can run "sudo -n hciconfig" without giving a password:
# (1) (as root) create a group like "hciusers"
# (2a) Linux: use visudo to create a file /etc/sudoers.d/hciusers containing  a line
#         %hciusers ALL=(ALL) NOPASSWD: /usr/bin/hcitool, /usr/bin/hciconfig
# (2b) FreeBSD: use visudo to create /usr/local/etc/sudoers.d/hciusers with the line
#         %hciusers ALL=(ALL) NOPASSWD: /usr/sbin/hccontrol
# (3) add the users who will run uxplay-beacon.py to the group hciusers

import subprocess
import time
import re
import subprocess
import platform
from typing import Optional
from typing import Literal

#global variables
hci = None
advertised_port = None
advertised_address = None

os_name = platform.system()
linux =  os_name == 'Linux'
freebsd = os_name == 'FreeBSD'
if not linux and not freebsd:
    print(f'{os_name} is not supported by the HCI module')
    raise SystemExit(1)
 
#help text
help_text1 = '''
   This HCI module requires users of the module to have elevated privileges that
   allow execution of a low-level Bluetooth HCI command using passwordless "sudo":
     (1) As System Administrator, create a group such as "hciusers")
'''
if linux:
    help_text2 = '''
     (2) use visudo to create a file /etc/sudoers.d/hciusers containing the line: 
          %hciusers ALL=(ALL) NOPASSWD: /usr/bin/hciconfig, /usr/bin/hcitool
    '''
elif freebsd:
    disclaimer = '''
    
    ***********************************************************************
    * FreeBSD < 15.1: this module requires a patch to FreeBSD's hccontrol *
    * utility, to add an option first introduced in FreeBSD 15.1. A patch *
    * for older FreeBSD releases is available at the UxPlay Wiki site:    *
    * https://github.com/FDH2/UxPlay/wiki/hccontrol-patch-for-FreeBSD-15.0*
    ***********************************************************************
    '''
    release = platform.release()
    version_str = release.split('-')[0]
    major, minor = map(int, version_str.split('.'))
    if (major < 15) or (major == 15 and minor < 1):
        print(f"FreeBSD release: {release}")
        print(disclaimer)
    
    help_text2 =  '''   
     (2) use visudo to create a file /usr/local/etc/sudoers.d/hciusers containing the line: 
          %hciusers ALL=(ALL) NOPASSWD: /usr/sbin/hccontrol
    '''
help_text3 = '''
     (3) add users of uxplay_beacon_module_HCI.py to the group "hciusers"
'''
help_text = help_text1 + help_text2 + help_text3

sudo = ['sudo', '-n']
if linux:
   ogf = "0x08"
   def le_cmd(hcicmd, args):
      cmd = sudo + ['hcitool', '-i', hci, 'cmd', ogf, hcicmd] + args
      subprocess.run(cmd, capture_output=True, text=True, check=True)
elif freebsd:
   def le_cmd(hcicmd, args):
      cmd = sudo + ['hccontrol', '-n', hci, hcicmd] + args
      subprocess.run(cmd, capture_output=True, text=True, check=True)
        
def setup_beacon(ipv4_str: str, port: int, advmin: int, advmax: int, index: Literal[None]) -> bool:
    global advertised_port
    global advertised_address
    advertised_port = None
    advertised_address = None

    # setup Advertising Parameters
    if linux:
        # convert into  units of 5/8 msec.
        advmin = (advmin * 8) // 5
        advmax = (advmax * 8) // 5
        min1 = f'{advmin %256 :#04x}'
        min2 = f'{advmin //256 :#04x}'
        max1 = f'{advmax % 256 :#04x}'
        max2 = f'{advmax // 256 :#04x}'
        args = [min1, min2, max1, max2, '0x03', '0x00', '0x00'] + ['0x00'] * 6 + ['0x07', '0x00']
        hcicmd = "0x0006"
    elif freebsd:
        min = f'-m {advmin}'
        max = f'-M {advmax}'
        args = [min, max, 't = 3']
        hcicmd = 'le_set_advertising_param'
    try:
        result = le_cmd(hcicmd, args)
    except subprocess.CalledProcessError as e:
        print(f'beacon_on error (set_advertising_parameters):', e.stderr, e.stdout)
        return False
     
    # setup Advertising Data      
    adv_head = ['0xff', '0x4c', '0x00', '0x09', '0x08', '0x13', '0x30']
    adv_int = [int(hex_str, 16) for hex_str in adv_head]
    ip = list(map(int, ipv4_str.split('.')))
    prt = [port // 256, port % 256]
    adv_int = adv_int + ip + prt
    
    if linux:
        adv_len = len(adv_int)
        adv_int = [adv_len + 1, adv_len ] + adv_int
        args = [f'{i:#04x}' for i in adv_int] 
        args += ['0x00'] * (31 - len(adv_int)) 
        hcicmd = '0x0008'
    elif freebsd:
        adv = ','.join(f'{byte:02x}' for byte in adv_int)
        args = ['-b', adv]
        hcicmd = 'le_set_advertising_data'
    try:
        le_cmd(hcicmd, args)
    except subprocess.CalledProcessError as e:
        print(f'beacon_on error (set_advertisng_parameters):', e.stderr, e.stdout)
        return False
    advertised_port = port
    advertised_address = ipv4_str
    return True

def beacon_on() -> Optional[int]:
    global advertised_port
    global advertised_address
    if linux:
       hcicmd  = '0x000a'
       args = ['0x01']
    elif freebsd:
       hcicmd = 'le_set_advertising_enable'
       args = ['enable']
    try: 
        le_cmd(hcicmd, args)
    except subprocess.CalledProcessError as e:
        print(f'beacon_on error:', e.stderr, e.stdout)
        advertised_port = None
        advertised_address = None
        return None
    print(f'AirPlay Service-Discovery beacon transmission started {advertised_address}:{advertised_port}')
    return advertised_port

def beacon_off():
    global advertised_port
    global advertised_address    
    if linux:
        hcicmd = '0x000a'
        args = ['0x00']
    elif freebsd:
        hcicmd = 'le_set_advertising_enable'
        args = ['disable'] 
    le_cmd(hcicmd, args)
    print(f'AirPlay Service-Discovery beacon transmission ended')
    advertised_address = None
    advertised_port = None

LMP = ["1.0b","1.1", "1.2", "2.0+EDR", "2.1+EDR", "3.0+HS"]
LMP += ["4.0","4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"]
def get_bluetooth_version(device_name):
    if linux:
        cmd  ='hciconfig'
        args = [cmd, device_name, '-a']
        regexp = r"LMP Version: .*?\(0x([0-9a-fA-F])\)"
    elif freebsd:
        cmd = 'hccontrol'
        args = [cmd, '-n', device_name, 'Read_Local_Version_Information']
        regexp = r"LMP version: .*?\[(0x[0-9a-fA-F]+)\]"
    try:
        # Run hciconfig -a for the specific device
        result = subprocess.check_output(args, stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running {cmd} for {device_name}: {e.output}")
        return None
    except FileNotFoundError:
        print(f"Error: {cmd} command not found")
        return None
    # Regex to find "LMP Version: X.Y (0xZ)"
    lmp_version_match = re.search(regexp, result)
    if lmp_version_match:
        version_hex =  lmp_version_match.group(1)
        return int(version_hex,16)
    return None

def list_devices_by_version(min_version):
    if linux:
        cmd = 'hcitool'
        args = [cmd]
        args.append('dev')
        regexp = r"(hci\d+)"
    elif freebsd:
        cmd = 'hccontrol'
        args = [cmd]
        args.append('Read_Node_List')
        regexp = r"(^ubt\d+hci)"
    try:
        # Run hciconfig to list all devices
        devices_list_output = subprocess.check_output(args, stderr=subprocess.STDOUT, text=True)
        print(devices_list_output)
    except subprocess.CalledProcessError as e:
        print(f"Error running {cmd}: {e.output}")
        return None
    except FileNotFoundError:
        print(f"Error: {cmd} command not found")
        return None
    # Regex to find device names (e.g., hci0, hci1)
    device_names = re.findall(regexp, devices_list_output, re.MULTILINE)
    found_devices = []
    for device_name in device_names:
        version_decimal = get_bluetooth_version(device_name)
        if version_decimal is None or version_decimal < min_version:
            continue
        bt_version = LMP[version_decimal]
        device = [device_name, bt_version]         
        found_devices.append(device)
    return found_devices

from typing import Optional
def find_device(hci_in: Optional[str]) -> Optional[str]:
    global hci
    list = list_devices_by_version(min_version=6)
    if list is None or len(list) == 0:
        return None
    hci = None
    if hci_in is not None:
        for item in list:
            if item[0] == hci_in:
                hci = hci_in
                return hci
    count = 0
    for index, item in enumerate(list, start = 1):
        count += 1
        print(f'=== detected HCI device {count}. {item[0]}: Bluetooth v{item[1]}')
        if count == 1:
            hci = item[0]
    if count > 1:
        print(f'warning: {count} HCI devices were found, the first found will be used')
        print(f'(to override this choice, specify "--device=..." in optional arguments)')
    if linux:
        cmd = sudo + ['hciconfig', hci, 'reset']
    elif freebsd:
        cmd = sudo + ['hccontrol', '-n', hci, 'Reset']
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f'hci reset error:', e.stderr, e.stdout)
        print(help_text)
        print('cannot continue: SystemExit(1)')
        raise SystemExit(1)
    return hci

print('loaded uxplay_beacon_module_HCI')
