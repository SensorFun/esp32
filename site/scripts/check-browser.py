"""Check the SensorFun ESP32 camera interface on desktop and mobile."""
import argparse
import base64
import hashlib
import json
import re
import struct
import tempfile
from pathlib import Path
from urllib.parse import urljoin
from playwright.sync_api import sync_playwright, expect
import yaml

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
        page.goto(urljoin(args.url.split('#')[0], 'missing'))
        expect(page.get_by_role('heading', name='Page not found', exact=True)).to_be_visible()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        page.get_by_role('link', name='ESP32 projects', exact=True).click()
        expect(page.get_by_role('heading', name='ESP32 projects', exact=True)).to_be_visible()
    assert not errors, errors

    # Flasher: fake Web Serial ports, synthetic firmware packages served by route,
    # and a synthetic build folder; no board is contacted.
    workspace = Path(__file__).resolve().parents[2]
    catalog = {}
    for catalog_file in sorted((workspace / 'config/boards').glob('*.yaml')):
        for board, entry in yaml.safe_load(catalog_file.read_text()).items():
            catalog[board] = entry
    hosted = Path(tempfile.mkdtemp())
    uploads = Path(tempfile.mkdtemp())
    def esp_image(chip_code, app, project=b'webrtc'):
        data = bytearray(8192)
        data[0] = 0xE9
        data[12:14] = chip_code.to_bytes(2, 'little')
        if app:
            data[0x20:0x24] = (0xABCD5432).to_bytes(4, 'little')
            data[0x30:0x35] = b'1.2.3'
            data[0x50:0x50 + len(project)] = project
        return bytes(data)
    (uploads / 'webrtc.bin').write_bytes(esp_image(9, True))
    merged = bytearray(b'\xff' * 0x12000)
    merged[0x1000:0x3000] = esp_image(0, False)
    merged[0x10000:0x12000] = esp_image(0, True)
    (uploads / 'merged.bin').write_bytes(merged)
    (uploads / 'notes.bin').write_bytes(b'not firmware' * 20)
    def write_build(folder, board, chip):
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
    for board in ('esp32_ai-thinker_cam', 'esp32s3n16r8_espressif_cam'):
        write_build(hosted / 'webrtc' / board, board, catalog[board]['target'])
    missing = 'esp32s3n16r8_goouuu_cam'
    manifest = {'webrtc': {
        'esp32_ai-thinker_cam': {'commit': 'abc123def456', 'built': 1763200000, 'frame_grabber': 'ov', 'private': False},
        'esp32s3n16r8_espressif_cam': {'commit': 'abc123def456', 'built': 1763200000, 'frame_grabber': 'ov', 'private': False},
    }}
    firmware_catalog = {path.stem: yaml.safe_load(path.read_text()) for path in sorted((workspace / 'config/firmware').glob('*.yaml'))}
    def serve_firmware(route):
        relative = route.request.url.split('/firmware/', 1)[1].split('?')[0]
        if relative == 'index.json':
            return route.fulfill(json=manifest)
        target = (hosted / relative).resolve()
        if not target.is_relative_to(hosted) or not target.is_file():
            return route.fulfill(status=404, body='')
        return route.fulfill(body=target.read_bytes(), content_type='application/octet-stream')
    fake_serial = '''(() => {
      const make = (vid, pid) => ({ getInfo: () => ({ usbVendorId: vid, usbProductId: pid }), async forget() { ports.splice(ports.indexOf(this), 1); } });
      // Two previously allowed ports: neither is assumed to be the board.
      const ports = [make(0x303a, 0x1001), make(0x0403, 0x6001)];
      const offered = [[0x10c4, 0xea60], [0x1a86, 0x7523]];
      Object.defineProperty(navigator, 'serial', { value: { getPorts: async () => [...ports], requestPort: async () => { const port = make(...offered.shift()); ports.push(port); return port; }, addEventListener() {}, removeEventListener() {} } });
      window.allowedPorts = () => ports.map(port => port.getInfo().usbVendorId);
      Object.defineProperty(window, 'showDirectoryPicker', { value: undefined });
    })();'''
    flasher_url = urljoin(args.url.split('#')[0], 'tools/flasher')

    def flasher_tab(page, name):
        page.get_by_role('tab', name=name, exact=True).click()

    for width in (320, 390, 768, 1440):
        context = browser.new_context(viewport={'width': width, 'height': 1000})
        context.add_init_script(fake_serial)
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.route('**/firmware/**', serve_firmware)
        page.goto(flasher_url)
        expect(page).to_have_title('Flasher · ESP32 · SensorFun')
        expect(page.get_by_role('heading', name='Flash firmware', exact=True)).to_be_visible()
        expect(page.get_by_text('This browser cannot flash boards')).to_have_count(0)
        tabs = page.get_by_role('tablist', name='Flasher setup')
        expect(tabs.get_by_role('tab')).to_have_text(['Firmware', 'USB Port', 'Flash'])
        expect(page.get_by_role('tabpanel')).to_have_count(1)
        boxes = [tab.bounding_box() for tab in tabs.get_by_role('tab').all()]
        assert all(box['y'] == boxes[0]['y'] and box['height'] >= 44 for box in boxes)
        assert boxes[0]['x'] < boxes[1]['x'] < boxes[2]['x']
        tabs.get_by_role('tab', name='Firmware').press('End')
        expect(page.get_by_role('tabpanel', name='Flash')).to_be_visible()
        tabs.get_by_role('tab', name='Flash').press('Home')
        expect(page.get_by_role('tabpanel', name='Firmware')).to_be_visible()
        # Every catalog board appears with its name and a loaded image.
        cards = page.locator('.flasher-board')
        expect(cards).to_have_count(len(catalog))
        for board, entry in catalog.items():
            card = cards.filter(has_text=entry['name'])
            expect(card).to_have_count(1)
            expect(card.locator('img')).to_have_count(1)
        page.wait_for_function("()=>[...document.querySelectorAll('.flasher-board img')].every(img=>img.complete&&img.naturalWidth>0)")
        goouuu = page.get_by_role('radio', name=re.compile('^' + re.escape(catalog[missing]['name'])))
        expect(goouuu).to_be_disabled()
        expect(cards.filter(has_text=catalog[missing]['name'])).to_contain_text('No firmware yet')
        # One board: Select USB port picks a single port; there is no list to add to.
        flasher_tab(page, 'USB Port')
        selected = page.get_by_test_id('selected-port')
        expect(selected).to_have_count(0)
        expect(page.get_by_role('button', name='Add USB port', exact=True)).to_have_count(0)
        expect(page.get_by_role('radio', name=re.compile('USB|CP210x|FTDI'))).to_have_count(0)
        page.get_by_role('button', name='Select USB port', exact=True).click()
        expect(selected).to_contain_text('Silicon Labs CP210x (10c4:ea60)')
        expect(selected).to_contain_text('ESP32 programmer')
        assert page.get_by_role('button', name='Select USB port', exact=True).bounding_box()['height'] >= 44
        flasher_tab(page, 'Flash')
        flash = page.get_by_role('button', name=re.compile(r'^Flash firmware(?: \(keep settings\))?$'))
        expect(flash).to_be_disabled()
        # Hosted firmware: choose a board card.
        flasher_tab(page, 'Firmware')
        s3 = catalog['esp32s3n16r8_espressif_cam']['name']
        page.get_by_role('radio', name=re.compile('^' + re.escape(s3))).check()
        summary = page.get_by_test_id('firmware-summary')
        expect(summary).to_contain_text(s3)
        expect(summary).to_contain_text('ESP32-S3')
        expect(summary).to_contain_text('abc123def456')
        expect(summary.locator('table')).to_have_count(0)
        assert cards.first.bounding_box()['height'] >= 44
        flasher_tab(page, 'Flash')
        expect(flash).to_be_enabled()
        assert flash.bounding_box()['height'] >= 44
        if width <= 600:
            assert page.get_by_label('Speed').evaluate('(select)=>parseFloat(getComputedStyle(select).fontSize)>=16')
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        flash.click()
        expect(page.get_by_test_id('flash-status')).not_to_have_text('Connecting to the board…', timeout=10000)
        expect(page.get_by_test_id('flash-status')).not_to_be_empty()
        expect(flash).to_be_enabled()
        # Upload one firmware file instead of a board package.
        flasher_tab(page, 'Firmware')
        page.get_by_label('Firmware source', exact=True).select_option('upload')
        upload = page.get_by_role('button', name='Upload firmware file', exact=True)
        assert upload.bounding_box()['height'] >= 44
        page.get_by_label('Firmware file').set_input_files(str(uploads / 'webrtc.bin'))
        expect(summary).to_contain_text('webrtc.bin')
        expect(summary).to_contain_text('App image · webrtc 1.2.3')
        expect(summary).to_contain_text('0x10000')
        expect(summary).to_contain_text('ESP32-S3')
        expect(page.locator('.flasher-board')).to_have_count(0)
        flasher_tab(page, 'Flash')
        expect(flash).to_be_enabled()
        flasher_tab(page, 'Firmware')
        page.get_by_label('Firmware file').set_input_files(str(uploads / 'merged.bin'))
        expect(summary).to_contain_text('Full flash image')
        expect(summary).to_contain_text('ESP32')
        page.get_by_label('Firmware file').set_input_files(str(uploads / 'notes.bin'))
        expect(page.get_by_role('alert')).to_contain_text('notes.bin is not an ESP32 firmware image')
        expect(summary).to_have_count(0)
        flasher_tab(page, 'Flash')
        expect(flash).to_be_disabled()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        context.close()
    # Every config/firmware project is offered; ?project= links straight to one.
    context = browser.new_context(viewport={'width': 1280, 'height': 1000})
    context.add_init_script(fake_serial)
    page = context.new_page()
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.route('**/firmware/**', serve_firmware)
    # An obsolete bookmark falls back to WebRTC. Preserve coverage of non-default upload offsets.
    manifest['webrtc']['esp32s3n16r8_espressif_cam']['app_offset'] = '0x20000'
    page.goto(flasher_url + '?project=hello_world')
    source = page.get_by_label('Firmware source', exact=True)
    expect(source).to_have_value('webrtc')
    options = source.locator('option').all_inner_texts()
    for entry in firmware_catalog.values():
        assert entry['name'] in options, (entry['name'], options)
    assert 'Hello World' not in options
    expect(page.get_by_text(firmware_catalog['webrtc']['description'])).to_be_visible()
    summary = page.get_by_test_id('firmware-summary')
    board = catalog['esp32s3n16r8_espressif_cam']
    page.get_by_role('radio', name=re.compile('^' + re.escape(board['name']))).check()
    expect(summary).to_contain_text('abc123def456')
    source.select_option('upload')
    page.get_by_label('Firmware file').set_input_files(str(uploads / 'webrtc.bin'))
    expect(summary).to_contain_text('App image · webrtc 1.2.3')
    expect(summary).to_contain_text('0x20000')
    # Selecting another board replaces the port and forgets the previous one.
    flasher_tab(page, 'USB Port')
    page.get_by_role('button', name='Select USB port', exact=True).click()
    expect(page.get_by_test_id('selected-port')).to_contain_text('Silicon Labs CP210x (10c4:ea60)')
    page.get_by_role('button', name='Select USB port', exact=True).click()
    expect(page.get_by_test_id('selected-port')).to_contain_text('WCH CH340 (1a86:7523)')
    assert page.evaluate('window.allowedPorts()') == [0x303a, 0x0403, 0x1a86], page.evaluate('window.allowedPorts()')
    page.goto(flasher_url + '?project=nonexistent')
    page.reload()
    expect(page.get_by_label('Firmware source', exact=True)).to_have_value('webrtc')
    context.close()
    # A single port allowed earlier is selected again automatically.
    context = browser.new_context()
    context.add_init_script("Object.defineProperty(navigator, 'serial', { value: { getPorts: async () => [{ getInfo: () => ({ usbVendorId: 0x1a86, usbProductId: 0x55d3 }) }], requestPort: async () => { throw new DOMException('none', 'NotFoundError'); }, addEventListener() {}, removeEventListener() {} } });")
    page = context.new_page()
    page.goto(flasher_url)
    flasher_tab(page, 'USB Port')
    expect(page.get_by_test_id('selected-port')).to_contain_text('WCH CH340 (1a86:55d3)')
    context.close()
    context = browser.new_context()
    context.add_init_script("Object.defineProperty(Navigator.prototype, 'serial', { get: () => undefined }); delete Navigator.prototype.serial;")
    page = context.new_page()
    page.goto(flasher_url)
    expect(page.get_by_text('This browser cannot flash boards')).to_be_visible()
    flasher_tab(page, 'USB Port')
    expect(page.get_by_role('button', name='Select USB port', exact=True)).to_be_disabled()
    flasher_tab(page, 'Flash')
    expect(page.get_by_role('button', name='Flash firmware (keep settings)', exact=True)).to_be_disabled()
    context.close()

    # A different ESP-IDF project: NVS has a non-default label and address.
    # Patch only the SDK's hardware methods; exercise the real React handler,
    # partition parser, erase command construction and verification in-browser.
    table = bytearray(b'\xff' * 4096)
    table[:32] = struct.pack('<HBBII16sI', 0x50aa, 1, 2, 0x20000, 0x5000, b'preferences', 0)
    table[32:64] = b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(table[:32]).digest()
    sdk = next((workspace / 'site/dist/assets').glob('lib-*.js'))
    sdk_url = urljoin(args.url.split('#')[0], 'assets/' + sdk.name)
    for width in (320, 390, 768, 1440):
        context = browser.new_context(viewport={'width': width, 'height': 1000})
        context.add_init_script(r"""const testPort = {
          getInfo:()=>({usbVendorId:0x0403,usbProductId:0x6001}),
          async open(options) {
            if (options.baudRate !== 115200) throw Error('Unexpected USB setup baud rate');
            let output, polls = 0, failed = false;
            const encode = new TextEncoder();
            const reply = data => {
              output.enqueue(encode.encode('Board diagnostic\nSENSOR'));
              output.enqueue(encode.encode('FUN_USB ' + JSON.stringify(data) + '\r\n'));
            };
            this.readable = new ReadableStream({start(controller) { output = controller; }});
            this.writable = new WritableStream({write(bytes) {
              const command = new TextDecoder().decode(bytes).trim();
              if (command === 'sensorfan-usb scan') {
                reply({result:'scan',ssids:['Home','Test+network'],camera_password_required:!window.testPasswordSaved,
                       camera_password_saved:!!window.testPasswordSaved});
              } else if (command === 'sensorfan-usb status') {
                polls++;
                const project = 'testBoardProject' in window ? window.testBoardProject : 'webrtc';
                if (polls >= 3 && !failed) window.testPasswordSaved = true;
                reply({status:true,project,result:polls < 3 ? 'pending' : failed ? 'restored' : 'saved',
                       ...(polls >= 3 ? {ip:'192.0.2.1',remote_url:'https://esp32.sensorfun.net/projects/webrtc/A1B2C3D4E5F6'} : {})});
              } else if (command === 'sensorfan-usb wifi ssid=Test%2Bnetwork&password=&camera_password=' + 'x'.repeat(12)) {
                reply({result:'pending'});
              } else if (window.testPasswordSaved && command === 'sensorfan-usb wifi ssid=Test%2Bnetwork&password=&camera_password=' + 'y'.repeat(12)) {
                reply({result:'pending'});
              } else if (command === 'sensorfan-usb wifi ssid=Missing&password=') {
                failed = true;
                reply({result:'pending'});
              } else {
                reply({result:'invalid'});
              }
            }});
          },
          async close() {},
        };
        window.open = url => { window.openedUrl = url; return null; };
        Object.defineProperty(navigator,'serial',{value:{
          getPorts:async()=>[testPort],
          addEventListener(){},removeEventListener(){},
        }});""")
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(flasher_url)
        await_patch = '''async config => {
          const {ESPLoader, Transport}=await import(config.url);
          window.operations=[];window.nvsConnections=0;window.nvsErases=[];window.nvsVerifies=[];window.nvsRestarts=0;window.nvsCloses=0;
          ESPLoader.prototype.main=async function(mode){
            window.lastConnectionMode=mode;
            if(window.requireManualBoot && mode!=='no_reset')throw Error('Failed to connect with the device');
            window.nvsConnections++;this.IS_STUB=true;this.chip={CHIP_NAME:'ESP32-S3'};
            await new Promise(resolve=>{window.nvsContinue=resolve;});
          };
          ESPLoader.prototype.detectFlashSize=async()=> '4MB';
          ESPLoader.prototype.readFlash=async(address,size)=>{
            if(address!==0x8000||size!==4096)throw Error('Wrong partition table read');
            return window.nvsBadTable?new Uint8Array(4096):Uint8Array.from(atob(config.table),c=>c.charCodeAt(0));
          };
          Transport.prototype.read=async()=>new Uint8Array(config.digest);
          ESPLoader.prototype.checkCommand=async function(description,op,data){
            if(op!==this.ESP_ERASE_REGION)throw Error('Unexpected command');
            window.nvsErases.push(Array.from(data));window.operations.push('erase');return 0;
          };
          ESPLoader.prototype.flashMd5sum=async(address,size)=>{window.nvsVerifies.push([address,size]);return config.erased;};
          ESPLoader.prototype.after=async()=>{window.nvsRestarts++;};
          Transport.prototype.disconnect=async()=>{window.nvsCloses++;};
          ESPLoader.prototype.writeFlash=async options=>{if(options.eraseAll)throw Error('Unexpected full erase');window.operations.push('flash');};
          ESPLoader.prototype.eraseFlash=async()=>{throw Error('Full flash must not be erased');};
        }'''
        page.evaluate(await_patch, {'url': sdk_url, 'table': base64.b64encode(table).decode(),
                                   'digest': list(hashlib.md5(table).digest()),
                                   'erased': hashlib.md5(b'\xff' * 0x5000).hexdigest()})
        flasher_tab(page, 'Flash')
        write_firmware = page.get_by_role('checkbox', name='Flash firmware', exact=True)
        clear_settings = page.get_by_role('checkbox', name=re.compile('^Clear settings'))
        expect(write_firmware).to_be_checked()
        expect(clear_settings).not_to_be_checked()
        write_firmware.uncheck()
        expect(page.get_by_role('button', name='Choose an action', exact=True)).to_be_disabled()
        clear_settings.check()
        reset = page.get_by_role('button', name='Clear settings', exact=True)
        expect(reset).to_be_enabled()
        expect(page.get_by_test_id('firmware-summary')).to_have_count(0)
        page.once('dialog', lambda dialog: dialog.dismiss())
        reset.click()
        page.wait_for_function('window.nvsConnections===1')
        flasher_tab(page, 'Firmware')
        expect(page.get_by_label('Firmware source', exact=True)).to_be_disabled()
        flasher_tab(page, 'USB Port')
        expect(page.get_by_role('button', name='Select USB port', exact=True)).to_be_disabled()
        flasher_tab(page, 'Flash')
        page.evaluate('window.nvsContinue()')
        expect(page.get_by_test_id('flash-status')).to_contain_text('Cancelled. No settings erased.')
        assert page.evaluate('window.nvsErases') == []
        assert page.evaluate('window.nvsCloses') == 1
        page.once('dialog', lambda dialog: dialog.accept())
        reset.click()
        page.wait_for_function('window.nvsConnections===2')
        page.evaluate('window.nvsContinue()')
        expect(page.get_by_test_id('flash-status')).to_contain_text('erased and verified. Firmware kept.')
        expect(reset).to_be_enabled()
        expect(page.get_by_role('progressbar')).to_have_count(0)
        assert page.evaluate('window.nvsErases') == [[0, 0, 2, 0, 0, 0x50, 0, 0]]
        assert page.evaluate('window.nvsVerifies') == [[0x20000, 0x5000]]
        assert page.evaluate('window.nvsRestarts') == 2
        assert page.evaluate('window.nvsCloses') == 2
        page.get_by_text('Advanced options', exact=True).click()
        assert page.get_by_label('Partition table address', exact=True).bounding_box()['height'] >= 44
        assert reset.evaluate('button => button.offsetHeight') >= 44
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
        if width == 1440:
            page.evaluate('window.requireManualBoot=true')
            reset.click()
            expect(page.get_by_test_id('flash-status')).to_contain_text('Select Manual bootloader')
            assert page.evaluate('window.nvsErases.length') == 1
            page.get_by_label('Connection', exact=True).select_option('manual')
            expect(page.get_by_label('Speed', exact=True)).to_be_disabled()
            page.evaluate('window.nvsBadTable=true')
            reset.click()
            page.wait_for_function('window.nvsConnections===3')
            assert page.evaluate('window.lastConnectionMode') == 'no_reset'
            page.evaluate('window.nvsContinue()')
            expect(page.get_by_test_id('flash-status')).to_contain_text('checksum failed. Nothing erased.')
            expect(reset).to_be_enabled()
            assert len(page.evaluate('window.nvsErases')) == 1
            page.evaluate('window.nvsBadTable=false')
            write_firmware.check()
            combined = page.get_by_role('button', name='Clear settings and flash', exact=True)
            expect(combined).to_be_disabled()
            flasher_tab(page, 'Firmware')
            page.get_by_label('Firmware source', exact=True).select_option('upload')
            page.get_by_label('Firmware file').set_input_files(str(uploads / 'webrtc.bin'))
            flasher_tab(page, 'Flash')
            expect(combined).to_be_enabled()
            page.once('dialog', lambda dialog: dialog.accept())
            combined.click()
            page.wait_for_function('window.nvsConnections===4')
            page.evaluate('window.nvsContinue()')
            expect(page.get_by_test_id('flash-status')).to_contain_text('Settings cleared. Firmware written and verified.')
            assert page.evaluate('window.operations') == ['erase', 'erase', 'flash']
            # Wi-Fi setup opens after a WebRTC flash; closing it frees the port.
            page.get_by_role('dialog', name='Provision WebRTC camera').get_by_role('button', name='Close').click()
            expect(page.get_by_role('dialog', name='Provision WebRTC camera')).to_be_hidden()
            clear_settings.uncheck()
            page.get_by_role('button', name='Flash firmware (keep settings)', exact=True).click()
            page.wait_for_function('window.nvsConnections===5')
            page.evaluate('window.nvsContinue()')
            expect(page.get_by_test_id('flash-status')).to_have_text('Firmware written and verified. Board restarted.')
            assert page.evaluate('window.operations') == ['erase', 'erase', 'flash', 'flash']
            # The popup runs the firmware's own setup page over USB.
            popup = page.get_by_role('dialog', name='Provision WebRTC camera')
            expect(popup).to_be_visible()  # Opens automatically after flashing.
            setup = page.frame_locator('.flasher-provisioning iframe')
            expect(setup.locator('#status')).to_have_text('Choose your network from the list.')
            assert setup.locator('#camera-access').is_visible()
            for setup_width in (320, 390, 768, 1440):
                page.set_viewport_size({'width': setup_width, 'height': 800})
                assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
                box = popup.bounding_box()
                assert box['x'] >= 0 and box['x'] + box['width'] <= setup_width and box['y'] + box['height'] <= 800
                assert popup.get_by_role('button', name='Close').bounding_box()['height'] >= 44
                for field in ('#ssid', '#password', '#save'):
                    assert setup.locator(field).bounding_box()['height'] >= 44
                assert setup.locator('#password').evaluate('input => parseFloat(getComputedStyle(input).fontSize) >= 16')
                assert setup.locator('html').evaluate('root => root.scrollWidth <= root.clientWidth')
            setup.locator('#ssid').select_option(label='Test+network')
            setup.locator('#camera-password').fill('x' * 12)
            setup.get_by_role('button', name='Save and connect').click()
            expect(setup.locator('#status')).to_have_text('Connected. Your settings are saved.')
            local_link = setup.locator('#local-viewer-link')
            expect(local_link).to_have_attribute('href', 'https://192.0.2.1/')
            remote_link = setup.locator('#remote-viewer-link')
            remote_url = 'https://esp32.sensorfun.net/projects/webrtc/A1B2C3D4E5F6'
            expect(remote_link).to_have_attribute('href', remote_url)
            expect(remote_link).to_have_attribute('target', '_blank')
            labels = ['View locally: https://192.0.2.1/', f'View on internet: {remote_url}']
            expect(setup.locator('.viewer-link')).to_have_text(labels)
            expect(page.locator('.flasher-viewer')).to_have_text(labels)
            for width in (320, 390, 768, 1440):
                page.set_viewport_size({'width': width, 'height': 800})
                assert setup.locator('html').evaluate('root => root.scrollWidth <= root.clientWidth')
                assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
                assert local_link.bounding_box()['height'] >= 44
                assert remote_link.bounding_box()['height'] >= 44
            local_link.click()
            expect(popup).to_be_hidden()
            assert page.evaluate('window.openedUrl') == 'https://192.0.2.1/'
            page.get_by_role('button', name='Provision', exact=True).click()
            expect(setup.locator('#status')).to_have_text('Choose your network from the list.')
            change = setup.get_by_role('checkbox', name='Change camera password')
            expect(change).not_to_be_checked()
            expect(setup.locator('#camera-password')).to_be_hidden()
            page.set_viewport_size({'width': 320, 'height': 800})
            assert setup.locator('#camera-password-option').bounding_box()['height'] >= 44
            change.check()
            expect(setup.locator('#camera-password')).to_be_visible()
            assert setup.locator('html').evaluate('root => root.scrollWidth <= root.clientWidth')
            setup.locator('#ssid').select_option(label='Test+network')
            setup.locator('#camera-password').fill('y' * 12)
            setup.get_by_role('button', name='Save and connect').click()
            expect(setup.locator('#status')).to_have_text('Connected. Your settings are saved.')
            popup.get_by_role('button', name='Close').click()
            page.get_by_role('button', name='Provision', exact=True).click()
            expect(setup.locator('#status')).to_have_text('Choose your network from the list.')
            setup.locator('#ssid').select_option(label='Other network…')
            setup.locator('#manual-ssid').fill('Missing')
            setup.get_by_role('button', name='Save and connect').click()
            expect(setup.locator('#status')).to_contain_text('reconnected to its saved network')
            assert setup.locator('#save').is_enabled()
            popup.get_by_role('button', name='Close').click()
            expect(popup).to_be_hidden()
            assert page.get_by_role('button', name='Provision', exact=True).is_enabled()
            # The board must run the project being provisioned.
            provision = page.get_by_role('button', name='Provision', exact=True)
            for reported, message in (('blink', 'This board runs blink firmware, but WebRTC camera is selected.'),
                                      (None, "This board's firmware does not say which project it runs.")):
                page.evaluate('value => { window.testBoardProject = value ?? undefined; }', reported)
                provision.click()
                expect(popup.get_by_role('alert')).to_contain_text(message)
                expect(popup.locator('iframe')).to_be_hidden()
                popup.get_by_role('button', name='Close').click()
                expect(popup).to_be_hidden()
            page.evaluate('delete window.testBoardProject')
            # Provisioning follows the selected project; uploads without a known project have none.
            flasher_tab(page, 'Firmware')
            page.get_by_label('Firmware source', exact=True).select_option('upload')
            flasher_tab(page, 'Flash')
            expect(provision).to_have_count(0)
            flasher_tab(page, 'Firmware')
            page.get_by_label('Firmware source', exact=True).select_option('webrtc')
            flasher_tab(page, 'Flash')
            expect(provision).to_be_visible()
        context.close()

    # Motion detection and recordings: the shared camera_ui elements on the
    # React WebRTC page and on the board's own viewer page, answered by a
    # simulated camera (motion-harness.js); no board or WebRTC connection.
    harness = (Path(__file__).parent / 'motion-harness.js').read_text()
    camera_ui = workspace / 'projects/common/camera_ui'
    viewer_csp = "default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; frame-ancestors 'none'; base-uri 'none'"

    def serve_viewer(route):
        path = route.request.url.split('camera.test', 1)[1]
        if path == '/':
            route.fulfill(body=(workspace / 'projects/webrtc/main/viewer.html').read_text(), content_type='text/html; charset=utf-8', headers={'Content-Security-Policy': viewer_csp})
        elif path in ('/camera-settings.js', '/camera-motion.js'):
            route.fulfill(body=(camera_ui / ('settings.js' if 'settings' in path else 'motion.js')).read_text(), content_type='application/javascript')
        else:
            route.fulfill(status=404, body='{}')

    def check_motion(page, width):
        page.add_script_tag(content=harness)
        page.evaluate('motionHarness.install()')
        page.get_by_role('tab', name='Motion detection', exact=True).click()
        expect(page.get_by_role('tab', name='Motion detection', exact=True)).to_have_attribute('aria-selected', 'true')
        panel = page.locator('sensorfan-motion-settings[group=detection]')
        expect(panel.get_by_role('heading', name='Motion detection', exact=True)).to_be_visible()
        flow = panel.get_by_label('Minimum speed', exact=True)
        expect(flow).to_have_value('2.5')
        expect(panel.get_by_label('Detect motion', exact=True)).to_be_checked()
        columns = panel.locator('.fields').evaluate(
            "grid => getComputedStyle(grid).gridTemplateColumns.split(' ').length"
        )
        if width <= 390:
            assert columns == 1
        if width == 1440:
            assert columns > 1
        for name in ('Method', 'Cell size'):
            dropdown = panel.get_by_role('combobox', name=name, exact=True)
            label = dropdown.locator('..').locator('.title')
            control_box, label_box = dropdown.bounding_box(), label.bounding_box()
            assert control_box['x'] >= label_box['x'] + label_box['width']
            assert abs(control_box['y'] + control_box['height'] / 2 - label_box['y'] - label_box['height'] / 2) <= 1
            assert control_box['height'] >= 44 and control_box['x'] + control_box['width'] <= width
            if width <= 600:
                assert dropdown.evaluate('(select) => parseFloat(getComputedStyle(select).fontSize) >= 16')
        if width <= 600:
            assert flow.evaluate('(input)=>parseFloat(getComputedStyle(input).fontSize)>=16')
        assert panel.locator('label.toggle').first.bounding_box()['height'] >= 44
        for button in panel.get_by_role('button').all():
            assert button.bounding_box()['height'] >= 44
        flow.fill('3.75')
        # Changes apply live, like camera settings; Save also keeps them.
        expect(panel.get_by_text('Live changes. Select Save to keep them after reboot.', exact=True)).to_be_visible()
        live = page.evaluate("motionHarness.state.commands.filter(c=>c.op==='motion_set').at(-1).settings")
        assert live['min_speed'] == 3.75 and live['enabled'] is True, live
        panel.get_by_label('Cell size', exact=True).select_option(label='10 px')
        panel.get_by_role('button', name='Save', exact=True).click()
        expect(panel.get_by_text('Motion settings saved on camera.', exact=True)).to_be_visible()
        applied = page.evaluate("motionHarness.state.commands.filter(c=>c.op==='motion_apply').at(-1).settings")
        assert applied['min_speed'] == 3.75 and applied['cell'] == 10 and applied['method'] == 'optical_flow' and applied['enabled'] is True, applied
        # Recording settings live in the SD card tab, not with detection.
        expect(panel.get_by_label('Pre-record', exact=True)).to_have_count(0)
        expect(panel.get_by_label('Record to SD card', exact=True)).to_have_count(0)
        page.evaluate('motionHarness.status()')
        expect(panel.get_by_text('Motion detected', exact=True)).to_be_visible()
        expect(panel.get_by_text('Recording 0:12', exact=True)).to_have_count(0)
        page.get_by_role('tab', name='SD card recording', exact=True).click()
        recording = page.locator('sensorfan-motion-settings[group=recording]')
        expect(recording.get_by_role('heading', name='Recording settings', exact=True)).to_be_visible()
        expect(recording.get_by_label('Minimum speed', exact=True)).to_have_count(0)
        record = recording.get_by_label('Record to SD card', exact=True)
        expect(record).to_be_checked()
        expect(recording.get_by_text('Recording 0:12', exact=True)).to_be_visible()
        for button in recording.get_by_role('button').all():
            assert button.bounding_box()['height'] >= 44
        pre = recording.get_by_label('Pre-record', exact=True)
        if width <= 600:
            assert pre.evaluate('(input)=>parseFloat(getComputedStyle(input).fontSize)>=16')
        expect(pre).to_have_attribute('max', '4')
        expect(recording.get_by_text(re.compile(r'^Up to 4 s now, limited by free PSRAM'))).to_be_visible()
        pre.fill('6')
        assert not pre.evaluate('(input)=>input.checkValidity()')
        pre.fill('2')
        recording.get_by_role('button', name='Save', exact=True).click()
        expect(recording.get_by_text('Recording settings saved on camera.', exact=True)).to_be_visible()
        applied = page.evaluate("motionHarness.state.commands.filter(c=>c.op==='motion_apply').at(-1).settings")
        assert applied == {'record': True, 'pre_seconds': 2, 'post_seconds': 10}, applied
        warning = recording.get_by_text("No SD card inserted. The camera can't record clips right now.", exact=True)
        expect(warning).to_be_hidden()
        page.evaluate("motionHarness.status({sd: {state: 'absent'}, enabled: false, recording: false})")
        expect(warning).to_be_visible()
        record.uncheck()
        expect(warning).to_be_hidden()
        record.check()
        expect(warning).to_be_visible()
        recording.get_by_role('button', name='Save', exact=True).click()
        expect(recording.get_by_text('Recording settings saved on camera.', exact=True)).to_be_visible()
        expect(warning).to_be_visible()
        page.evaluate('motionHarness.status()')
        expect(warning).to_be_hidden()
        # Reset restores only this panel's fields; detection keeps its saved values.
        recording.get_by_label('Keep recording', exact=True).fill('30')
        recording.get_by_role('button', name='Reset to Default', exact=True).click()
        expect(recording.get_by_label('Keep recording', exact=True)).to_have_value('10')
        assert page.evaluate("motionHarness.state.values.min_speed") == 3.75
        assert page.evaluate("motionHarness.state.commands.filter(c=>c.op==='motion_reset').length") == 0
        # A resolution change lowers the camera's limit; the field follows it.
        page.evaluate('motionHarness.status({limits: {pre_seconds: 1}})')
        expect(pre).to_have_attribute('max', '1')
        expect(pre).to_have_value('1')
        expect(recording.get_by_text(re.compile(r'^Up to 1 s now'))).to_be_visible()
        overlay = page.locator('sensorfan-motion-overlay')
        expect(overlay.get_by_text('MOTION', exact=True)).to_be_visible()
        preview = page.locator('[data-testid=remote-image], #preview').first.bounding_box()
        box = overlay.bounding_box()
        assert all(abs(box[key] - preview[key]) <= 1 for key in ('x', 'y', 'width', 'height')), (box, preview)
        assert overlay.evaluate("(el)=>{const c=el.shadowRoot.querySelector('canvas');return c.getContext('2d').getImageData(0,0,c.width,c.height).data.some((v,i)=>i%4===3&&v>0)}")
        page.get_by_role('tab', name='SD card recording', exact=True).click()
        expect(panel).not_to_be_visible()
        recordings = page.locator('sensorfan-recordings')
        expect(recordings.get_by_text(re.compile(r'SD card · 28\.32 GB free of 29\.72 GB'))).to_be_visible()
        expect(recordings.locator('li')).to_have_count(2)
        expect(recordings.locator('li').first).to_contain_text('0:30 · 1280 × 720 · 17.6 MB')
        recordings.locator('li').nth(1).get_by_role('button', name=re.compile('^Play ')).click()
        player = recordings.locator('.player img')
        expect(player).to_be_visible()
        # Playback starts with the first frames, while the rest is still loading.
        page.wait_for_function("()=>{const r=document.querySelector('sensorfan-recordings');const i=r.shadowRoot.querySelector('.player img');return i&&i.complete&&i.naturalWidth===320}")
        expect(recordings.locator('.transfer')).to_be_visible()
        expect(recordings.locator('.transfer-text')).to_contain_text('Loading clip')
        expect(recordings.locator('.transfer')).to_be_hidden(timeout=15000)
        expect(recordings.locator('.player .time')).to_contain_text('/ 0:03')
        with page.expect_download() as download:
            recordings.get_by_role('button', name='Download', exact=True).click()
        assert re.fullmatch(r'camera-2026-11-1\d_\d\d-\d\d-00\.avi', download.value.suggested_filename), download.value.suggested_filename
        assert page.evaluate("motionHarness.state.commands.filter(c=>c.op==='recording_download').length") == 1  # Download reused the played clip.
        if width == 390:
            page.evaluate("""() => {
                window.screenLocks = { acquired: 0, released: 0 };
                Object.defineProperty(navigator, 'wakeLock', { configurable: true, value: {
                    async request(type) {
                        if (type !== 'screen') throw Error('Expected a screen wake lock');
                        screenLocks.acquired++;
                        return { released: false, async release() {
                            if (!this.released) { this.released = true; screenLocks.released++; }
                        } };
                    },
                } });
            }""")
            download_clip = recordings.locator('li').first.get_by_role('button', name=re.compile('^Download '))
            download_clip.click()
            page.wait_for_function('screenLocks.acquired === 1 && screenLocks.released === 0')
            recordings.get_by_role('button', name='Cancel', exact=True).click()
            page.wait_for_function('screenLocks.released === 1')
            with page.expect_download():
                download_clip.click()
            page.wait_for_function('screenLocks.acquired === 2 && screenLocks.released === 2')
        page.once('dialog', lambda dialog: dialog.accept())
        recordings.locator('li').first.get_by_role('button', name=re.compile('^Delete ')).click()
        expect(recordings.locator('li')).to_have_count(1)
        # Formatting asks again in a modal dialog; Cancel sends nothing.
        format_button = recordings.get_by_role('button', name='Format SD card', exact=True)
        assert format_button.bounding_box()['height'] >= 44
        format_button.click()
        dialog = recordings.get_by_role('dialog', name='Format the SD card?')
        expect(dialog).to_be_visible()
        expect(dialog.get_by_role('button', name='Cancel', exact=True)).to_be_focused()
        box = dialog.bounding_box()
        assert box['x'] >= 0 and box['x'] + box['width'] <= width, box
        dialog.get_by_role('button', name='Cancel', exact=True).click()
        expect(dialog).to_be_hidden()
        assert page.evaluate("motionHarness.state.commands.filter(c=>c.op==='sd_format').length") == 0
        format_button = recordings.get_by_role('button', name='Format SD card', exact=True)
        format_button.click()
        recordings.get_by_role('dialog', name='Format the SD card?').get_by_role('button', name='Erase and format', exact=True).click()
        expect(recordings.get_by_role('button', name='Format SD card', exact=True)).to_be_disabled()
        expect(recordings.get_by_text('SD card formatted', exact=True)).to_be_visible()
        assert page.evaluate("motionHarness.state.commands.filter(c=>c.op==='sd_format').length") == 1
        expect(recordings.locator('li')).to_have_count(1)
        expect(recordings.locator('li')).to_contain_text('No recordings yet')
        expect(recordings.get_by_role('button', name='Format SD card', exact=True)).to_be_enabled()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')

    for width in (320, 390, 768, 1440):
        context = browser.new_context(viewport={'width': width, 'height': 900}, accept_downloads=True)
        page = context.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        # The remote link from Wi-Fi setup fills in the camera; only the password is left.
        page.goto(urljoin(args.url.split('#')[0], 'projects/webrtc/a1b2c3d4e5f6'))
        expect(page.get_by_label('Camera MAC address', exact=True)).to_have_value('A1:B2:C3:D4:E5:F6')
        page.get_by_label('Camera password', exact=True).fill('x' * 12)
        expect(page.get_by_role('button', name='Connect camera', exact=True)).to_be_enabled()
        page.goto(urljoin(args.url.split('#')[0], 'projects/webrtc'))
        expect(page.get_by_role('heading', name='Remote camera', exact=True)).to_be_visible()
        check_motion(page, width)
        page.route('https://camera.test/**', serve_viewer)
        page.goto('https://camera.test/')
        expect(page).to_have_title('Local WebRTC camera')
        expect(page.get_by_role('button', name='Connect', exact=True)).to_be_enabled()
        check_motion(page, width)
        context.close()
    assert not errors, errors
    print('Passed: SensorFun ESP32 branding, logo, project list, breadcrumbs, flasher, remote-only camera interface, motion detection and recordings, and desktop/mobile layouts.')
    browser.close()
