# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# HCI_Linux (uses sudo hciconfig):  module for a standalone python-3.6 or later AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 

# this requires that users can run "sudo hciconfig" with giving a password:
# (1) (as root) create a group like "hciusers"
# (2) use visudo to make an entry in /etc/sudoers:
#         %hciusers ALL=(ALL) NOPASSWD: /usr/bin/hcitool, /usr/bin/hciconfig
# (or or use visudo /etc/sudoers.d/hciusers to create a file /etc/sudoers.d/hciusers with this line in it)
# (3) add the user who will run uxplay-beacon.py to the group hciusers


import subprocess
import time
import re

hci = None

LMP_version_map = ["1.0b","1.1", "1.2", "2.0+EDR", "2.1+EDR", "3.0+HS", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"]



from typing import Literal
def setup_beacon(ipv4_str: str, port: int, advmin: int, advmax: int, index: Literal[None]) ->int:
    global hci
    adv = (advmin * 160) // 100
    value = adv % 256
    min1 = f'{value:#04x}'
    value = (adv // 256) % 256
    min2 = f'{value:#04x}'

    adv = (advmax * 160) // 100
    value = adv % 256
    max1 = f'{value:#04x}'
    value = (adv // 256) % 256
    max2 = f'{value:#04x}'

    cmd = f'sudo hcitool -i {hci} cmd 0x08 0x0006 {min1} {min2} {max1} {max2} 0x03 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x07 0x00'
    result = subprocess.run(cmd, shell=True, text=True, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    response = result.stdout
    if response[-3] +response[-4] != f'00':
        print(response)
        return 0
    ip = list(map(int, ipv4.split('.')))
    ip1 = f'{ip[0]:#04x}'
    ip2 = f'{ip[1]:#04x}'
    ip3 = f'{ip[2]:#04x}'
    ip4 = f'{ip[3]:#04x}'
    value = port // 256
    p1 = f"{value:#04x}"
    value = port % 256
    p2 = f"{value:#04x}"    
    cmd = f'sudo hcitool -i {hci} cmd 0x08 0x0008 0x0e 0x0d 0xff 0x4c 0x00 0x09 0x08 0x13 0x30 '
    cmd = cmd + f'{ip1} {ip2} {ip3} {ip4} {p1} {p2}'
    cmd = cmd + f' 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00'
    result = subprocess.run(cmd, shell=True, text=True, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    response = result.stdout
    if response[-3] +response[-4] != f'00':
        print(response)
        return 0
    return port

    
def beacon_on() ->bool:
    cmd = f'sudo hcitool -i hci0 cmd 0x08 0x000a 0x01'
    result = subprocess.run(cmd, shell=True, text=True, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
    response = result.stdout
    if response[-3] +response[-4] != f'00':
        print(response)
        return False
    else:
        return True

def beacon_off() ->int:
    cmd = f'sudo hcitool -i hci0 cmd 0x08 0x000a 0x00'
    return 0
    

def get_bluetooth_version(device_name):
    """
    Runs 'hciconfig -a <device_name>' and extracts the LMP version.
    """
    try:
        # Run hciconfig -a for the specific device
        result = subprocess.check_output(['hciconfig', device_name, '-a'], stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running hciconfig for {device_name}: {e.output}")
        return None
    except FileNotFoundError:
        print("Error: hciconfig command not found")
        return None
    # Regex to find "LMP Version: X.Y (0xZ)"
    lmp_version_match = re.search(r"LMP Version: .*?\(0x([0-9a-fA-F])\)", result)
    if lmp_version_match:
        version_hex =  lmp_version_match.group(1)
        return int(version_hex,16)
    return None

def list_devices_by_version(min_version): 
    try:
        # Run hciconfig to list all devices
        devices_list_output = subprocess.check_output(['hciconfig'], stderr=subprocess.STDOUT, text=True)
    except subprocess.CalledProcessError as e:
        print(f"Error running hciconfig: {e.output}")
        return None
    except FileNotFoundError:
        print("Error: hciconfig command not found")
        return None

    # Regex to find device names (e.g., hci0, hci1)
    device_names = re.findall(r"^(hci\d+):", devices_list_output, re.MULTILINE)
    
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
    cmd = f'sudo -n hciconfig  {hci} reset'
    result = subprocess.run(cmd, shell=True, text=True, stderr=subprocess.PIPE)
    if result.stderr != f'':
        print(f'stderr:[{result.stderr}]')
        text='''
        
   This HCI module requires NOPASSWD: privileges for sudo hciconfig, hctool')
     (1) create a group such as "hciusers"')
     (2) use visudo to set group sudo privileges:')
            %hciusers ALL=(ALL) NOPASSWD: /usr/bin/hciconfig, /usr/bin/hcitool')
     (3) add users of uxplay_beacon_module_HCI_Linux.py  to this group')

        '''
        print(text)
        print('cannot continue: SystemExit(1)')
        raise SystemExit(1)
    return hci
        
    
if __name__ == "__main__":
    # The minimum version is set to 6 (decimal) which corresponds to Bluetooth 4.0 (LMP version 6)

    device_address = None
    use_device  = find_device(device_address)
    if use_device is None:
        print(f'No HCI devices were found')
        raise SystemExit(1)
    if device_address is not None and use_device != device_address:
        print(f'Error: A required device was NOT found at {device_address} given as an optional argument')
        print(f'(however required devices WERE found and are listed above')
        raise SystemExit(1)
    print(f'using the required device found at {use_device}')
 
    ipv4 = '192.168.1.253'
    port = 7000
    advmin = 100
    advmax = 180
    
    ret = setup_beacon(ipv4, port, advmin, advmax, None)
    print(ret)
