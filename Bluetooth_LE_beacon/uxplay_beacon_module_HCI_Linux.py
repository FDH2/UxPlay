import subprocess
import re

hci = None

LMP_version_map = ["1.0b","1.1", "1.2", "2.0+EDR", "2.1+EDR", "3.0+HS", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"]

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
    return hci
        
    
if __name__ == "__main__":
    # The minimum version is set to 6 (decimal) which corresponds to Bluetooth 4.0 (LMP version 6)

    device_address = 'hci4'
    use_device  = find_device(device_address)
    if use_device is None:
        print(f'No HCI devices were found')
        raise SystemExit(1)
    if device_address is not None and use_device != device_address:
        print(f'Error: A required device was NOT found at {device_address} given as an optional argument')
        print(f'(however required devices WERE found and are listed above')
        raise SystemExit(1)
    print(f'using the required device found at {use_device}')
 
    
