#!/usr/bin/env python3
"""Provision over the physical USB console without storing credentials on host."""
import argparse
import getpass
import sys
from pathlib import Path
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--credentials-stdin', action='store_true', help='Read SSID, Wi-Fi password and optional camera password as stdin lines without prompting')
    camera_options = parser.add_mutually_exclusive_group()
    camera_options.add_argument('--camera-password', action='store_true', help='Also set the WebRTC camera password')
    camera_options.add_argument('--camera-password-file', type=Path, help='Read the camera password from a private local file')
    args = parser.parse_args()
    def stdin_line():
        line = sys.stdin.readline()
        if not line:
            parser.error('Missing credential line on stdin')
        return line.rstrip('\r\n')

    if args.credentials_stdin:
        ssid = stdin_line()
        password = stdin_line()
        camera_password = stdin_line() if args.camera_password else None
    else:
        ssid = input('Wi-Fi SSID: ')
        password = getpass.getpass('Wi-Fi password: ')
        camera_password = getpass.getpass('Camera password (12–96 bytes): ') if args.camera_password else None
    if not 1 <= len(ssid.encode()) <= 32 or any(c in ssid + password for c in '\r\n'):
        parser.error('Invalid SSID or newline in credentials')
    if args.camera_password_file is not None:
        try:
            camera_password = args.camera_password_file.read_text().rstrip('\r\n')
        except OSError:
            parser.error('Cannot read the camera password file')
    if camera_password is not None and (not 12 <= len(camera_password.encode()) <= 96 or any(c in camera_password for c in '\r\n')):
        parser.error('Invalid camera password')
    with serial.Serial(args.port, 115200, timeout=.2) as port:
        # Opening common adapters resets the board. Wait for setup to be ready.
        port.dtr = False
        port.rts = True
        time.sleep(.1)
        port.rts = False
        deadline = time.monotonic() + 180
        buffer = ''
        while time.monotonic() < deadline:
            buffer += port.read(4096).decode(errors='replace')
            if 'waiting for USB provisioning' in buffer:
                break
        else:
            raise RuntimeError('Board is not waiting for provisioning; use wifi-reset first if already provisioned')
        port.write(('wifi-provision\n' + ssid + '\n' + password + '\n' + (camera_password + '\n' if camera_password is not None else '')).encode())
        camera_password = None
        password = ''
        deadline = time.monotonic() + 180
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
