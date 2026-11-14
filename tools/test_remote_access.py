#!/usr/bin/env python3
"""Test remote camera authorization with temporary in-memory passwords, then clear them.

Run with serial monitors closed. This replaces existing camera passwords: use
only on test boards. Saved Wi-Fi credentials are preserved.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import time
import threading
from camera_password import update
from run_webrtc_e2e import discover

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ports', nargs='+', required=True)
    parser.add_argument('--browser', required=True)
    args = parser.parse_args()
    devices, processes, touched = [], [], []
    try:
        with concurrent.futures.ThreadPoolExecutor() as pool:
            devices = list(pool.map(discover, args.ports))
        config = ROOT / 'build/e2e/cameras.json'
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text(json.dumps([camera for camera, _ in devices]))
        processes.append(subprocess.Popen(['node', 'scripts/signaling-bridge.mjs', '--state', str(ROOT / 'build/e2e/signaling-memberships.json'), '--config', str(config), '--port', '8787', '--origin', 'http://localhost:5173'], cwd=ROOT / 'site'))
        processes.append(subprocess.Popen(['npm', 'run', 'preview', '--', '--host', 'localhost', '--port', '5173', '--strictPort'], cwd=ROOT / 'site', start_new_session=True))
        time.sleep(2)
        for camera, console in devices:
            password = secrets.token_urlsafe(24)
            touched.append(console)
            start = time.monotonic()
            update(console, password)
            print(f"Password setup completed ({camera['name']}): {time.monotonic() - start:.1f}s", flush=True)
            command = [sys.executable, str(ROOT / 'site/scripts/check-e2e.py'), '--browser', args.browser, '--url', 'http://localhost:5173/#/spaces/webrtc', '--mac', camera['id'].removeprefix('camera-'), '--password-stdin']
            stop_drain = threading.Event()
            def drain_console():
                while not stop_drain.is_set():
                    for _, device_console in devices:
                        chunk = device_console.read(4096).decode(errors='replace')
                        for line in chunk.splitlines():
                            if any(term in line for term in ('remote:', 'Peer state', 'E (', 'APPRTC_SIG')):
                                print('USB:', line, flush=True)
            reader = threading.Thread(target=drain_console)
            reader.start()
            try:
                for flags, supplied in [(['--reject'], secrets.token_urlsafe(24)), (['--unauthenticated'], password), (['--replay'], password), ([], password)]:
                    time.sleep(3)
                    subprocess.run(command + flags, input=supplied + '\n', text=True, check=True)
            finally:
                stop_drain.set()
                reader.join()
            update(console, None)
            touched.remove(console)
            password = None
        print('PASS: both boards tested; temporary camera passwords cleared; Wi-Fi preserved', flush=True)
    finally:
        for console in touched:
            update(console, None)
        for process in reversed(processes):
            if process.poll() is None:
                if process.args[0] == 'npm': os.killpg(process.pid, signal.SIGTERM)
                else:
                    process.terminate()
                    process.wait(timeout=75)
        for _, console in devices:
            console.close()


if __name__ == '__main__':
    main()
