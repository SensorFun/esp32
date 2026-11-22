"""Flash a board, provision it like a real user, and stream through the real
public signaling tunnel from a locally served copy of the website.

This is the full first-run path end to end: build + flash the firmware,
`wifi-reset` any previous provisioning, provision fresh through the browser
over USB, verify Wi-Fi recovery and camera password changes, confirm the
board reaches the default public signaling server,
then drive a real browser against a local static build of the site to
confirm it streams, that live setting changes reach the camera, and that
Save ("apply") persists on the camera while Reset to Default changes only live settings. No Wi-Fi
credentials are stored on disk; the random camera password lives only in
this process's memory.

Runs once per board listed in tests/config/e2e.yaml (see tests/e2e/conftest.py);
each board's USB port is auto-detected, since it can change between runs.
Assumes `tools/build.py`/`tools/flash.py` already have a working local
sdkconfig for that device (board selection and signaling API secret
configured via menuconfig, matching the built-in signaling service), per
tools/signal_server/README.md and projects/webrtc/README.md. Does not touch
tools/signal_server/.secrets or any other board's saved provisioning.
"""
from collections import deque
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import secrets
import subprocess
import sys
from threading import Event, Thread
import time

import serial
from playwright.sync_api import expect, sync_playwright

from conftest import Secret

ROOT = Path(__file__).resolve().parents[2]


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


def run(args, cwd=ROOT, *, stage, **kwargs):
    result = subprocess.run(args, cwd=cwd, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{stage} failed (exit {result.returncode}); see captured output and the full run report')


def wait_for(console, *needles, timeout, on_line=None):
    deadline = time.monotonic() + timeout
    buffer = ''
    while time.monotonic() < deadline:
        chunk = console.read(4096).decode(errors='replace')
        if chunk and on_line:
            for line in chunk.splitlines():
                on_line(line)
        buffer += chunk
        if all(needle in buffer for needle in needles):
            return buffer
        if 'Guru Meditation' in buffer or 'abort() was called' in buffer:
            raise RuntimeError('Firmware crashed while waiting for: ' + ', '.join(needles))
    raise TimeoutError('Timed out waiting for: ' + ', '.join(needles))


def reset_to_setup(port):
    # A board left over from a previous run is already connected to Wi-Fi;
    # wifi-reset clears its saved credentials and restarts into the setup AP,
    # exactly as documented for recovering an already-provisioned board.
    console = serial.Serial(port, 115200, timeout=.2)
    try:
        console.dtr = False
        console.rts = True
        time.sleep(.1)
        console.rts = False
        # Right after esptool's own post-flash reset, the app's console reader
        # task needs a moment to start; wait for its startup banner before
        # writing, or the command can land before anything is reading stdin.
        wait_for(console, 'Type wifi-reset', timeout=30)
        console.write(b'wifi-reset\n')
        wait_for(console, 'Setup Wi-Fi:', timeout=60)
    finally:
        console.close()


def provision_in_browser(url, browser_path, port, ssid, wifi_password, camera_password):
    # Bridge Chromium's Web Serial boundary to real hardware without interacting
    # with the native chooser. The production flasher and firmware exchange bytes.
    with serial.Serial(baudrate=115200, timeout=.15) as console, sync_playwright() as playwright:
        console.port = port
        console.dtr = console.rts = False
        browser = playwright.chromium.launch(executable_path=browser_path, args=['--no-sandbox'])
        page = browser.new_page(viewport={'width': 390, 'height': 900})

        def open_port():
            console.open()
            console.reset_input_buffer()

        page.expose_function('testSerialOpen', open_port)
        page.expose_function('testSerialRead', lambda: list(console.read(4096)) if console.is_open else [])
        page.expose_function('testSerialWrite', lambda data: console.write(bytes(data)))
        page.expose_function('testSerialClose', console.close)
        page.add_init_script("""
          const port = {
            getInfo: () => ({}),
            async open() {
              await testSerialOpen();
              let reading = true;
              this.readable = new ReadableStream({
                async pull(controller) {
                  while (reading) {
                    const bytes = await testSerialRead();
                    if (bytes.length && reading) {
                      controller.enqueue(Uint8Array.from(bytes));
                      break;
                    }
                  }
                },
                cancel() { reading = false; },
              });
              this.writable = new WritableStream({ write: bytes => testSerialWrite([...bytes]) });
            },
            close: () => testSerialClose(),
          };
          const api = new EventTarget();
          api.getPorts = async () => [];
          api.requestPort = async () => port;
          Object.defineProperty(navigator, 'serial', { value: api });
        """)
        page.goto(url + '#/tools/flasher')
        page.get_by_role('button', name='Select USB port').click()
        page.get_by_role('button', name='Provision', exact=True).click()
        popup = page.get_by_role('dialog', name='Provision WebRTC camera')
        setup = page.frame_locator('.flasher-provisioning iframe')
        status = setup.locator('#status')

        def open_setup():
            if not popup.is_visible():
                page.get_by_role('button', name='Provision', exact=True).click()
            expect(status).to_have_text(re.compile(r'Choose your network|No networks found'), timeout=90000)

        def connect(network, password, camera=None):
            # The firmware's own setup page; values are set without logging them.
            setup.locator('#ssid').select_option('manual')
            setup.locator('#manual-ssid').evaluate('(input, value) => { input.value = value; }', network)
            setup.locator('#password').evaluate('(input, value) => { input.value = value; }', password)
            assert setup.locator('#camera-access').is_visible() == (camera is not None)
            if camera is not None:
                setup.locator('#camera-password').evaluate('(input, value) => { input.value = value; }', camera)
            setup.get_by_role('button', name='Save and connect').click()

        def close_setup():
            popup.get_by_role('button', name='Close').click()
            expect(popup).to_be_hidden()

        open_setup()
        connect(ssid, wifi_password, camera_password)
        expect(status).to_have_text('Connected. Your settings are saved.', timeout=195000)
        expect(setup.locator('#local-viewer-link')).to_have_attribute('href', re.compile(r'^https://[\d.]+/$'))
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        close_setup()
        open_setup()  # The saved camera password is kept; the page no longer asks for it.
        connect(ssid, wifi_password)
        expect(status).to_have_text('Connected. Your settings are saved.', timeout=60000)
        close_setup()
        open_setup()
        connect('missing-' + secrets.token_hex(8), '')
        expect(status).to_contain_text('reconnected to its saved network', timeout=60000)
        close_setup()
        browser.close()


def discover(port, lines):
    console = serial.Serial(port, 115200, timeout=.2)
    console.dtr = False
    console.rts = True
    time.sleep(.1)
    console.rts = False
    try:
        buffer = wait_for(console, 'Remote camera ID:', 'Local camera ready:', 'Private signaling connected',
                          timeout=60, on_line=lines.append)
    except Exception:
        console.close()
        raise
    camera = re.search(r'Remote camera ID: ([A-F0-9]{12})', buffer)
    if not camera:
        console.close()
        raise RuntimeError('Camera ID not found after provisioning')
    local = re.search(r'Local camera ready: (https://[\d.]+/)', buffer)
    if not local:
        console.close()
        raise RuntimeError('Local viewer URL missing from startup diagnostics')
    return camera[1], local[1], console


def test_provision_and_stream(board, wifi_credentials, tmp_path):
    device, port = board['device'], board['port']
    wifi_ssid, wifi_password = wifi_credentials
    camera_password = Secret(secrets.token_urlsafe(24))  # Disposable; never written to disk.

    print('Building firmware…', flush=True)
    run([sys.executable, 'tests/boards_launcher.py', '--build', device], stage='Firmware build')
    print('Flashing board…', flush=True)
    run([sys.executable, 'tools/flash.py', 'project=webrtc', f'device={device}', f'port={port}'], stage='Firmware flash')

    print('Resetting any previous provisioning…', flush=True)
    reset_to_setup(port)
    http_server = None
    console = None
    diagnostics = deque(maxlen=50)
    stop = Event()
    reader = None
    try:
        site_output = tmp_path / 'site-dist'
        print('Building the website against the public signaling server…', flush=True)
        run(['npm', 'run', 'build', '--', '--outDir', str(site_output)],
            cwd=ROOT / 'site', stage='Website build')
        http_server = ThreadingHTTPServer(('127.0.0.1', 0), partial(QuietHandler, directory=str(site_output)))
        Thread(target=http_server.serve_forever, daemon=True).start()
        site_url = f'http://127.0.0.1:{http_server.server_port}/'
        print('Testing browser USB provisioning, Wi-Fi recovery and password changes…', flush=True)
        provision_in_browser(site_url, board['browser'], port, wifi_ssid, wifi_password, camera_password)
        print('Waiting for the camera to reach the public signaling server…', flush=True)
        camera_id, local_url, console = discover(port, diagnostics)
        print('Provisioned camera ID:', camera_id, flush=True)

        def drain():
            while not stop.is_set():
                try:
                    chunk = console.read(4096).decode(errors='replace')
                except (OSError, serial.SerialException):
                    return
                for line in chunk.splitlines():
                    diagnostics.append(line)
                    if 'Received settings Save (' in line or 'camera_transport:' in line:
                        print('Transport diagnostic:', line, flush=True)
        reader = Thread(target=drain, daemon=True)
        reader.start()

        print('Driving a real browser through the public signaling tunnel…', flush=True)
        run([sys.executable, str(ROOT / 'site/scripts/check-e2e.py'), '--browser', board['browser'],
             '--url', f'http://127.0.0.1:{http_server.server_port}/#/projects/webrtc',
             '--mac', camera_id, '--password-stdin'],
            stage='Remote WebRTC checks', input=camera_password + '\n', text=True)
        # Release USB before the persistence test resets the board and reads
        # fresh settings through a new viewer connection after each restart.
        stop.set()
        reader.join(timeout=2)
        assert not reader.is_alive(), 'USB diagnostic reader did not stop'
        console.close()
        print('Testing HTTPS assets and camera reconnects across restarts…', flush=True)
        run([sys.executable, 'tools/test_https_reconnect.py', '--port', port,
             '--browser', board['browser'], '--cycles', '3'], stage='Local HTTPS reconnect checks')
        print('Testing randomized settings across board restarts…', flush=True)
        run([sys.executable, 'tools/test_camera_settings.py', '--url', local_url,
             '--port', port, '--browser', board['browser'], '--rounds', '3'], stage='Randomized settings persistence checks')
        print('PASS: fresh flash, provisioning, streaming, live reset and saved settings across reboots', flush=True)
    except Exception:
        print('USB diagnostics:', flush=True)
        for line in diagnostics:
            for value in (wifi_ssid, wifi_password, camera_password):
                if value:
                    line = line.replace(value, '<redacted>')
            print(' ', line, flush=True)
        raise
    finally:
        stop.set()
        if reader:
            reader.join(timeout=1)
        if console:
            console.close()
        if http_server:
            http_server.shutdown()
            http_server.server_close()
