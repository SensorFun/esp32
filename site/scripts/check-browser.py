"""Check the SensorFun ESP32 camera interface on desktop and mobile."""
import argparse
import json
import tempfile
from pathlib import Path
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
        expect(page).to_have_title('ESP32 · SensorFun')
        expect(page.get_by_role('heading', name='ESP32 projects', exact=True)).to_be_visible()
        assert page.locator('.sensorfan-brand img').evaluate('(img)=>img.complete && img.naturalWidth>0')
        assert page.get_by_role('link', name='SensorFun', exact=True).get_attribute('href') == 'https://sensorfun.net/'
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        expect(page.get_by_label('Camera MAC address', exact=True)).to_have_count(0)
        page.get_by_role('link', name='Connected cameras WebRTC', exact=False).click()
        expect(page).to_have_title('WebRTC · ESP32 · SensorFun')
        page.reload()
        expect(page.get_by_role('heading', name='Remote camera', exact=True)).to_be_visible()
        trail = page.get_by_role('navigation', name='Breadcrumb')
        assert trail.get_by_role('link', name='ESP32', exact=True).bounding_box()['height'] >= 44
        expect(trail.get_by_role('link', name='WebRTC', exact=True)).to_have_attribute('aria-current', 'page')
        expect(page.get_by_label('Camera MAC address', exact=True)).to_be_visible()
        if width <= 600:
            assert page.get_by_label('Camera MAC address', exact=True).evaluate('(input)=>parseFloat(getComputedStyle(input).fontSize)>=16')
        assert page.get_by_role('button', name='Connect camera', exact=True).bounding_box()['height'] >= 44
        expect(page.get_by_test_id('remote-fps')).to_have_text('0.0 FPS')
        expect(page.get_by_text('Connect on your local network', exact=True)).to_have_count(0)
        expect(page.get_by_label('Camera address', exact=True)).to_have_count(0)
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        trail.get_by_role('link', name='ESP32', exact=True).click()
        expect(page.get_by_role('heading', name='ESP32 projects', exact=True)).to_be_visible()
        page.go_back()
        expect(page.get_by_label('Camera MAC address', exact=True)).to_be_visible()
        page.goto(args.url.split('#')[0] + '#/missing')
        expect(page.get_by_role('heading', name='Page not found', exact=True)).to_be_visible()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        page.get_by_role('link', name='ESP32 projects', exact=True).click()
        expect(page.get_by_role('heading', name='ESP32 projects', exact=True)).to_be_visible()
    assert not errors, errors

    # Flasher: fake Web Serial ports and a synthetic build folder; no board is contacted.
    fixture = Path(tempfile.mkdtemp())
    for board, chip in (('esp32_ai-thinker_cam', 'esp32'), ('esp32s3n16r8_espressif_cam', 'esp32s3')):
        folder = fixture / 'webrtc' / board
        (folder / 'bootloader').mkdir(parents=True)
        (folder / 'partition_table').mkdir()
        (folder / 'bootloader' / 'bootloader.bin').write_bytes(bytes(2048))
        (folder / 'partition_table' / 'partition-table.bin').write_bytes(bytes(3072))
        (folder / 'webrtc.bin').write_bytes(bytes(8192))
        (folder / 'firmware_selection.json').write_text(json.dumps({'board': board, 'frame_grabber': 'ov'}))
        (folder / 'flasher_args.json').write_text(json.dumps({
            'flash_settings': {'flash_mode': 'dio', 'flash_size': '4MB', 'flash_freq': '80m'},
            'flash_files': {'0x0': 'bootloader/bootloader.bin', '0x8000': 'partition_table/partition-table.bin', '0x10000': 'webrtc.bin'},
            'extra_esptool_args': {'after': 'hard-reset', 'before': 'default-reset', 'stub': True, 'chip': chip}}))
    fake_serial = '''(() => {
      const make = (vid, pid) => ({ getInfo: () => ({ usbVendorId: vid, usbProductId: pid }), async forget() { ports.splice(ports.indexOf(this), 1); } });
      const ports = [make(0x303a, 0x1001), make(0x1a86, 0x7523)];
      Object.defineProperty(navigator, 'serial', { value: { getPorts: async () => [...ports], requestPort: async () => { const port = make(0x10c4, 0xea60); ports.push(port); return port; }, addEventListener() {}, removeEventListener() {} } });
      Object.defineProperty(window, 'showDirectoryPicker', { value: undefined });
    })();'''
    flasher_url = args.url.split('#')[0] + '#/projects/flasher'
    for width in (320, 390, 768, 1440):
        context = browser.new_context(viewport={'width': width, 'height': 1000})
        context.add_init_script(fake_serial)
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(flasher_url)
        expect(page).to_have_title('Flasher · ESP32 · SensorFun')
        expect(page.get_by_role('heading', name='Flash firmware', exact=True)).to_be_visible()
        expect(page.get_by_text('This browser cannot flash boards')).to_have_count(0)
        expect(page.get_by_role('radio', name='Espressif USB (303a:1001)')).to_be_visible()
        expect(page.get_by_role('radio', name='WCH CH340 (1a86:7523)')).to_be_visible()
        page.get_by_role('button', name='Add USB port', exact=True).click()
        expect(page.get_by_role('radio', name='Silicon Labs CP210x (10c4:ea60)')).to_be_checked()
        page.get_by_role('button', name='Forget WCH CH340 (1a86:7523)').click()
        expect(page.get_by_role('radio', name='WCH CH340 (1a86:7523)')).to_have_count(0)
        flash = page.get_by_role('button', name='Flash firmware', exact=True)
        expect(flash).to_be_disabled()
        # Chromium's automated directory upload fails below ~600px; upload wide, then restore the width.
        page.set_viewport_size({'width': 1280, 'height': 1000})
        page.locator('input[type=file]').set_input_files(str(fixture))
        page.set_viewport_size({'width': width, 'height': 1000})
        page.get_by_role('radio', name='webrtc/esp32s3n16r8_espressif_cam').check()
        summary = page.get_by_test_id('firmware-summary')
        expect(summary).to_contain_text('ESP32-S3')
        expect(summary).to_contain_text('0x10000')
        expect(summary).to_contain_text('webrtc.bin')
        expect(flash).to_be_enabled()
        for name in ('Choose build folder', 'Add USB port', 'Flash firmware'):
            assert page.get_by_role('button', name=name, exact=True).bounding_box()['height'] >= 44
        if width <= 600:
            assert page.get_by_label('Speed').evaluate('(select)=>parseFloat(getComputedStyle(select).fontSize)>=16')
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        flash.click()
        expect(page.get_by_test_id('flash-status')).not_to_have_text('Connecting to the board…', timeout=10000)
        expect(page.get_by_test_id('flash-status')).not_to_be_empty()
        expect(flash).to_be_enabled()
        context.close()
    context = browser.new_context()
    context.add_init_script("Object.defineProperty(Navigator.prototype, 'serial', { get: () => undefined }); delete Navigator.prototype.serial;")
    page = context.new_page()
    page.goto(flasher_url)
    expect(page.get_by_text('This browser cannot flash boards')).to_be_visible()
    expect(page.get_by_role('button', name='Add USB port', exact=True)).to_be_disabled()
    context.close()
    assert not errors, errors
    print('Passed: SensorFun ESP32 branding, logo, project list, breadcrumbs, flasher, remote-only camera interface, and desktop/mobile layouts.')
    browser.close()
