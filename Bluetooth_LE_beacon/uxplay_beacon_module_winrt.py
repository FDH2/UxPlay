# SPDX-License-Identifier: LGPL-2.1-or-later
#---------------------------------------------------------------
# winrt (Windows) module for a standalone python-3.6 AirPlay Service-Discovery Bluetooth LE beacon for UxPlay
# (c)  F. Duncanh, March 2026 

# Import WinRT APIs (see https://pypi.org/project/winrt-Windows.Foundation.Collections/)
try:
    import winrt.windows.foundation.collections
except ImportError:
    print(f"ImportError from winrt-Windows.Foundation.Collections")
    print(f"Install with 'pip install winrt-Windows.Foundation'")
    print(f"and with 'pip install winrt-Windows.Foundation.Collections'")
    print(f'You may need to use pip option "--break-system-packages" (disregard the warning)')
    raise SystemExit(1)

try:
    import winrt.windows.devices.bluetooth.advertisement as ble_adv
except ImportError:
    print(f"ImportError from winrt-Windows.Devices.Bluetooth.Advertisement")
    print(f"Install with 'pip install winrt-Windows.Devices.Bluetooth.Advertisement'")
    print(f'You may need to use pip option "--break-system-packages" (disregard the warning)')
    raise SystemExit(1)

try:
    import winrt.windows.storage.streams as streams
except ImportError:
    print(f"ImportError from winrt-Windows.Storage.Streams")
    print(f"Install with 'pip install winrt-Windows.Storage.Streams'")
    print(f'You may need to use pip option "--break-system-packages" (disregard the warning)')
    raise SystemExit(1)

import os

publisher = None
advertised_port = None
advertised_address = None
quiet = False

def on_status_changed(sender, args):
    global publisher
    global quiet
    if not quiet:
        print(f"Publisher status change to: {args.status.name}")
    if args.status.name == "ABORTED":
        print(f'Publisher was aborted after starting: perhaps no Bluetooth interface is available?')
        print(f'Stopping')
        os._exit(1)
    if args.status.name == "STOPPED":
        publisher = None

def create_airplay_service_discovery_advertisement_publisher(ipv4_str, port):
    assert port > 0
    assert port <= 65535
    mfg_data = bytearray([0x09, 0x08, 0x13, 0x30]) # Apple Data Unit type 9 (Airplay), length 8, flags 0001 0011, seed 30
    import ipaddress
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
        if not quiet:
            print(f"AirPlay Service_Discovery Advertisement ({advertised_address}:{advertised_port}) registered")
    except Exception as e:
        print(f"Failed to start Publisher: {e}")
        print(f"Publisher Status: {publisher.status.name}")
        advertised_address = None
        advertised_port = None

from typing import Literal    
def setup_beacon(ipv4_str: str, port:int , advmin: Literal[None], advmax :Literal[None], index :Literal[None]) ->bool:
    global quiet
    quiet = False
    if port == 1:
        #fake port used for testing
        print(f'beacon test')
        quiet = True
    if (advmin is not None) or (advmax is not None) or (index is not None):
        raise ValueError('uxplay_beacon_module_winrt: advmin, advmax, index were not all None')
    create_airplay_service_discovery_advertisement_publisher(ipv4_str, port)
    return True

from typing import Optional
def beacon_on() -> Optional[int]:
    import asyncio
    try:
        asyncio.run(publish_advertisement())
    except Exception as e:
        print(f"Failed to start publisher: {e}")
        global publisher
        publisher = None
    #advertised_port is set to None if publish_advertisement failed
    global advertised_port
    return advertised_port
    
def beacon_off():
    publisher.stop()
    global advertised_port
    global advertised_address
    advertised_port = None
    advertised_address = None

print(f'loaded uxplay_beacon_module_winrt')
