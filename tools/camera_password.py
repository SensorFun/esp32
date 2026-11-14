#!/usr/bin/env python3
"""Set or clear the remote camera password over the physical USB console."""
import argparse
import getpass
import time
import serial


def update(console, password):
    if password is not None and (not 12 <= len(password.encode()) <= 96 or any(c in password for c in '\r\n')):
        raise ValueError('Camera password must be 12–96 UTF-8 bytes with no newlines')
    console.write(b'camera-password-clear\n' if password is None else ('camera-password\n' + password + '\n').encode())
    data = ''
    deadline = time.monotonic() + 180
    label = 'Camera password cleared:' if password is None else 'Camera password saved:'
    while time.monotonic() < deadline:
        data += console.read(4096).decode(errors='replace')
        if label in data:
            if label + ' ESP_OK' not in data:
                raise RuntimeError('Camera rejected the password update')
            return
        if 'abort() was called' in data or 'Guru Meditation' in data:
            raise RuntimeError('Board crashed during password update')
    raise TimeoutError('No password update acknowledgement from board')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--clear', action='store_true', help='Disable remote access until another password is set')
    args = parser.parse_args()
    password = None if args.clear else getpass.getpass('Camera password (12–96 bytes): ')
    if password is not None and password != getpass.getpass('Confirm camera password: '):
        parser.error('Passwords do not match')
    with serial.Serial(args.port, 115200, timeout=.2) as console:
        console.dtr = False
        console.rts = True
        time.sleep(.1)
        console.rts = False
        data = ''
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            data += console.read(4096).decode(errors='replace')
            if 'Local camera ready:' in data or 'Setup Wi-Fi:' in data:
                break
        else:
            raise TimeoutError('Board console not ready')
        update(console, password)
    print('Remote access disabled.' if password is None else 'Camera password updated. Wi-Fi settings preserved.')


if __name__ == '__main__':
    main()
