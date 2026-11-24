#!/usr/bin/env python3
"""Live hardware check: USB identity, HTTPS access rules, JPEG, controls, DTLS.

Requires pyserial and playwright plus an installed Chromium browser. Provision
first. Credentials and camera images are never written to disk.
"""
import argparse
import getpass
import hashlib
import re
import socket
import ssl
import sys
import time
import threading
import urllib.error
import urllib.request

import serial
from playwright.sync_api import sync_playwright
from camera_panel import allowed_values, check_settings_panel, set_control, slide


class CameraConsole(serial.Serial):
    def __init__(self, *args, **kwargs):
        self.drain_stop = threading.Event()
        self.diagnostic_thread = None
        super().__init__(*args, **kwargs)

    def close(self):
        self.drain_stop.set()
        super().close()
        if self.diagnostic_thread and self.diagnostic_thread is not threading.current_thread():
            self.diagnostic_thread.join(timeout=1)


def identity(port):
    console = CameraConsole(port, 115200, timeout=.2)
    console.dtr = False
    console.rts = True
    time.sleep(.1)
    console.rts = False
    text = ''
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        text += console.read(4096).decode(errors='replace')
        url = re.search(r'Local camera ready: (https://[\d.]+/)', text)
        fingerprint = re.search(r'HTTPS certificate SHA256: ([A-F0-9]{64})', text)
        if url and fingerprint:
            console.runtime_faults = []
            def drain():
                while console.is_open and not console.drain_stop.is_set():
                    try:
                        line = console.readline().decode(errors='replace').strip()
                    except (TypeError, OSError, serial.SerialException):
                        if console.drain_stop.is_set() or not console.is_open:
                            return
                        raise
                    if any(term in line.lower() for term in ('task_wdt', 'guru meditation', 'abort() was called', 'assert failed', 'stack canary', 'corrupt heap', 'rst:')):
                        console.runtime_faults.append(line)
                    if any(term in line.lower() for term in ('peer state', 'dtls', 'e (', 'abort()', 'assert failed', 'sctp', 'ice agent', 'guru meditation', 'backtrace:', 'stack canary', 'corrupt heap', 'rst:', 'camera_transport', 'stalled camera', 'task_wdt', 'closing camera peer', 'control send failed', 'video paused', 'transport:')):
                        print('USB:', line)
            console.diagnostic_thread = threading.Thread(target=drain, daemon=True)
            console.diagnostic_thread.start()
            return url[1], fingerprint[1], console
        if 'abort() was called' in text or 'ESP_ERROR_CHECK failed' in text:
            errors = [line for line in text.splitlines() if 'E (' in line or 'abort()' in line or 'ESP_ERROR_CHECK' in line]
            console.close()
            raise RuntimeError('\n'.join(errors))
    console.close()
    raise RuntimeError('No local camera URL; check provisioning and USB logs')


def wait_js(page, expression, *, arg=None, timeout=40000):
    # CDP evaluation avoids Playwright's eval-based polling, so the real page
    # keeps its strict CSP during testing.
    deadline = time.monotonic() + timeout / 1000
    while time.monotonic() < deadline:
        if page.evaluate(expression, arg):
            return
        if expression.startswith("document.querySelector('#preview')") and page.evaluate('pc === null'):
            raise RuntimeError(page.locator('#status').inner_text())
        page.wait_for_timeout(100)
    raise TimeoutError('Browser condition timed out')


def add_password_argument(parser):
    parser.add_argument('--password-stdin', action='store_true',
                        help='Read the camera password from stdin instead of prompting')


def camera_password(args):
    """The camera password, from one stdin line or a hidden prompt; never logged."""
    password = sys.stdin.readline().rstrip('\r\n') if args.password_stdin else getpass.getpass('Camera password: ')
    if not 12 <= len(password.encode()) <= 96:
        raise SystemExit('The camera password must be 12–96 UTF-8 bytes')
    return password


def enter_password(page, password):
    # Set without typing, so Playwright never records the value.
    page.locator('#camera-password').evaluate('(input, value) => { input.value = value; }', password)


def connect_local(page, password, *, timeout=40000):
    """Sign in on the board's viewer and wait for the authenticated channel."""
    enter_password(page, password)
    page.locator('#connect').click()
    wait_js(page, "authed && channel?.readyState==='open'", timeout=timeout)
    # The board checks the proof; the page must not keep the password anywhere.
    assert page.locator('#camera-password').input_value() == '', 'The password stayed in the form'
    assert password not in page.content(), 'The password appears in the page HTML'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--browser', required=True, help='Installed Chromium executable')
    add_password_argument(parser)
    args = parser.parse_args()
    password = camera_password(args)
    url, fingerprint, console = identity(args.port)
    host = url.split('/')[2]
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    with socket.create_connection((host, 443), timeout=10) as raw:
        with context.wrap_socket(raw, server_hostname=host) as tls:
            assert hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest().upper() == fingerprint
            print('HTTPS certificate matches physical USB identity;', tls.version(), tls.cipher()[0])
    try:
        with socket.create_connection((host, 80), timeout=2):
            raise AssertionError('Plain HTTP is unexpectedly available')
    except (ConnectionRefusedError, TimeoutError):
        pass

    def request_status(method='GET', data=None, headers=None):
        request = urllib.request.Request(url + 'api/session', method=method, data=data, headers=headers or {})
        try:
            with urllib.request.urlopen(request, context=context, timeout=10) as response:
                return response.status
        except urllib.error.HTTPError as error:
            return error.code

    assert request_status() == 404  # No active peer; the data channel requires the camera password.
    assert request_status(headers={'Origin': 'https://untrusted.invalid'}) == 403
    assert request_status('POST', b'invalid SDP') == 400
    print('Local session API answers; cross-origin and malformed SDP rejected')
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=args.browser, headless=True,
                                    args=['--no-sandbox'])
        page = browser.new_page(ignore_https_errors=True)
        page.on('requestfailed', lambda r: print('Browser request failure:', r.url, r.failure))
        page.on('pageerror', lambda error: print('Browser error:', error))
        page.goto(url, wait_until='domcontentloaded')
        print('Viewer loaded')
        assert page.locator('input[type=password]').count() == 1
        connect_local(page, password)
        print('Signed in with the camera password')
        try:
            wait_js(page, "document.querySelector('#preview').naturalWidth > 0", timeout=40000)
        except Exception:
            print('Viewer status:', page.locator('#status').inner_text())
            print('Peer state:', page.evaluate("pc && ({connection:pc.connectionState, ice:pc.iceConnectionState, signaling:pc.signalingState})"))
            raise
        wait_js(page, "document.querySelector('#settings-panel').value?.controls.length > 0")
        print('Decoded live JPEG:', page.locator('#settings-panel').evaluate('(panel)=>panel.value.sensor'))
        panel = page.locator('#settings-panel')
        counts = check_settings_panel(panel, panel.evaluate('(panel)=>panel.value'))
        print(f"Settings panel: {counts['switch']} On/Off switches, {counts['list']} lists, {counts['range']} number fields")
        wait_js(page, "parseFloat(document.querySelector('#fps').textContent) > 0", timeout=5000)
        print('Local displayed frame rate:', page.locator('#fps').inner_text())
        assert request_status('POST', b'busy') == 409
        controls = page.locator('#settings-panel').evaluate("panel=>panel.value.controls.map(c=>({name:c.name,value:c.value,disabled:!c.supported}))")
        page.evaluate("window.lastResponse=null; channel.addEventListener('message', e=>{if(typeof e.data==='string'){const message=JSON.parse(e.data);if(['settings','error'].includes(message.type))window.lastResponse=message;}})")
        for control in controls:
            if control['disabled']:
                continue
            page.evaluate("c=>{window.lastResponse=null;command({op:'set',name:c.name,value:c.value});}", control)
            wait_js(page, 'window.lastResponse !== null')
            response = page.evaluate('window.lastResponse')
            assert response['type'] == 'settings', (control['name'], response)
        page.evaluate("window.lastResponse=null;command({op:'get'})")
        wait_js(page, "window.lastResponse && window.lastResponse.type==='settings'")
        initial = page.evaluate('window.lastResponse')
        maximum = {'ov2640': (1600, 1200), 'ov3660': (2048, 1536), 'gc0308': (640, 480)}[initial['sensor']]
        sizes = initial['resolutions']
        assert (sizes[0]['width'], sizes[0]['height']) == (160, 120)
        assert all(size['width'] >= 160 and size['height'] >= 120 for size in sizes)
        assert (sizes[-1]['width'], sizes[-1]['height']) == maximum
        original_resolution = initial['resolution']
        for size in sizes:
            print('Testing resolution:', size['width'], '×', size['height'], flush=True)
            page.evaluate('window.lastResponse=null')
            page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(size['value']))
            wait_js(page, "value=>window.lastResponse?.resolution===value", arg=size['value'], timeout=15000)
            wait_js(page, "size=>{const img=document.querySelector('#preview');return img.naturalWidth===size.width && img.naturalHeight===size.height;}", arg=size, timeout=30000)
        page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(original_resolution))
        wait_js(page, "document.querySelector('#preview').naturalWidth===320 && document.querySelector('#preview').naturalHeight===240", timeout=30000)
        print('All', len(sizes), 'resolutions decoded, including', maximum)
        for invalid in (-1, 0, 2, 999, 6.5, 'invalid'):
            page.evaluate("value=>{window.lastResponse=null;command({op:'set',name:'resolution',value});}", invalid)
            wait_js(page, "window.lastResponse?.type==='error'")
            assert page.evaluate('window.lastResponse.error') == 'Unsupported camera resolution'
        page.evaluate("window.lastResponse=null;command({op:'get'})")
        wait_js(page, "window.lastResponse?.type==='settings'")
        initial = page.evaluate('window.lastResponse')
        if initial['sensor'] == 'gc0308':
            # The GC0308 has no OV light timing; its panel offers none.
            assert initial['timing_profiles'] == []
            assert page.get_by_role('combobox', name='Light timing', exact=True).count() == 0
        else:
            assert initial['timing_profiles'] == ['high_light', 'mid_light', 'low_light']
            page.evaluate("window.lastResponse=null;command({op:'set',name:'timing_profile',value:'high_light'})")
            wait_js(page, "window.lastResponse?.timing_profile==='high_light'")
            baseline = page.evaluate('window.lastResponse.timing_register')
            readbacks = []
            for profile, factor in [('mid_light', 2), ('low_light', 3 if initial['sensor'] == 'ov3660' else 4), ('high_light', 1)]:
                page.evaluate('window.lastResponse=null')
                page.get_by_role('combobox', name='Light timing', exact=True).select_option(profile)
                wait_js(page, 'window.lastResponse !== null')
                response = page.evaluate('window.lastResponse')
                assert response.get('timing_profile') == profile, (profile, response)
                actual = page.evaluate('window.lastResponse.timing_register')
                expected = baseline * factor if initial['sensor'] == 'ov3660' else (baseline + 1) * factor - 1
                assert actual == expected, (profile, actual, expected)
                readbacks.append((profile, actual))
                # Observe three fresh decoded frames after each mode change.
                for _ in range(3):
                    previous = page.locator('#preview').get_attribute('src')
                    wait_js(page, "src=>document.querySelector('#preview').src!==src && document.querySelector('#preview').naturalWidth>0", arg=previous)
            page.evaluate("value=>{window.lastResponse=null;command({op:'set',name:'timing_profile',value});}", initial['timing_profile'])
            wait_js(page, "value=>window.lastResponse?.timing_profile===value", arg=initial['timing_profile'])
            print('Timing profiles changed real registers; live frames continued:', readbacks)
        page.evaluate("window.lastResponse=null;command({op:'set',name:'timing_profile',value:'invalid'})")
        wait_js(page, "window.lastResponse && window.lastResponse.type==='error'")
        assert page.evaluate('window.lastResponse.error') == 'Unknown timing profile'
        original = next(c['value'] for c in controls if c['name'] == 'contrast')
        new = -1 if original != -1 else 1
        page.evaluate("value=>command({op:'set',name:'contrast',value})", new)
        wait_js(page, "v=>document.querySelector('#settings-panel').value.controls.some(c=>c.name==='contrast'&&c.value===v)", arg=new)
        page.evaluate("command({op:'set',name:'contrast',value:999})")
        wait_js(page, "document.querySelector('#status').textContent.includes('one of the advertised values')")
        page.evaluate("value=>command({op:'set',name:'contrast',value})", original)
        wait_js(page, "v=>document.querySelector('#settings-panel').value.controls.some(c=>c.name==='contrast'&&c.value===v)", arg=original)
        # Every listed value of each list, and both switch states, through the GUI.
        page.evaluate("window.lastResponse=null;command({op:'get'})")
        wait_js(page, "window.lastResponse?.type==='settings'")
        current = page.evaluate('window.lastResponse')
        page.evaluate('()=>{const p=document.querySelector("#settings-panel");p.dirty=false;p.settings=window.lastResponse;}')
        risky = {'colorbar', 'automatic_exposure', 'automatic_gain'}  # Keep the image usable while testing.
        for control in current['controls']:
            if control['kind'] == 'range' or control['name'] in risky:
                continue
            for value in [v for v in allowed_values(control) if v != control['value']] + [control['value']]:
                page.evaluate('window.lastResponse=null')
                set_control(panel, control, value)
                wait_js(page, "v=>window.lastResponse?.controls?.some(c=>c.name===v.name&&c.value===v.value)", arg={'name': control['name'], 'value': value}, timeout=10000)
        print('Every list value and switch state applied through the GUI and read back')
        # Sliders: top and bottom stops, then the exact original typed back in.
        for control in current['controls']:
            if control['kind'] != 'range':
                continue
            for position in (10, 0):
                slide(page, panel, control, position, lambda expression, arg: wait_js(page, expression, arg=arg, timeout=10000))
            page.evaluate('window.lastResponse=null')
            set_control(panel, control, control['value'])
            wait_js(page, "v=>window.lastResponse?.controls?.some(c=>c.name===v.name&&c.value===v.value)", arg={'name': control['name'], 'value': control['value']}, timeout=10000)
        print('Every slider reached its top and bottom stops; exact values restored')
        stats = page.evaluate("async()=>Array.from((await pc.getStats()).values()).filter(s=>s.type==='transport').map(s=>({dtlsState:s.dtlsState,tlsVersion:s.tlsVersion,dtlsCipher:s.dtlsCipher}))")
        assert any(s['dtlsState'] == 'connected' for s in stats), stats
        print('WebRTC transport:', stats)
        print('Accepted', sum(not c['disabled'] for c in controls), 'sensor controls; contrast change/readback and invalid value rejection passed')
        await_disconnect = page.locator('#disconnect')
        await_disconnect.click()
        wait_js(page, "!document.querySelector('#connect').disabled")
        assert page.locator('#fps').inner_text() == '0.0 FPS'
        connect_local(page, password)
        wait_js(page, "parseFloat(document.querySelector('#fps').textContent) > 0", timeout=5000)
        page.locator('#disconnect').click()
        browser.close()
        print('Disconnect and reconnect passed:', url)


if __name__ == '__main__':
    main()
