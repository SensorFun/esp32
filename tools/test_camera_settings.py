"""Capture settings or verify live changes and durable Save/Reset on a USB camera.

Settings snapshots contain no Wi-Fi credentials, passwords, or camera images.
"""
import argparse
import json
import random
import secrets
from contextlib import ExitStack
from pathlib import Path

from playwright.sync_api import sync_playwright
from test_local_webrtc import identity, wait_js

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True)
parser.add_argument('--port')
parser.add_argument('--browser', help='Chromium executable; otherwise use Playwright Chromium')
parser.add_argument('--snapshot', type=Path)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--seed', type=int, help='Replay a reported random seed')
parser.add_argument('--capture', action='store_true')
args = parser.parse_args()
if args.rounds < 1 or (args.capture and args.snapshot is None) or (not args.capture and not args.port):
    parser.error('Use positive rounds, --snapshot for capture, and --port for reboot tests')


def values(data):
    return dict(resolution=data['resolution'], timing_profile=data['timing_profile'],
                **{c['name']: c['value'] for c in data['controls'] if c['supported']})


with sync_playwright() as p, ExitStack() as serial_handles:
    usb = []
    browser = p.chromium.launch(executable_path=args.browser, headless=True, args=['--no-sandbox', '--ignore-certificate-errors'])

    def connect():
        page = browser.new_page(ignore_https_errors=True, viewport={'width': 375, 'height': 844})
        page.goto(args.url, wait_until='domcontentloaded')
        wait_js(page, "!!customElements.get('sensorfan-camera-settings')")
        page.locator('#connect').click()
        wait_js(page, "channel?.readyState==='open'", timeout=40000)
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
            original = json.loads(args.snapshot.read_text())
            apply(page, original)
        seed = args.seed if args.seed is not None else secrets.randbits(64)
        rng = random.Random(seed)
        print(f'Random settings seed: {seed}', flush=True)

        def edit(settings):
            before = values(page.evaluate('window.snapshot'))
            controls = page.evaluate('window.snapshot.controls')
            page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(settings['resolution']))
            wait_js(page, "v=>window.snapshot?.resolution===v", arg=settings['resolution'])
            page.get_by_role('combobox', name='Light timing', exact=True).select_option(settings['timing_profile'])
            wait_js(page, "v=>window.snapshot?.timing_profile===v", arg=settings['timing_profile'])
            for control in controls:
                if control['supported'] and control['name'] in settings and settings[control['name']] != before[control['name']]:
                    field = page.get_by_role('spinbutton', name=control['name'].replace('_', ' '), exact=True)
                    field.fill('')
                    field.fill(str(settings[control['name']]))
                    try:
                        wait_js(page, "v=>window.snapshot?.controls?.find(c=>c.name===v.name)?.value===v.value", arg={'name': control['name'], 'value': settings[control['name']]})
                    except TimeoutError as error:
                        print('Live change diagnostics:', page.evaluate("name=>({state:channel?.readyState,status:document.querySelector('#status').textContent,commands:window.commands.slice(-6),reported:window.snapshot?.controls?.find(c=>c.name===name)})", control['name']), flush=True)
                        raise AssertionError(f"Live change not acknowledged: {control['name']}={settings[control['name']]}") from error
            page.evaluate("window.snapshot=null;command({op:'get'})")
            try:
                wait_js(page, "expected=>{const s=window.snapshot;if(s?.type!=='settings')return false;const actual={resolution:s.resolution,timing_profile:s.timing_profile,...Object.fromEntries(s.controls.filter(c=>c.supported).map(c=>[c.name,c.value]))};return Object.entries(expected).every(([key,value])=>actual[key]===value)}", arg=settings)
            except TimeoutError:
                actual = page.evaluate('window.snapshot')
                if actual and actual.get('type') == 'settings':
                    reported = values(actual)
                    print('Settings mismatch (requested, reported):', {k: (v, reported.get(k)) for k, v in settings.items() if reported.get(k) != v}, flush=True)
                raise
            assert values(page.evaluate('window.snapshot')) == settings, 'Live settings differ from requested values'

        try:
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
                    low, high = control['min'], control['max']
                    if control['name'] == 'jpeg_quality':
                        low, high = max(low, 8), min(high, 25)
                    choices = [v for v in range(low, high + 1) if v != current[control['name']]]
                    expected[control['name']] = rng.choice(choices)
                sizes = [s['value'] for s in metadata['resolutions'] if s['width'] <= 640 and s['height'] <= 480 and s['value'] != current['resolution']]
                expected['resolution'] = rng.choice(sizes)
                expected['timing_profile'] = rng.choice([v for v in metadata['timing_profiles'] if v != current['timing_profile']])
                edit(expected)
                if iteration == 0:
                    close(page); reboot(); page = connect()
                    assert values(page.evaluate('window.snapshot')) == current, 'Unsaved changes survived reboot'
                    edit(expected)
                page.evaluate('window.snapshot=null')
                page.get_by_role('button', name='Save', exact=True).nth(iteration % 2).click()
                wait_js(page, "window.snapshot?.saved===true", timeout=20000)
                assert values(page.evaluate('window.snapshot')) == expected, 'Save acknowledgement differs from requested settings'
                assert page.get_by_role('button', name='Save', exact=True).count() == 2
                assert page.get_by_role('button', name='Reset to Default', exact=True).count() == 2
                assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
                close(page); reboot(); page = connect()
                assert values(page.evaluate('window.snapshot')) == expected, 'Saved random settings did not survive reboot'
                print(f'PASS: randomized round {iteration + 1}/{args.rounds}: multiple controls, resolution and timing survived reboot', flush=True)
            page.evaluate('window.snapshot=null')
            page.get_by_role('button', name='Reset to Default', exact=True).last.click()
            wait_js(page, "window.snapshot?.saved===true", timeout=20000)
            defaults = values(page.evaluate('window.snapshot'))
            close(page); reboot(); page = connect()
            assert values(page.evaluate('window.snapshot')) == defaults, 'Defaults did not survive reboot'
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
        print('PASS: random Save/reboot, unsaved reboot, Reset/reboot, both action rows, mobile panel, original settings restored.')
    browser.close()
