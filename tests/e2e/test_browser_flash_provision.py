"""Flash a board from the browser flasher, then set up its Wi-Fi in the USB
setup popup that opens afterwards, the way a first-time user would.

1. Build the board's firmware and serve it through the local flasher
   (`npm run dev -- --firmware-dir build`).
2. In Chromium, choose the board, select Clear settings and Flash firmware, and
   flash over Web Serial (esptool-js), so the board starts with no saved Wi-Fi
   or camera password.
3. The "Provision WebRTC camera" popup opens by itself and shows the firmware's
   own setup page. Choose the network, enter the Wi-Fi and camera passwords,
   and wait for "Connected".
4. Check the page links the remote viewer for this camera's ID, the
   reported local viewer answers over HTTPS, and that reopening the
   popup no longer asks for the camera password.
5. Try a network that does not exist and check the camera returns to the
   saved one.

Chromium's Web Serial port chooser cannot be automated, so a small bridge
gives the page a SerialPort-like object backed by pyserial on the real port.
It reports the adapter's real USB IDs (esptool-js picks its reset method from
them) and supports the signals, break and baud-rate changes esptool-js uses.
Wi-Fi credentials come from the hardware fixtures and are never written to
disk or logs; the random camera password lives only in this process.
"""
import re
import secrets
import ssl
import subprocess
import sys
from threading import Thread
import time
import urllib.request
from pathlib import Path

import serial
from serial.tools import list_ports
from playwright.sync_api import expect, sync_playwright

from conftest import Secret

ROOT = Path(__file__).resolve().parents[2]
ANSI = re.compile(r'\x1b\[[0-9;]*m')

# A SerialPort-like object for the page. `readable` follows Web Serial: after a
# cancel it hands out a fresh stream, and after the device is lost it is null
# until the port is opened again.
BRIDGE_SCRIPT = """
(() => {
  const info = INFO;
  let open = false, lost = false, readable = null, writable = null;
  function makeReadable() {
    let reading = true;
    const stream = new ReadableStream({
      async pull(controller) {
        while (reading) {
          const bytes = await bridgeRead();
          if (!reading) return;
          if (bytes === null) {
            lost = true; reading = false; readable = null;
            controller.error(new DOMException('The device has been lost.', 'NetworkError'));
            return;
          }
          if (bytes.length) { controller.enqueue(Uint8Array.from(bytes)); return; }
        }
      },
      cancel() { reading = false; if (readable === stream) readable = null; },
    });
    return stream;
  }
  const port = {
    getInfo: () => info,
    get readable() { if (!open || lost) return null; return readable ??= makeReadable(); },
    get writable() {
      if (!open || lost) return null;
      return writable ??= new WritableStream({ write: bytes => bridgeWrite([...bytes]) });
    },
    async open(options) {
      if (open) throw new DOMException('The port is already open.', 'InvalidStateError');
      await bridgeOpen(options.baudRate);
      open = true; lost = false; readable = null; writable = null;
    },
    async close() {
      open = false; readable = null; writable = null;
      await bridgeClose();
    },
    async setSignals(signals) { await bridgeSignals(signals); },
    async setBaudRate(baudRate) { await bridgeBaud(baudRate); },
    async forget() {},
  };
  const api = new EventTarget();
  api.getPorts = async () => [port];
  api.requestPort = async () => port;
  Object.defineProperty(navigator, 'serial', { value: api });
})();
"""


class SerialBridge:
    """pyserial behind the page's Web Serial calls."""

    def __init__(self, path):
        self.path = path
        self.console = serial.Serial()
        self.console.port = path
        self.console.timeout = .03
        self.transcript = bytearray()  # Recent board output, shown on failure.

    def info(self):
        for item in list_ports.comports():
            if item.device == self.path and item.vid is not None:
                return {'usbVendorId': item.vid, 'usbProductId': item.pid}
        return {}

    def open(self, baud):
        # Boards with built-in USB (ESP32-S3) re-enumerate after a reset; wait
        # for the port to come back, as Chromium does before it reconnects.
        self.console.baudrate = baud
        deadline = time.monotonic() + 15
        while True:
            try:
                self.console.open()
                return
            except serial.SerialException:
                if time.monotonic() > deadline:
                    raise
                time.sleep(.2)

    def close(self):
        self.console.close()

    def read(self):
        try:
            if not self.console.is_open:
                return None
            data = self.console.read(self.console.in_waiting or 1)
            if data and self.console.in_waiting:
                data += self.console.read(self.console.in_waiting)
            self.transcript = (self.transcript + data)[-16384:]
            return list(data)
        except (OSError, serial.SerialException):
            self.console.close()
            return None

    def write(self, data):
        self.console.write(bytes(data))

    def report(self, secrets_to_hide):
        text = self.transcript.decode(errors='replace')
        for value in secrets_to_hide:
            if value:
                text = text.replace(value, '<redacted>')
        # Flash data is binary; keep printable lines only.
        lines = [line for line in text.splitlines() if line.strip() and line.isprintable()]
        return '\n'.join(lines[-60:])

    def signals(self, values):
        if values.get('dataTerminalReady') is not None:
            self.console.dtr = values['dataTerminalReady']
        if values.get('requestToSend') is not None:
            self.console.rts = values['requestToSend']
        if values.get('break') is not None:
            self.console.break_condition = values['break']

    def baud(self, value):
        self.console.baudrate = value

    def attach(self, page):
        page.expose_function('bridgeOpen', self.open)
        page.expose_function('bridgeClose', self.close)
        page.expose_function('bridgeRead', self.read)
        page.expose_function('bridgeWrite', self.write)
        page.expose_function('bridgeSignals', self.signals)
        page.expose_function('bridgeBaud', self.baud)
        page.add_init_script(BRIDGE_SCRIPT.replace('INFO', repr(self.info()).replace("'", '"')))


def start_flasher_site():
    """Starts the loopback dev server with the workspace's local builds."""
    server = subprocess.Popen(['node', 'scripts/launch.mjs', 'dev', '--firmware-dir', str(ROOT / 'build')],
                              cwd=ROOT / 'site', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 60
    output = []
    while time.monotonic() < deadline:
        line = server.stdout.readline()
        if not line:
            break
        output.append(line)
        found = re.search(r'Local:\s+(http://127\.0\.0\.1:\d+/)', ANSI.sub('', line))
        if found:
            # Keep reading so the server never blocks on a full pipe.
            Thread(target=lambda: [None for _ in server.stdout], daemon=True).start()
            return server, found[1]
    server.kill()
    raise RuntimeError('Local flasher site did not start:\n' + ''.join(output[-20:]))


def viewer_answers(url):
    # The camera's certificate is self-signed; this only checks it is serving.
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(url, context=context, timeout=5) as response:
                if response.status == 200:
                    return
        except OSError:
            time.sleep(2)
    raise RuntimeError(f'Local viewer {url} did not answer')


def test_browser_flash_then_usb_provision(board, wifi_credentials):
    device, port, board_id = board['device'], board['port'], board['board']
    ssid, wifi_password = wifi_credentials
    camera_password = Secret(secrets.token_urlsafe(24))  # Disposable; never written to disk.

    print('Building firmware…', flush=True)
    if subprocess.call([sys.executable, 'tests/boards_launcher.py', '--build', device], cwd=ROOT):
        raise RuntimeError('Firmware build failed')
    server, site = start_flasher_site()
    bridge = SerialBridge(port)
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(executable_path=board['browser'], args=['--no-sandbox'])
            page = browser.new_page(viewport={'width': 390, 'height': 900})
            bridge.attach(page)
            page.goto(site + 'tools/flasher?project=webrtc')

            print('Flashing from the browser (settings cleared)…', flush=True)
            radio = page.locator(f'input[name=board][value="{board_id}"]')
            expect(radio).to_be_enabled(timeout=30000)
            radio.check()
            expect(page.get_by_test_id('firmware-summary')).to_contain_text('Local build')
            page.get_by_role('tab', name='USB Port', exact=True).click()
            expect(page.get_by_test_id('selected-port')).to_be_visible()
            page.get_by_role('tab', name='Flash', exact=True).click()
            page.get_by_label('Speed', exact=True).select_option('460800')
            page.get_by_role('checkbox', name=re.compile('^Clear settings')).check()
            page.once('dialog', lambda dialog: dialog.accept())
            page.get_by_role('button', name='Clear settings and flash', exact=True).click()
            expect(page.get_by_test_id('flash-status')).to_have_text(
                'Settings cleared. Firmware written and verified. Board restarted.', timeout=300000)

            print('Setting up Wi-Fi in the USB popup…', flush=True)
            popup = page.get_by_role('dialog', name='Provision WebRTC camera')
            expect(popup).to_be_visible()  # Opens by itself after a WebRTC flash.
            setup = page.frame_locator('.flasher-provisioning iframe')
            status = setup.locator('#status')

            def wait_for_page():
                # Shows the error instead of a timeout if the board never answers.
                error = popup.locator('.flasher-error')
                ready = setup.locator('#status').filter(has_text=re.compile('Choose your network|No networks found'))
                deadline = time.monotonic() + 90
                while not ready.count():
                    assert not error.is_visible(), error.inner_text()
                    assert time.monotonic() < deadline, 'The setup page did not finish scanning'
                    page.wait_for_timeout(250)  # Keeps serving the serial bridge.

            def connect(network, password, camera=None):
                # Values are set without echoing them into Playwright logs.
                setup.locator('#ssid').select_option('manual')
                setup.locator('#manual-ssid').evaluate('(input, value) => { input.value = value; }', network)
                setup.locator('#password').evaluate('(input, value) => { input.value = value; }', password)
                assert setup.locator('#camera-access').is_visible() == (camera is not None)
                if camera is not None:
                    setup.locator('#camera-password').evaluate('(input, value) => { input.value = value; }', camera)
                setup.get_by_role('button', name='Save and connect').click()

            def reopen():
                popup.get_by_role('button', name='Close').click()
                expect(popup).to_be_hidden()
                page.get_by_role('button', name='Provision', exact=True).click()
                wait_for_page()

            wait_for_page()
            assert setup.locator('#camera-access').is_visible(), 'A fresh board must ask for a camera password'
            connect(ssid, wifi_password, camera_password)
            expect(status).to_have_text('Connected. Your settings are saved.', timeout=120000)
            link = setup.locator('#local-viewer-link')
            expect(link).to_have_attribute('href', re.compile(r'^https://[\d.]+/$'))
            viewer = link.get_attribute('href')
            remote = setup.locator('#remote-viewer-link')
            expect(remote).to_have_attribute('href', re.compile(r'^https://esp32\.sensorfun\.net/projects/webrtc/[0-9A-F]{12}$'))
            expect(page.get_by_role('link', name=remote.get_attribute('href'))).to_be_visible()
            expect(page.get_by_role('link', name=viewer)).to_be_visible()
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
            print('Camera joined Wi-Fi; checking the local viewer…', flush=True)
            viewer_answers(viewer)

            print('Checking the saved camera password and Wi-Fi recovery…', flush=True)
            reopen()
            assert not setup.locator('#camera-access').is_visible(), 'The saved camera password must be kept'
            connect('missing-' + secrets.token_hex(8), '')
            expect(status).to_contain_text('reconnected to its saved network', timeout=90000)
            popup.get_by_role('button', name='Close').click()
            expect(popup).to_be_hidden()
            viewer_answers(viewer)
            browser.close()
        print(f'PASS: {device} flashed in the browser and set up over USB', flush=True)
    except Exception:
        print('Recent board output:\n' + bridge.report([ssid, wifi_password, camera_password]), flush=True)
        raise
    finally:
        bridge.close()
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
