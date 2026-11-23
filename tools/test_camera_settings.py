"""Capture settings or verify live changes, live reset and durable saves on a USB camera.

Settings snapshots contain no Wi-Fi credentials, passwords, or camera images.
"""
import argparse
import json
import random
import secrets
from contextlib import ExitStack
from pathlib import Path

from playwright.sync_api import sync_playwright
from camera_panel import allowed_values, set_control
from test_local_webrtc import add_password_argument, camera_password, connect_local, identity, wait_js

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True)
parser.add_argument('--port')
parser.add_argument('--browser', help='Chromium executable; otherwise use Playwright Chromium')
parser.add_argument('--snapshot', type=Path)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--seed', type=int, help='Replay a reported random seed')
parser.add_argument('--capture', action='store_true')
add_password_argument(parser)
args = parser.parse_args()
if args.rounds < 1 or (args.capture and args.snapshot is None) or (not args.capture and not args.port):
    parser.error('Use positive rounds, --snapshot for capture, and --port for reboot tests')
password = camera_password(args)


def values(data):
    settings = {'resolution': data['resolution']}
    if 'stream_images' in data:
        settings['stream_images'] = data['stream_images']
    if data['timing_profiles']:
        settings['timing_profile'] = data['timing_profile']
    settings.update({c['name']: c['value'] for c in data['controls'] if c['supported']})
    return settings


with sync_playwright() as p, ExitStack() as serial_handles:
    usb = []
    browser = p.chromium.launch(executable_path=args.browser, headless=True, args=['--no-sandbox', '--ignore-certificate-errors'])

    def connect():
        page = browser.new_page(ignore_https_errors=True, viewport={'width': 375, 'height': 844})
        page.goto(args.url, wait_until='domcontentloaded')
        wait_js(page, "!!customElements.get('sensorfan-camera-settings')")
        connect_local(page, password)
        page.evaluate("window.snapshot=null;channel.addEventListener('message',e=>{if(typeof e.data==='string'){const message=JSON.parse(e.data);if(['settings','error'].includes(message.type))window.snapshot=message;}});command({op:'get'})")
        wait_js(page, "window.snapshot?.type==='settings'")
        page.evaluate("()=>{window.commands=[];const send=channel.send.bind(channel);channel.send=text=>{const c=JSON.parse(text);if(c.op!=='ping')window.commands.push(c);send(text)}}")
        return page

    def close(page):
        page.locator('#disconnect').click()
        wait_js(page, "!document.querySelector('#connect').disabled")
        page.close()

    def reboot():
        if usb:
            usb.pop().close()
        url, _, console = identity(args.port)
        args.url = url
        # Keep the adapter attached throughout the viewer session. Closing it
        # can change DTR/RTS, and would also discard all runtime diagnostics.
        usb.append(console)
        serial_handles.callback(console.close)

    def apply(page, settings):
        page.evaluate("settings=>{window.snapshot=null;command({op:'apply',settings})}", settings)
        wait_js(page, "window.snapshot?.saved===true", timeout=20000)
        assert values(page.evaluate('window.snapshot')) == settings

    def check_manual_recording(page):
        page.evaluate("""() => channel.addEventListener('message', ({data}) => {
            if (typeof data !== 'string') return;
            const reply = JSON.parse(data);
            if (reply.type === 'motion_status') window.motionStatus = reply;
            if (reply.type === 'motion') window.motionValues = reply.values;
        })""")
        wait_js(page, 'Boolean(window.motionStatus)')
        if page.evaluate("window.motionStatus.sd?.state") != 'mounted':
            print('SKIP: manual recording requires a mounted SD card', flush=True)
            return
        page.evaluate("command({op:'motion_get'})")
        wait_js(page, 'Boolean(window.motionValues)')
        motion = page.evaluate('({enabled:window.motionValues.enabled, record:window.motionValues.record})')
        size = page.evaluate('window.snapshot.resolutions.find(size=>size.value===window.snapshot.resolution)')
        page.evaluate("command({op:'motion_set',settings:{enabled:true,record:false}})")
        clip = None
        page.get_by_role('tab', name='SD card recording').click()
        try:
            page.get_by_role('button', name='Start recording', exact=True).click()
            wait_js(page, """window.motionStatus?.manual_recording==='active' && window.motionStatus.enabled &&
                window.motionStatus.analysis_suspended && window.motionStatus.fps===0 && window.motionStatus.clip_frames>=2""", timeout=20000)
            clip = page.evaluate('window.motionStatus.clip')
            assert not page.locator('#preview').get_attribute('src'), 'Manual recording enabled the live preview'
            page.get_by_role('button', name='Stop recording', exact=True).click()
            wait_js(page, "window.motionStatus?.manual_recording==='idle' && !window.motionStatus.analysis_suspended")
            wait_js(page, "name=>document.querySelector('#recordings').list?.files?.some(file=>file.name===name && file.complete)", arg=clip)
            result = page.evaluate("name=>document.querySelector('#recordings').list.files.find(file=>file.name===name)", clip)
            assert (result['width'], result['height']) == (size['width'], size['height'])
            print('PASS: manual clip used the active resolution with live video and motion analysis off', flush=True)
        finally:
            page.evaluate("command({op:'recording_stop'})")
            page.evaluate("settings=>command({op:'motion_set',settings})", motion)
            if clip:
                page.evaluate("name=>command({op:'recording_delete',name})", clip)
            page.get_by_role('tab', name='Camera settings').click()

    if args.port and not args.capture:
        reboot()
    page = connect()
    if args.capture:
        args.snapshot.parent.mkdir(parents=True, exist_ok=True)
        args.snapshot.write_text(json.dumps(values(page.evaluate('window.snapshot'))))
        args.snapshot.chmod(0o600)
        close(page)
        print('Captured existing camera settings without credentials or images.')
    else:
        original = values(page.evaluate('window.snapshot'))
        if args.snapshot:
            original.update(json.loads(args.snapshot.read_text()))
            apply(page, original)
        seed = args.seed if args.seed is not None else secrets.randbits(64)
        rng = random.Random(seed)
        print(f'Random settings seed: {seed}', flush=True)

        def edit(settings):
            before = values(page.evaluate('window.snapshot'))
            controls = page.evaluate('window.snapshot.controls')
            page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(settings['resolution']))
            wait_js(page, "v=>window.snapshot?.resolution===v", arg=settings['resolution'])
            if 'timing_profile' in settings:
                page.get_by_role('combobox', name='Light timing', exact=True).select_option(settings['timing_profile'])
                wait_js(page, "v=>window.snapshot?.timing_profile===v", arg=settings['timing_profile'])
            for control in controls:
                if control['supported'] and control['name'] in settings and settings[control['name']] != before[control['name']]:
                    set_control(page.locator('#settings-panel'), control, settings[control['name']])
                    try:
                        wait_js(page, "v=>window.snapshot?.controls?.find(c=>c.name===v.name)?.value===v.value", arg={'name': control['name'], 'value': settings[control['name']]})
                    except TimeoutError as error:
                        print('Live change diagnostics:', page.evaluate("name=>({state:channel?.readyState,status:document.querySelector('#status').textContent,commands:window.commands.slice(-6),reported:window.snapshot?.controls?.find(c=>c.name===name)})", control['name']), flush=True)
                        raise AssertionError(f"Live change not acknowledged: {control['name']}={settings[control['name']]}") from error
            page.evaluate("window.snapshot=null;command({op:'get'})")
            try:
                wait_js(page, "expected=>{const s=window.snapshot;if(s?.type!=='settings')return false;const actual={resolution:s.resolution,stream_images:s.stream_images,timing_profile:s.timing_profile,...Object.fromEntries(s.controls.filter(c=>c.supported).map(c=>[c.name,c.value]))};return Object.entries(expected).every(([key,value])=>actual[key]===value)}", arg=settings)
            except TimeoutError:
                actual = page.evaluate('window.snapshot')
                if actual and actual.get('type') == 'settings':
                    reported = values(actual)
                    print('Settings mismatch (requested, reported):', {k: (v, reported.get(k)) for k, v in settings.items() if reported.get(k) != v}, flush=True)
                raise
            assert values(page.evaluate('window.snapshot')) == settings, 'Live settings differ from requested values'

        try:
            panel = page.locator('#settings-panel')
            stream = panel.get_by_role('checkbox', name='Stream camera images', exact=True)
            stream.check()
            wait_js(page, "window.snapshot?.stream_images===1 && document.querySelector('#preview').naturalWidth>0")
            stream.uncheck()
            wait_js(page, "window.snapshot?.stream_images===0 && !document.querySelector('#preview').hasAttribute('src')")
            page.evaluate("""() => {
                window.imagePackets = 0;
                channel.addEventListener('message', ({data}) => {
                    if (data instanceof ArrayBuffer && data.byteLength >= 16 && new DataView(data).getUint32(0, true) === 0x4745504a)
                        window.imagePackets++;
                });
            }""")
            panel.get_by_role('button', name='Save', exact=True).click()
            wait_js(page, "window.snapshot?.saved===true && window.snapshot.stream_images===0")
            page.wait_for_timeout(1000)
            assert page.evaluate('window.imagePackets') == 0, 'Camera sent images after streaming was turned off'
            close(page); reboot(); page = connect()
            panel = page.locator('#settings-panel')
            assert not panel.get_by_role('checkbox', name='Stream camera images', exact=True).is_checked()
            assert page.evaluate('window.snapshot.stream_images') == 0, 'Streaming preference did not survive reboot'
            check_manual_recording(page)
            panel.get_by_role('button', name='Reset to Default', exact=True).click()
            wait_js(page, "window.snapshot?.reset===true && window.snapshot.stream_images===1 && document.querySelector('#preview').naturalWidth>0", timeout=20000)
            apply(page, original)
            print('PASS: streaming stops live, stays off after reboot, and defaults resume the preview', flush=True)

            for iteration in range(args.rounds):
                metadata = page.evaluate('window.snapshot')
                current = values(metadata)
                expected = dict(current)
                # Independent image controls avoid automatic exposure/white-balance
                # dependencies; unsupported controls are never selected.
                independent = {'brightness', 'contrast', 'saturation', 'horizontal_mirror',
                               'vertical_flip', 'colorbar', 'jpeg_quality', 'sharpness', 'denoise'}
                candidates = [c for c in metadata['controls'] if c['supported'] and c['name'] in independent and c['min'] < c['max']]
                assert len(candidates) >= 5, 'Need at least five supported controls'
                for control in rng.sample(candidates, rng.randint(5, len(candidates))):
                    choices = allowed_values(control)
                    if control['name'] == 'jpeg_quality':
                        choices = [v for v in choices if 8 <= v <= 25]
                    choices = [v for v in choices if v != current[control['name']]]
                    expected[control['name']] = rng.choice(choices)
                sizes = [s['value'] for s in metadata['resolutions'] if s['width'] <= 640 and s['height'] <= 480 and s['value'] != current['resolution']]
                expected['resolution'] = rng.choice(sizes)
                if metadata['timing_profiles']:
                    expected['timing_profile'] = rng.choice([v for v in metadata['timing_profiles'] if v != current['timing_profile']])
                edit(expected)
                if iteration == 0:
                    close(page); reboot(); page = connect()
                    assert values(page.evaluate('window.snapshot')) == current, 'Unsaved changes survived reboot'
                    edit(expected)
                page.evaluate('window.snapshot=null')
                page.locator('#settings-panel').get_by_role('button', name='Save', exact=True).click()
                wait_js(page, "window.snapshot?.saved===true", timeout=20000)
                assert values(page.evaluate('window.snapshot')) == expected, 'Save acknowledgement differs from requested settings'
                assert page.locator('#settings-panel').get_by_role('button', name='Save', exact=True).count() == 1
                assert page.locator('#settings-panel').get_by_role('button', name='Reset to Default', exact=True).count() == 1
                assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
                close(page); reboot(); page = connect()
                assert values(page.evaluate('window.snapshot')) == expected, 'Saved random settings did not survive reboot'
                print(f'PASS: randomized round {iteration + 1}/{args.rounds}: randomized settings survived reboot', flush=True)
            page.evaluate('window.snapshot=null')
            page.get_by_role('button', name='Reset to Default', exact=True).last.click()
            wait_js(page, "window.snapshot?.reset===true && window.snapshot?.saved!==true", timeout=20000)
            defaults = values(page.evaluate('window.snapshot'))
            page.evaluate("""() => {
                window.framesAfterReset = 0;
                document.querySelector('#preview').addEventListener('load', () => window.framesAfterReset++);
            }""")
            wait_js(page, 'window.framesAfterReset >= 2', timeout=20000)
            close(page); reboot(); page = connect()
            assert values(page.evaluate('window.snapshot')) == expected, 'Reset overwrote the saved settings'
            page.get_by_role('button', name='Reset to Default', exact=True).first.click()
            wait_js(page, "window.snapshot?.reset===true", timeout=20000)
            assert values(page.evaluate('window.snapshot')) == defaults
            page.get_by_role('button', name='Save', exact=True).first.click()
            wait_js(page, "window.snapshot?.saved===true", timeout=20000)
            close(page); reboot(); page = connect()
            assert values(page.evaluate('window.snapshot')) == defaults, 'Saved defaults did not survive reboot'
        finally:
            # A failed command can leave the channel unusable. Reboot before
            # cleanup so restoration is independent of the failing session.
            if not page.is_closed():
                page.close()
            reboot()
            page = connect()
            apply(page, original)
            close(page); reboot(); page = connect()
            assert values(page.evaluate('window.snapshot')) == original, 'Original settings were not restored'
            close(page)
        print('PASS: randomized saves, live defaults, reset without saving, saved defaults across reboot, original settings restored.')
    browser.close()
