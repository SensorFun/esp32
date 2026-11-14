#!/usr/bin/env python3
"""Test provisioned USB cameras through the real public Janus tunnel.

Uses the private test passwords prepared in signal_server/.secrets/hardware.
Does not change Wi-Fi settings, camera passwords, or flash firmware.
"""
import argparse
from collections import deque
import concurrent.futures
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import re
import subprocess
import sys
from threading import Event, Thread
import time
from urllib.parse import urlsplit

import serial

ROOT = Path(__file__).resolve().parents[1]


def discover(port):
    console = serial.Serial(port, 115200, timeout=.2)
    try:
        console.dtr = False
        console.rts = True
        time.sleep(.1)
        console.rts = False
        data = ''
        deadline = time.monotonic() + 65
        while time.monotonic() < deadline:
            data += console.read(4096).decode(errors='replace')
            if 'Setup Wi-Fi:' in data:
                raise RuntimeError(f'{port}: Wi-Fi provisioning is required')
            if 'Guru Meditation' in data or 'abort() was called' in data:
                raise RuntimeError(f'{port}: firmware crashed')
            camera = re.search(r'Remote camera ID: ([A-F0-9]{12})', data)
            if camera and 'Local camera ready:' in data and 'Private signaling connected' in data:
                chip = 'esp32s3' if 'esp32s3' in data.lower() or 'esp32-s3' in data.lower() else 'esp32'
                password = (ROOT / 'tools/signal_server/.secrets/hardware' / f'{chip}-camera-password').read_text().strip()
                return port, camera[1], password, console
        raise RuntimeError(f'{port}: camera or private signaling did not become ready')
    except BaseException:
        console.close()
        raise


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', nargs='+', required=True)
    parser.add_argument('--browser', type=Path, required=True)
    parser.add_argument('--server', required=True, help='Public signaling hostname without a path')
    args = parser.parse_args()
    url = urlsplit('wss://' + args.server)
    if not url.hostname or url.path or url.query or url.fragment or url.username or url.password:
        parser.error('--server must be a hostname, optionally with a port')
    environment = dict(os.environ, SIGNALING_URL='wss://' + args.server + '/viewer')
    subprocess.run(['npm', 'run', 'build'], cwd=ROOT / 'site', env=environment, check=True)
    devices = []
    server = None
    stop = Event()
    reader = None
    diagnostics = {}
    capture_recoveries = {}
    try:
        with concurrent.futures.ThreadPoolExecutor() as pool:
            futures = [pool.submit(discover, port) for port in args.ports]
            failures = []
            for future in futures:
                try:
                    devices.append(future.result())
                except Exception as error:
                    failures.append(str(error))
            if failures:
                raise RuntimeError('; '.join(failures))
        server = ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(ROOT / 'site/dist')))
        Thread(target=server.serve_forever, daemon=True).start()

        def drain():
            while not stop.is_set():
                for port, _, _, console in devices:
                    lines = console.read(4096).decode(errors='replace').splitlines()
                    for line in lines:
                        if 'Restarting stalled camera capture' in line:
                            capture_recoveries[port] = capture_recoveries.get(port, 0) + 1
                        if any(label in line for label in ('Peer state', 'Received signal type', 'Sent signal type', 'Signaling unavailable', 'Remote peer failed', 'cam_hal:', 'camera:', 'Restarting stalled camera capture')):
                            diagnostics.setdefault(port, deque(maxlen=25)).append(line)

        reader = Thread(target=drain)
        reader.start()
        for port, camera, password, _ in devices:
            command = [sys.executable, str(ROOT / 'site/scripts/check-e2e.py'), '--browser', str(args.browser),
                       '--url', f'http://127.0.0.1:{server.server_port}/#/spaces/webrtc', '--mac', camera, '--password-stdin']
            for flags, value in [(['--reject'], password + '-wrong'), (['--unauthenticated'], password), (['--replay'], password), ([], password)]:
                subprocess.run(command + flags, input=value + '\n', text=True, check=True)
                time.sleep(3)
            print(f'PASS: {port}: password rejection, unauthorized access, replay rejection, live JPEG, controls, and reconnect', flush=True)
            print(f'Capture recoveries on {port}: {capture_recoveries.get(port, 0)}', flush=True)
    except Exception:
        for port, lines in list(diagnostics.items()):
            print(f'Device diagnostics for {port}:', flush=True)
            for line in list(lines):
                print(line, flush=True)
        raise
    finally:
        stop.set()
        if reader:
            reader.join()
        if server:
            server.shutdown()
            server.server_close()
        for _, _, _, console in devices:
            console.close()


if __name__ == '__main__':
    main()
