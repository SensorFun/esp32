#!/usr/bin/env python3
"""Provision over the physical USB console without storing credentials on host."""
import argparse
import getpass
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    ssid = input('Wi-Fi SSID: ')
    password = getpass.getpass('Wi-Fi password: ')
    if not 1 <= len(ssid.encode()) <= 32 or any(c in ssid + password for c in '\r\n'):
        parser.error('Invalid SSID or newline in credentials')
    with serial.Serial(args.port, 115200, timeout=.2) as port:
        # Opening common adapters resets the board. Wait for setup to be ready.
        port.dtr = False
        port.rts = True
        time.sleep(.1)
        port.rts = False
        deadline = time.monotonic() + 40
        buffer = ''
        while time.monotonic() < deadline:
            buffer += port.read(4096).decode(errors='replace')
            if 'Setup Wi-Fi:' in buffer:
                break
        else:
            raise RuntimeError('No setup AP announced; use wifi-reset first if already provisioned')
        port.write(('wifi-provision\n' + ssid + '\n' + password + '\n').encode())
        password = ''
        deadline = time.monotonic() + 40
        buffer = ''
        while time.monotonic() < deadline:
            buffer += port.read(4096).decode(errors='replace')
            if 'Local camera ready: https://' in buffer:
                print('Provisioned; ' + buffer[buffer.index('Local camera ready:'):].splitlines()[0])
                return
            if 'USB provisioning submitted:' in buffer and 'USB provisioning submitted: ESP_OK' not in buffer:
                raise RuntimeError('Board rejected provisioning')
        raise RuntimeError('Provisioning or camera startup failed; inspect the USB console')


if __name__ == '__main__':
    main()
