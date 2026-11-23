"""Verify local HTML, settings module and WebRTC after repeated hardware resets.

The first cycle also checks the viewer's camera password: plain HTTP is not
served, a session that skips authentication receives no frames, settings or
recordings, and a wrong password is rejected before the right one streams.
"""
import argparse
import secrets
import socket

from playwright.sync_api import sync_playwright
from test_local_webrtc import add_password_argument, camera_password, connect_local, enter_password, identity, wait_js

# Wraps the page's data channel before it exists, to count binary messages
# (video frames and recording chunks) the camera sends.
COUNT_BINARY = """()=>{
  window.binaryMessages=0;
  const create=RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel=function(...args){
    const created=create.apply(this,args);
    created.addEventListener('message',event=>{if(typeof event.data!=='string')binaryMessages++;});
    return created;
  };
}"""

# Replaces the page's password proof with requests for camera data, as a
# client that ignores the challenge would send.
SKIP_AUTHENTICATION = """()=>{
  window.seen=[];
  authenticate=data=>{
    seen.push(data);
    // Read-only requests: a broken gate must not change the camera or its card.
    if(data.type==='auth_challenge')for(const op of ['get','motion_get','recordings','time'])command({op});
  };
}"""


def check_password_required(page, url):
    host = url.split('/')[2]
    try:
        with socket.create_connection((host, 80), timeout=3):
            raise AssertionError('Plain HTTP is unexpectedly available')
    except (ConnectionRefusedError, TimeoutError):
        pass

    page.evaluate(COUNT_BINARY)
    page.evaluate(SKIP_AUTHENTICATION)
    enter_password(page, 'unused-' + secrets.token_urlsafe(12))
    page.locator('#connect').click()
    wait_js(page, "seen.filter(m=>m.type==='auth_error').length>=4", timeout=40000)
    page.wait_for_timeout(2000)  # Room for any frame or reply that should not come.
    seen = page.evaluate('seen')
    assert seen[0]['type'] == 'auth_challenge', seen
    assert all(m['type'] in ('auth_challenge', 'auth_error') for m in seen), seen
    assert all(m['error'] == 'Camera password authentication required' for m in seen[1:]), seen
    assert page.evaluate('binaryMessages') == 0, 'Camera sent frames before authentication'
    assert page.evaluate("!document.querySelector('#preview').getAttribute('src')")
    page.evaluate('stop()')
    wait_js(page, "!document.querySelector('#connect').disabled")
    print('PASS: plain HTTP closed; unauthenticated session received no frames, settings or recordings', flush=True)

    page.reload(wait_until='load')
    wait_js(page, "!!customElements.get('sensorfan-camera-settings') && !document.querySelector('#connect').disabled", timeout=5000)
    page.evaluate(COUNT_BINARY)
    enter_password(page, 'wrong-' + secrets.token_urlsafe(16))
    page.locator('#connect').click()
    wait_js(page, "document.querySelector('#status').textContent.includes('Camera authentication failed')", timeout=40000)
    assert not page.evaluate('authed')
    assert page.evaluate('binaryMessages') == 0, 'Camera sent frames to a wrong password'
    assert page.evaluate("document.querySelector('#settings-panel').hidden")
    print('PASS: wrong camera password rejected', flush=True)
    page.reload(wait_until='load')
    wait_js(page, "!!customElements.get('sensorfan-camera-settings') && !document.querySelector('#connect').disabled", timeout=5000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--browser')
    parser.add_argument('--cycles', type=int, default=5)
    add_password_argument(parser)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error('--cycles must be positive')
    password = camera_password(args)
    with sync_playwright() as p:
        # Context-level certificate overrides let Chromium abort and retry
        # speculative TLS handshakes on these self-signed, per-boot identities.
        # Apply the same test trust policy at the browser network layer too.
        browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox', '--ignore-certificate-errors'])
        try:
            for cycle in range(args.cycles):
                url, _, console = identity(args.port)
                context = browser.new_context(ignore_https_errors=True)
                try:
                    page = context.new_page()
                    failures = []
                    page.on('requestfailed', lambda request: failures.append(f'{request.method} {request.url}: {request.failure}')
                            if request.url.startswith(url) else None)
                    page.goto(url, wait_until='load', timeout=15000)
                    wait_js(page, "!!customElements.get('sensorfan-camera-settings')", timeout=5000)
                    assert not failures, f'Local page assets failed: {failures}'
                    # Exercise the browser's parallel connection pool rather than
                    # serializing away the original two-socket eviction failure.
                    assert page.evaluate("async()=>Promise.all(Array.from({length:3},async()=>{const r=await fetch('/camera-settings.js',{cache:'no-store'});return r.ok&&(await r.text()).includes('sensorfan-camera-settings')}))") == [True] * 3
                    if cycle == 0:
                        check_password_required(page, url)
                        # Its reloads abort the viewer's unread POST/DELETE
                        # response bodies (net::ERR_ABORTED); track afresh.
                        failures.clear()
                    connect_local(page, password)
                    wait_js(page, "!!document.querySelector('#settings-panel').value")
                    wait_js(page, "document.querySelector('#preview').naturalWidth>0")
                    page.locator('#disconnect').click()
                    wait_js(page, "!document.querySelector('#connect').disabled")
                    assert not failures, f'Local requests failed: {failures}'
                    print(f'PASS: HTTPS assets, parallel requests, camera password, controls and video after reboot {cycle + 1}/{args.cycles}', flush=True)
                finally:
                    context.close()
                    console.close()
        finally:
            browser.close()


if __name__ == '__main__':
    main()
