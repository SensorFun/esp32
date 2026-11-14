"""Smoke test the built site with Playwright; run against npm run preview.

Usage: python scripts/check-browser.py --url http://127.0.0.1:4173/ --browser /path/to/chromium
"""
import argparse
from playwright.sync_api import sync_playwright, expect

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True)
parser.add_argument('--browser', required=True)
args = parser.parse_args()
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox'])
    page = browser.new_page(viewport={'width': 1440, 'height': 1000})
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto(args.url)
    expect(page.get_by_role('heading', name='Small devices. Big possibilities.')).to_be_visible()
    page.get_by_role('link', name='Enter project space').click()
    expect(page.locator('.rtc-theme')).to_be_visible()
    expect(page.get_by_role('heading', name='A window into your hardware.')).to_be_visible()
    assert page.locator('.pill.amber').inner_text() == 'PRIVATE SIGNALING'
    expect(page.get_by_label('Signaling server URL')).to_have_count(0)
    expect(page.get_by_label('Signaling server secret')).to_have_count(0)
    assert page.locator('.remote-credentials input').count() == 2
    expect(page.get_by_role('button', name='Connect camera', exact=True)).to_be_disabled()
    page.get_by_role('navigation', name='WebRTC navigation').get_by_role('link', name='Connection guide').click()
    expect(page.get_by_role('heading', name='Local connection', exact=True)).to_be_visible()
    page.reload()
    expect(page.get_by_role('heading', name='Local connection', exact=True)).to_be_visible()
    assert page.locator('.rtc-sidebar a[aria-current=page]').inner_text() == 'Connection guide'
    # Invalid address content is rejected without navigating off site.
    page.locator('#camera-address').fill('https://invalid.example/path')
    page.get_by_role('button', name='Open local viewer').click()
    expect(page.get_by_role('alert')).to_be_visible()
    # Clicking a valid URL opens the board viewer; intercept to avoid network access.
    page.context.route('https://device.invalid/**', lambda route: route.fulfill(status=200, body='local viewer'))
    page.locator('#camera-address').fill('https://device.invalid/')
    with page.context.expect_page() as popup_info:
        page.get_by_role('button', name='Open local viewer').click()
    popup = popup_info.value
    popup.wait_for_load_state()
    assert popup.url == 'https://device.invalid/'
    popup.close()
    page.get_by_role('link', name='Device Spaces', exact=True).click()
    expect(page.locator('.home-theme')).to_be_visible()
    page.go_back()
    expect(page.locator('.rtc-theme')).to_be_visible()
    page.goto(args.url + '#/spaces/missing')
    expect(page.get_by_role('heading', name='This space isn’t here.')).to_be_visible()
    for route in ['#/', '#/spaces', '#/about', '#/spaces/webrtc', '#/spaces/webrtc/setup', '#/spaces/webrtc/notes']:
        page.set_viewport_size({'width': 390, 'height': 844})
        page.goto(args.url + route)
        assert page.locator('main h1, main h2').count() > 0
        assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth'), route
    assert not errors, errors
    print('Passed: navigation, theme switching, refresh, history, missing routes, local viewer form, and mobile layouts.')
    browser.close()
