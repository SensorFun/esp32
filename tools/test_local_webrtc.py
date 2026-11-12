#!/usr/bin/env python3
"""Live hardware check: USB identity, HTTPS access rules, JPEG, controls, DTLS.

Requires pyserial and playwright plus an installed Chromium browser. Provision
first. Credentials and camera images are never written to disk.
"""
import argparse
import hashlib
import re
import socket
import ssl
import time
import threading
import urllib.error
import urllib.request

import serial
from playwright.sync_api import sync_playwright


def identity(port):
    console = serial.Serial(port, 115200, timeout=.2)
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
            def drain():
                while console.is_open:
                    line = console.readline().decode(errors='replace').strip()
                    if any(term in line.lower() for term in ('peer state', 'dtls', 'e (', 'abort()', 'assert failed', 'sctp', 'ice agent')):
                        print('USB:', line)
            threading.Thread(target=drain, daemon=True).start()
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--browser', required=True, help='Installed Chromium executable')
    args = parser.parse_args()
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

    assert request_status() == 404  # Accessible without credentials; no active peer.
    assert request_status(headers={'Origin': 'https://untrusted.invalid'}) == 403
    assert request_status('POST', b'invalid SDP') == 400
    print('Local API works without credentials; cross-origin and malformed SDP rejected')
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=args.browser, headless=True,
                                    args=['--no-sandbox'])
        page = browser.new_page(ignore_https_errors=True)
        page.on('requestfailed', lambda r: print('Browser request failure:', r.url, r.failure))
        page.on('pageerror', lambda error: print('Browser error:', error))
        page.goto(url, wait_until='domcontentloaded')
        print('Viewer loaded')
        assert page.locator('input[type=password]').count() == 0
        page.locator('#connect').click()
        print('Connect clicked')
        try:
            wait_js(page, "document.querySelector('#preview').naturalWidth > 0", timeout=40000)
        except Exception:
            print('Viewer status:', page.locator('#status').inner_text())
            print('Peer state:', page.evaluate("pc && ({connection:pc.connectionState, ice:pc.iceConnectionState, signaling:pc.signalingState})"))
            raise
        wait_js(page, "document.querySelectorAll('#controls label').length > 0")
        print('Decoded live JPEG:', page.locator('#sensor').inner_text())
        assert request_status('POST', b'busy') == 409
        controls = page.locator('#controls label').evaluate_all("labels=>labels.map(l=>({name:l.firstChild.textContent,value:Number(l.querySelector('input').value),disabled:l.querySelector('input').disabled}))")
        page.evaluate("window.lastResponse=null; channel.addEventListener('message', e=>{if(typeof e.data==='string') window.lastResponse=JSON.parse(e.data);})")
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
        assert initial['timing_profiles'] == ['high_light', 'balanced', 'low_light']
        assert initial['timing_profile'] == 'high_light'
        baseline = initial['timing_register']
        readbacks = []
        for profile, factor in [('balanced', 2), ('low_light', 3 if initial['sensor'] == 'ov3660' else 4), ('high_light', 1)]:
            page.evaluate('window.lastResponse=null')
            page.locator('#timing select').select_option(profile)
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
        print('Timing profiles changed real registers; live frames continued:', readbacks)
        page.evaluate("window.lastResponse=null;command({op:'set',name:'timing_profile',value:'invalid'})")
        wait_js(page, "window.lastResponse && window.lastResponse.type==='error'")
        assert page.evaluate('window.lastResponse.error') == 'Unknown timing profile'
        original = next(c['value'] for c in controls if c['name'] == 'contrast')
        new = -1 if original != -1 else 1
        page.evaluate("value=>command({op:'set',name:'contrast',value})", new)
        wait_js(page, "v=>Array.from(document.querySelectorAll('#controls label')).some(l=>l.firstChild.textContent==='contrast' && Number(l.querySelector('input').value)===v)", arg=new)
        page.evaluate("command({op:'set',name:'contrast',value:999})")
        wait_js(page, "document.querySelector('#status').textContent.includes('within the advertised range')")
        page.evaluate("value=>command({op:'set',name:'contrast',value})", original)
        wait_js(page, "v=>Array.from(document.querySelectorAll('#controls label')).some(l=>l.firstChild.textContent==='contrast' && Number(l.querySelector('input').value)===v)", arg=original)
        stats = page.evaluate("async()=>Array.from((await pc.getStats()).values()).filter(s=>s.type==='transport').map(s=>({dtlsState:s.dtlsState,tlsVersion:s.tlsVersion,dtlsCipher:s.dtlsCipher}))")
        assert any(s['dtlsState'] == 'connected' for s in stats), stats
        print('WebRTC transport:', stats)
        print('Accepted', sum(not c['disabled'] for c in controls), 'sensor controls; contrast change/readback and invalid value rejection passed')
        await_disconnect = page.locator('#disconnect')
        await_disconnect.click()
        wait_js(page, "!document.querySelector('#connect').disabled")
        page.locator('#connect').click()
        wait_js(page, "channel && channel.readyState==='open'", timeout=40000)
        page.locator('#disconnect').click()
        browser.close()
        print('Disconnect and reconnect passed:', url)


if __name__ == '__main__':
    main()
