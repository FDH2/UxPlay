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

os_name = platform.system()
if os_name == 'Darwin':
   os_name = 'macOS' 
linux =  os_name == 'Linux'
bsd = 'BSD' in os_name
if not linux and not bsd:
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
elif bsd:
    help_text2 =  '''   
     (2) use visudo to create a file /usr/local/etc/sudoers.d/hciusers containing the line: 
          %hciusers ALL=(ALL) NOPASSWD: /usr/sbin/hccontrol
    '''
help_text3 = '''
     (3) add users of uxplay_beacon_module_HCI.py to the group "hciusers"
'''
help_text = help_text1 + help_text2 + help_text3

hci = None
LMP_version_map = ["1.0b","1.1", "1.2", "2.0+EDR", "2.1+EDR", "3.0+HS", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"]


advertised_port = None
advertised_address = None



def setup_beacon(ipv4_str: str, port: int, advmin: int, advmax: int, index: Literal[None]) -> bool:
    global hci
    global advertised_port
    global advertised_address
    advertised_port = None
    advertised_address = None

    # convert into  units of 5/8 msec.
    advmin = (advmin * 8) // 5
    advmax = (advmax * 8) // 5

    # setup Advertising Parameters
    if linux:
        min1 = f'{advmin %256 :#04x}'
        min2 = f'{advmin //256 :#04x}'
        max1 = f'{advmax % 256 :#04x}'
        max2 = f'{advmax // 256 :#04x}'
        ogf = "0x08"
        ocf = "0x0006"
        cmd = ["sudo", '-n',  "hcitool", "-i", hci, "cmd", ogf, ocf,  min1, min2, max1, max2, '0x03', '0x00', '0x00'] + ['0x00'] * 6 + ['0x07', '0x00']
    elif bsd:
        min = f'{advmin :04x}'
        max = f'{advmax :04x}'
        cmd = ["sudo", "-n", "hccontrol", "-n", hci, "le_set_advertising_param", min, max, '03', '00', '00', '000000000000', '07','00']
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print("Error:", e.stderr, e.stdout)
        return False
   
    # setup Advertising Data      
    adv_head = ['0xff', '0x4c', '0x00', '0x09', '0x08', '0x13', '0x30']
    adv_int = [int(hex_str, 16) for hex_str in adv_head]
    ip = list(map(int, ipv4_str.split('.')))
    prt = [port // 256, port % 256]
    adv_int = adv_int + ip + prt
    adv_len = len(adv_int)
    adv_int = [adv_len + 1, adv_len ] + adv_int
    if linux:
        ogf = '0x08'
        ocf = '0x0008'
        cmd = ['sudo',  '-n', 'hcitool', '-i', hci, 'cmd', ogf, ocf]
        cmd = cmd + [f'{i:#04x}' for i in adv_int] 
        cmd = cmd + ['0x00'] * 17
    elif bsd:
        cmd = ['sudo', '-n', 'hccontrol', '-n', hci, 'le_set_advertising_data']
        cmd = cmd +  [f'{i:02x}' for i in adv_int]
        
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print("Error:", e.stderr, e.stdout)
        return False
         
    advertised_port = port
    advertised_address = ipv4_str
    return True

def beacon_on() -> Optional[int]:
    global advertised_port
    global advertised_address

    if linux:
       ogf = '0x08'
       ocf = '0x000a'
       cmd = ['sudo', '-n', 'hcitool', '-i', hci, 'cmd', ogf, ocf, '0x01']
    elif bsd:
       cmd = ['sudo', '-n', 'hccontrol', '-n', hci, 'le_set_advertising_enable', 'enable']
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f'Started Bluetooth LE Service Discovery beacon {advertised_address}:{advertised_port}')
    except subprocess.CalledProcessError as e:
        print(f'beacon_on error:', e.stderr, e.stdout)
        advertised_port = None
        advertised_address = None
    finally:
       return advertised_port

def beacon_off():
    if linux:
       ogf = '0x08'
       ocf = '0x000a'
       cmd = ['sudo', '-n', 'hcitool', '-i', hci, 'cmd', ogf, ocf, '0x00']
    elif bsd:
       cmd = ['sudo', '-n', 'hccontrol', '-n', hci, 'le_set_advertising_enable', 'disable']
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        print(f'Stopped Bluetooth LE Service Discovery beacon')
    except subprocess.CalledProcessError as e:
        print("Error (beacon_off):", e.stderr, e.stdout)
        advertised_address = None
        advertised_port = None
        
def get_bluetooth_version(device_name):
    """
    Runs 'hciconfig -a <device_name>' and extracts the LMP version.
    """
    if linux:
        cmd = f'hciconfig'
        opt1 = f''
        opt2 = f'-a'
        regexp = r"LMP Version: .*?\(0x([0-9a-fA-F])\)"
    elif bsd:
        cmd = f'hccontrol'
        opt1 = f'-n'
        opt2 = f'Read_Local_Version_Information'
        regexp = r"LMP version: .*?\[(0x[0-9a-fA-F]+)\]"
    try:
        # Run hciconfig -a for the specific device
        result = subprocess.check_output([cmd, opt1, device_name, opt2], stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running {cmd} for {device_name}: {e.output}")
        return None
    except FileNotFoundError:
        print("Error: {cmd} command not found")
        return None
    # Regex to find "LMP Version: X.Y (0xZ)"
    lmp_version_match = re.search(regexp, result)
    if lmp_version_match:
        version_hex =  lmp_version_match.group(1)
        return int(version_hex,16)
    return None

def list_devices_by_version(min_version):
    if linux:
        cmd = f'hcitool'
        opt = f'dev'
        regexp = r"(hci\d+)"
    elif bsd:
        cmd = f'hccontrol'
        opt = f'Read_Node_List'
        regexp = r"(^ubt\d+hci)"
        
    try:
        # Run hciconfig to list all devices
        devices_list_output = subprocess.check_output([cmd, opt], stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running hciconfig: {e.output}")
        return None
    except FileNotFoundError:
        print("Error: hciconfig command not found")
        return None
    # Regex to find device names (e.g., hci0, hci1)
    device_names = re.findall(regexp, devices_list_output, re.MULTILINE)
    found_devices = []
    for device_name in device_names:
        version_decimal = get_bluetooth_version(device_name)
        if version_decimal is None or version_decimal < min_version:
            continue
        bt_version = LMP_version_map[version_decimal]
        device = [device_name, bt_version]         
        found_devices.append(device)
    return found_devices

from typing import Optional
def find_device(hci_in: Optional[str]) -> Optional[str]:
    global hci
    list = list_devices_by_version(min_version=6)
    if len(list) == 0:
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
        cmd = ['sudo', '-n', 'hciconfig', hci, 'reset']
    elif bsd:
        cmd = ['sudo', '-n', 'hccontrol', '-n', hci, 'Reset']
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f'hci reset error:', e.stderr, e.stdout)
        print(help_text)
        print('cannot continue: SystemExit(1)')
        raise SystemExit(1)
    return hci

