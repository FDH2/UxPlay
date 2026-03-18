# SPDX-License-Identifier: LGPL-2.1-or-later
# adapted from https://github.com/bluez/bluez/blob/master/test/example-advertisement
#----------------------------------------------------------------
# BleuIO (for BleuIO UB serial device)  module for a standalone python-3.6 or later AirPlay Service-Discovery Bluetooth LE beacon for UxPlay 
# (c)  F. Duncanh, March 2026


# **** This implementation requires a blueio dongle https://bleuio.com/bluetooth-low-energy-usb-ssd005.php
# This device has a self-contained bluetooth LE stack packaged as a usb serial modem.
# It is needed on macOS because macOS does not permit users to send manufacturer-specific BLE advertisements
# with its native BlueTooth stack.    It works also on linux and windows.

import time
import os

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

#check AdvInterval
def check_adv_intrvl(min, max):
    if not (100 <= min):
        raise ValueError('advmin was smaller than 100 msecs')
    if not (max >= min):
        raise ValueError('advmax  was smaller than advmin')
    if not (max <= 10240):
        raise ValueError('advmax was larger than 10240 msecs')

from typing import Literal
def setup_beacon(ipv4_str: str, port: int, advmin: int, advmax: int, index: Literal[None]) ->bool:
    if index is not None:
        raise ValuError('uxplay_beacon_module_BleuIO called with value of index: not None')
    check_adv_intrvl(advmin, advmax)
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
    global advertised_port
    global advertised_address
    global airplay_advertisement
    global advertisement_parameters   
    airplay_advertisement = ':'.join(format(b,'02x') for b in adv_data)
    advertisement_parameters = "0;" + str(advmin) + ";" + str(advmax) + ";0;"  # non-connectable mode, min ad internal, max ad interval, time = unlimited
    advertised_address = ipv4_str
    advertised_port = port
    return True

def beacon_on() ->bool:
    global advertised_port
    ser = None
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
    except serial.SerialException as e:
        print(f"beacon_on: Serial port error: {e}")
        raise SystemExit(1)
        advertised_port = None
    except Exception as e:
        print(f"beacon_on: An unexpected error occurred: {e}")
        advertised_port = None
    finally:
        if ser is not None:
            ser.close()
        return advertised_port
    
def beacon_off():
    global advertisement_parameters
    global airplay_advertisement
    global advertised_port
    global advertised_address
    ser = None
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
    except Exception as e:
        print(f"beacon_off: An unexpected error occurred: {e}")
    finally:
        if ser is not None:
            ser.close()

from typing import Optional
def find_device(serial_port_in: Optional[str]) ->Optional[str]:
    global serial_port
    serial_ports = list(list_ports.comports())
    count = 0
    serial_port_found = False
    serial_port = None
    TARGET_VID = '0x2DCF'   # used by BleuIO and BleuIO Pro
    target_vid = int(TARGET_VID,16)
    if serial_port_in is not None:
        for p in serial_ports:
            if getattr(p, 'vid', None) == target_vid or TARGET_VID in p.hwid:
                if p.device == serial_port_in:
                    serial_port = serial_port_in
                    break
    if serial_port is None:
        for p in serial_ports:
            if getattr(p, 'vid', None) == target_vid or TARGET_VID in p.hwid:
                count+=1
                if count == 1:
                    serial_port = p.device
                    print(f'=== detected BlueuIO {count}. port: {p.device} desc: {p.description} hwid: {p.hwid}')
                if count>1:
                    print(f'warning: {count} BleueIO devices were found, the first found will be used')
                    print(f'(to override this choice, specify "--device =..." in optional arguments)')
    if serial_port is None:
        return serial_port
    
    #test access to serial_port
    try:
        with serial.Serial(serial_port, 115200, timeout = 1) as ser:
            send_at_command(ser, "AT")
            ser.close()
    except Exception as e:
        print(f"beacon_on: Serial port error: {e}")
        text='''
  The user does not have sufficient privileges to access this serial port:
  On Linux, the user should be added to the "dialout" or "uucp" group
  On BSD systems, the necesary group is usually the "dialer" group.
  The correct group can be found using '''
        print(text, f'"ls -l {serial_port}"')
        raise SystemExit(1)
    return serial_port

print(f'Imported uxplay_beacon_module_BleuIO')
