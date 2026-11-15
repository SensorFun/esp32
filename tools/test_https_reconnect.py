"""Verify local HTML, settings module and WebRTC after repeated hardware resets."""
import argparse

from playwright.sync_api import sync_playwright
from test_local_webrtc import identity, wait_js


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--browser')
    parser.add_argument('--cycles', type=int, default=5)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error('--cycles must be positive')
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
                    page.on('requestfailed', lambda request: failures.append(request.url)
                            if request.url.startswith(url) else None)
                    page.goto(url, wait_until='load', timeout=15000)
                    wait_js(page, "!!customElements.get('sensorfan-camera-settings')", timeout=5000)
                    assert not failures, f'Local page assets failed: {failures}'
                    # Exercise the browser's parallel connection pool rather than
                    # serializing away the original two-socket eviction failure.
                    assert page.evaluate("async()=>Promise.all(Array.from({length:3},async()=>{const r=await fetch('/camera-settings.js',{cache:'no-store'});return r.ok&&(await r.text()).includes('sensorfan-camera-settings')}))") == [True] * 3
                    page.locator('#connect').click()
                    wait_js(page, "channel?.readyState==='open' && !!document.querySelector('#settings-panel').value")
                    wait_js(page, "document.querySelector('#preview').naturalWidth>0")
                    page.locator('#disconnect').click()
                    wait_js(page, "!document.querySelector('#connect').disabled")
                    assert not failures, f'Local requests failed: {failures}'
                    print(f'PASS: HTTPS assets, parallel requests, camera controls and video after reboot {cycle + 1}/{args.cycles}', flush=True)
                finally:
                    context.close()
                    console.close()
        finally:
            browser.close()


if __name__ == '__main__':
    main()
