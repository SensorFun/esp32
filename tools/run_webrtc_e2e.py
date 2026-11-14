#!/usr/bin/env python3
"""Discover board camera IDs over USB and launch the static site + signaling bridge.

No Wi-Fi credentials are needed here; provision the boards first.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess
import time
import serial

ROOT = Path(__file__).resolve().parents[1]


def discover(port):
    console = serial.Serial(port, 115200, timeout=.2)
    console.dtr = False; console.rts = True; time.sleep(.1); console.rts = False
    deadline = time.monotonic() + 60
    data = ''
    while time.monotonic() < deadline:
        data += console.read(4096).decode(errors='replace')
        room = re.search(r'Remote camera ID: ([A-F0-9]{12})', data)
        if room and 'Public signaling connected' in data:
            # Keep USB open: closing some adapters resets the board.
            return {'id': room[1], 'name': 'ESP32-S3 camera' if ('esp32s3' in data.lower() or 'esp32-s3' in data.lower()) else 'ESP32-CAM'}, console
        if 'abort() was called' in data:
            errors = [line for line in data.splitlines() if any(tag in line for tag in ('E (', 'abort()', 'assert failed'))]
            console.close(); raise RuntimeError(f'{port}: firmware aborted: ' + '\n'.join(errors))
    errors = [line for line in data.splitlines() if any(tag in line for tag in ('E (', 'Signaling unavailable', 'APPRTC_SIG'))]
    console.close(); raise RuntimeError(f'{port}: public signaling not ready: ' + '\n'.join(errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', nargs='+', required=True)
    args = parser.parse_args()
    processes, consoles = [], []
    try:
        subprocess.run(['npm', 'run', 'build'], cwd=ROOT / 'site', check=True)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            devices = list(pool.map(discover, args.ports))
        cameras = [device[0] for device in devices]
        consoles = [device[1] for device in devices]
        config = ROOT / 'build/e2e/cameras.json'
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(json.dumps(cameras))
        processes.append(subprocess.Popen(['node', 'scripts/signaling-bridge.mjs', '--state', str(ROOT / 'build/e2e/signaling-memberships.json'), '--config', str(config), '--port', '8787', '--origin', 'http://localhost:5173'], cwd=ROOT / 'site'))
        processes.append(subprocess.Popen(['npm', 'run', 'preview', '--', '--host', 'localhost', '--port', '5173', '--strictPort'], cwd=ROOT / 'site', start_new_session=True))
        print('Open http://localhost:5173/#/spaces/webrtc and enter a MAC + camera password (Load cameras is optional).', flush=True)
        print('STUN-only mode: TURN relay is disabled. Same-LAN tests do not prove connectivity through every NAT.', flush=True)
        while all(p.poll() is None for p in processes):
            # Drain serial output without logging credentials/session details.
            for console in consoles:
                chunk = console.read(4096).decode(errors='replace')
                for line in chunk.splitlines():
                    if any(label in line for label in ('Peer state', 'DTLS:', 'Public signaling', 'remote:', 'E (')):
                        print('USB:', line, flush=True)
            time.sleep(.1)
    except KeyboardInterrupt:
        pass
    finally:
        import os, signal
        for process in reversed(processes):
            if process.poll() is None:
                if process.args[0] == 'npm': os.killpg(process.pid, signal.SIGTERM)
                else:
                    process.terminate()
                    process.wait(timeout=75)
        for console in consoles: console.close()


if __name__ == '__main__':
    main()
