"""Check the SensorFan camera interface on desktop and mobile."""
import argparse
from playwright.sync_api import sync_playwright, expect

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True)
parser.add_argument('--browser', required=True)
args = parser.parse_args()
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox'])
    page = browser.new_page(ignore_https_errors=True)
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    for width in (320, 375, 390, 768, 1440):
        page.set_viewport_size({'width': width, 'height': 1000})
        page.goto(args.url)
        expect(page).to_have_title('SensorFan')
        expect(page.get_by_role('heading', name='Projects', exact=True)).to_be_visible()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        expect(page.get_by_label('Camera MAC address', exact=True)).to_have_count(0)
        page.get_by_role('link', name='Connected cameras WebRTC', exact=False).click()
        expect(page).to_have_title('WebRTC · SensorFan')
        page.reload()
        expect(page.get_by_role('heading', name='Remote camera', exact=True)).to_be_visible()
        assert page.locator('.sensorfan-brand img').evaluate('(img)=>img.complete && img.naturalWidth>0')
        assert 'Device Spaces' not in page.locator('body').inner_text()
        assert page.get_by_role('navigation').count() == 0
        expect(page.get_by_label('Camera MAC address', exact=True)).to_be_visible()
        if width <= 600:
            assert page.get_by_label('Camera MAC address', exact=True).evaluate('(input)=>parseFloat(getComputedStyle(input).fontSize)>=16')
        assert page.get_by_role('button', name='Connect camera', exact=True).bounding_box()['height'] >= 44
        expect(page.get_by_test_id('remote-fps')).to_have_text('0.0 FPS')
        expect(page.get_by_text('Connect on your local network', exact=True)).to_have_count(0)
        expect(page.get_by_label('Camera address', exact=True)).to_have_count(0)
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        page.get_by_role('link', name='SensorFan', exact=True).click()
        expect(page.get_by_role('heading', name='Projects', exact=True)).to_be_visible()
        page.go_back()
        expect(page.get_by_label('Camera MAC address', exact=True)).to_be_visible()
    assert not errors, errors
    print('Passed: SensorFan branding, logo, remote-only camera interface, and desktop/mobile layouts.')
    browser.close()
