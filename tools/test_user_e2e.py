#!/usr/bin/env python3
"""User-level end-to-end check of the board's own web page.

Opens the board's HTTPS page in a separate Chromium process and works only
through the visible UI: Connect, the camera settings controls, the motion
panel, the SD card tab, Save, Disconnect, reconnect and page reloads. After
every action the live preview must keep updating, with no gap between frames
longer than the step allows. The USB log is watched for resets, aborts,
watchdogs and dropped peers. The starting camera and motion settings are
restored at the end. Credentials and images are never written to disk.
"""
import argparse
import sys
import threading
import time

from playwright.sync_api import sync_playwright
from test_local_webrtc import identity, wait_js

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True, help='USB serial port of the board')
parser.add_argument('--browser', required=True, help='Installed Chromium executable')
parser.add_argument('--headed', action='store_true', help='Show the browser window (needs a display)')
parser.add_argument('--watch', type=float, default=12, help='Seconds of streaming checked after each action')
parser.add_argument('--usb-log', help='Also write the full USB log to this file')
args = parser.parse_args()

# Frame gaps a user would still call a live stream. Large JPEGs take longer
# to capture and send, so big resolutions get more slack.
def allowed_gap(width):
    return 3.0 if width <= 800 else 6.0

FAULTS = ('rst:', 'abort()', 'guru meditation', 'task_wdt', 'assert failed', 'stack canary', 'corrupt heap',
          'peer disconnected', 'closing camera peer', 'stalled camera', 'esp_error_check')
url, _, console = identity(args.port)
console.runtime_faults = []
expected_disconnect = [False]
usb_events = []
_drain = console.diagnostic_thread
console.drain_stop.set(); _drain.join(2)
console.drain_stop.clear()


usb_log = open(args.usb_log, 'w') if args.usb_log else None


def watch_usb():
    while console.is_open and not console.drain_stop.is_set():
        try:
            line = console.readline().decode(errors='replace').strip()
        except Exception:
            return
        if usb_log and line:
            usb_log.write(f'{time.strftime("%H:%M:%S")} {line}\n'); usb_log.flush()
        lowered = line.lower()
        if any(term in lowered for term in FAULTS):
            if expected_disconnect[0] and ('peer disconnected' in lowered or 'closing camera peer' in lowered):
                continue
            usb_events.append(line)


threading.Thread(target=watch_usb, daemon=True).start()

failures, results = [], []


class Abort(Exception):
    pass
step_name = ['start']


def note(message):
    print(f'{time.strftime("%H:%M:%S")} {message}', flush=True)
    if usb_log:
        usb_log.write(f'### {time.strftime("%H:%M:%S")} {message}\n'); usb_log.flush()


def fail(message):
    failures.append(f'{step_name[0]}: {message}')
    note(f'FAIL {message}')


INSTRUMENT = """() => {
  if (window.e2e) return;
  window.e2e = {frames: [], messages: {}};
  const preview = document.querySelector('#preview');
  preview.addEventListener('load', () => e2e.frames.push({t: performance.now(), w: preview.naturalWidth, h: preview.naturalHeight}));
}"""

TAP_CHANNEL = """() => {
  if (!channel || channel.e2eTapped) return false;
  channel.e2eTapped = true;
  window.e2eChannel = channel;
  channel.addEventListener('message', event => {
    if (typeof event.data === 'string') { try { const m = JSON.parse(event.data); e2e.messages[m.type] = m; } catch {} }
  });
  return true;
}"""


def connect(page, label):
    step_name[0] = label
    page.evaluate(INSTRUMENT)
    wait_js(page, "!document.querySelector('#connect').disabled", timeout=35000)
    start = time.monotonic()
    page.get_by_role('button', name='Connect', exact=True).click()
    try:
        wait_js(page, "channel?.readyState==='open'", timeout=30000)
        page.evaluate(TAP_CHANNEL)
        page.evaluate('e2e.frames=[]')
        wait_js(page, 'e2e.frames.length>0', timeout=10000)
        note(f'{label}: first frame {time.monotonic() - start:.1f} s after Connect')
        return True
    except Exception:
        fail(f'no live frame within 10 s of the channel opening (status: {page.locator("#status").inner_text()})')
        return False


def stream_check(page, seconds=None, gap=None):
    """Watches the preview; returns (frames, worst gap) and records failures."""
    seconds = seconds or args.watch
    channel_before = page.evaluate('window.e2eChannel')
    page.evaluate('e2e.frames=[]; e2e.watchStart=performance.now()')
    page.wait_for_timeout(int(seconds * 1000))
    data = page.evaluate("""() => {
      const times = [e2e.watchStart, ...e2e.frames.map(f => f.t), performance.now()];
      let worst = 0; for (let i = 1; i < times.length; i++) worst = Math.max(worst, times[i] - times[i - 1]);
      const last = e2e.frames.at(-1);
      return {count: e2e.frames.length, worst: worst / 1000, w: last?.w || 0, h: last?.h || 0,
              open: channel?.readyState === 'open', same: channel === window.e2eChannel};
    }""")
    limit = gap or allowed_gap(data['w'] or 320)
    fps = data['count'] / seconds
    results.append((step_name[0], data['w'], data['h'], fps, data['worst']))
    note(f'{step_name[0]}: {data["count"]} frames in {seconds:.0f} s ({fps:.1f} fps), longest gap {data["worst"]:.1f} s, '
         f'{data["w"]}x{data["h"]}')
    if not data['open'] or not data['same']:
        fail('the camera connection dropped')
    elif data['count'] == 0:
        fail('no frames: the stream stopped')
    elif data['worst'] > limit:
        fail(f'stream froze for {data["worst"]:.1f} s (allowed {limit:.0f} s)')
    if usb_events:
        fail('board log: ' + ' | '.join(usb_events[:5]))
        usb_events.clear()
    return data


def wait_reply(page, kind, expression, timeout=15000):
    try:
        wait_js(page, expression, timeout=timeout)
        return True
    except Exception:
        fail(f'no {kind} reply from the camera within {timeout // 1000} s')
        return False


with console, sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, headless=not args.headed,
                                args=['--no-sandbox', '--ignore-certificate-errors'])
    context = browser.new_context(ignore_https_errors=True, viewport={'width': 390, 'height': 844}, is_mobile=True, has_touch=True)
    page = context.new_page()
    original_camera = original_motion = None
    try:
        # A user opens the page; Connect must be there every time.
        for attempt in range(3):
            step_name[0] = f'page load {attempt + 1}'
            page.goto(url, wait_until='load', timeout=30000)
            connect_button = page.get_by_role('button', name='Connect', exact=True)
            try:
                connect_button.wait_for(state='visible', timeout=10000)
                note(f'{step_name[0]}: Connect button visible')
            except Exception:
                fail('the Connect button did not appear')

        if not connect(page, 'connect'):
            raise Abort
        page.evaluate("()=>{e2e.messages={};command({op:'get'});command({op:'motion_get'})}")
        wait_js(page, 'e2e.messages.settings && e2e.messages.motion', timeout=15000)
        metadata = page.evaluate('e2e.messages.settings')
        original_motion = page.evaluate('e2e.messages.motion.values')
        original_camera = dict(resolution=metadata['resolution'], timing_profile=metadata['timing_profile'],
                               **{c['name']: c['value'] for c in metadata['controls'] if c['supported']})
        step_name[0] = 'streaming after connect'
        stream_check(page)

        page.get_by_role('tab', name='Camera settings', exact=True).click()
        sizes = {(s['width'], s['height']): s['value'] for s in metadata['resolutions']}
        for width, height in [(320, 240), (640, 480), (800, 600), (1280, 720), (1600, 1200), (320, 240)]:
            if (width, height) not in sizes:
                continue
            step_name[0] = f'resolution {width}x{height}'
            page.evaluate('e2e.messages.settings=null')
            page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(sizes[(width, height)]))
            if wait_reply(page, 'settings', f"e2e.messages.settings?.resolution==={sizes[(width, height)]}"):
                try:
                    wait_js(page, f'e2e.frames.some(f=>f.w==={width}&&f.h==={height})', timeout=15000)
                except Exception:
                    fail(f'no {width}x{height} frame within 15 s')
            stream_check(page)

        for profile in metadata['timing_profiles']:
            step_name[0] = f'light timing {profile}'
            page.evaluate('e2e.messages.settings=null')
            page.get_by_role('combobox', name='Light timing', exact=True).select_option(profile)
            wait_reply(page, 'settings', f"e2e.messages.settings?.timing_profile==='{profile}'")
            stream_check(page, seconds=6)

        for name, value in (('brightness', 1), ('contrast', -1), ('jpeg quality', 14)):
            step_name[0] = f'{name} {value}'
            page.evaluate('e2e.messages.settings=null')
            box = page.get_by_role('spinbutton', name=name, exact=True)
            box.fill(str(value)); box.press('Tab')
            wait_reply(page, 'settings', f"e2e.messages.settings?.controls.find(c=>c.name==='{name.replace(' ', '_')}')?.value==={value}")
            stream_check(page, seconds=6)

        step_name[0] = 'save camera settings'
        page.evaluate('e2e.messages.settings=null')
        page.get_by_role('button', name='Save', exact=True).first.click()
        wait_reply(page, 'save', 'e2e.messages.settings?.saved===true')
        stream_check(page, seconds=6)

        page.get_by_role('tab', name='Motion detection', exact=True).click()
        panel = page.locator('sensorfan-motion-settings')
        for enabled in (False, True):
            step_name[0] = f'motion detection {"on" if enabled else "off"}'
            box = panel.get_by_label('Detect motion', exact=True)
            box.check() if enabled else box.uncheck()
            page.evaluate('e2e.messages.motion=null')
            panel.get_by_role('button', name='Save', exact=True).click()
            wait_reply(page, 'motion save', 'e2e.messages.motion?.saved===true')
            stream_check(page, seconds=8)
        step_name[0] = 'minimum speed 3.75'
        panel.get_by_label('Minimum speed', exact=True).fill('3.75')
        page.evaluate('e2e.messages.motion=null')
        panel.get_by_role('button', name='Save', exact=True).click()
        wait_reply(page, 'motion save', 'e2e.messages.motion?.saved===true')
        stream_check(page, seconds=8)

        step_name[0] = 'SD card tab'
        page.get_by_role('tab', name='SD card recording', exact=True).click()
        try:
            page.locator('sensorfan-recordings').get_by_role('heading').first.wait_for(state='visible', timeout=10000)
        except Exception:
            fail('the SD card panel did not appear')
        stream_check(page, seconds=6)

        for cycle in range(3):
            step_name[0] = f'disconnect {cycle + 1}'
            expected_disconnect[0] = True
            page.get_by_role('button', name='Disconnect', exact=True).click()
            page.wait_for_timeout(1500)
            expected_disconnect[0] = False
            if connect(page, f'reconnect {cycle + 1}'):
                stream_check(page)

        step_name[0] = 'page reload'
        expected_disconnect[0] = True
        page.reload(wait_until='load')
        page.wait_for_timeout(1500)
        expected_disconnect[0] = False
        if connect(page, 'connect after reload'):
            stream_check(page)
    except Abort:
        pass
    except Exception as error:
        fail(f'test stopped: {type(error).__name__}: {error}')
    finally:
        step_name[0] = 'restore settings'
        try:
            if page.evaluate("typeof channel==='undefined' || channel?.readyState!=='open'"):
                connect(page, 'restore connect')
            page.evaluate(TAP_CHANNEL)
            if original_motion:
                page.evaluate("s=>{e2e.messages.motion=null;command({op:'motion_apply',settings:s})}", original_motion)
                wait_reply(page, 'motion restore', 'e2e.messages.motion?.saved===true', timeout=30000)
            if original_camera:
                page.evaluate("s=>{e2e.messages.settings=null;command({op:'apply',settings:s})}", original_camera)
                wait_reply(page, 'camera restore', 'e2e.messages.settings?.saved===true', timeout=30000)
            expected_disconnect[0] = True
            page.get_by_role('button', name='Disconnect', exact=True).click()
            page.wait_for_timeout(1000)
        except Exception as error:
            fail(f'could not restore settings: {error}')
        browser.close()

print('\nStep summary (fps, longest gap):')
for name, width, height, fps, worst in results:
    print(f'  {name:28s} {width:>4}x{height:<4} {fps:5.1f} fps  {worst:4.1f} s')
if failures:
    print(f'\nFAIL: {len(failures)} problem(s):')
    for item in failures:
        print('  -', item)
    sys.exit(1)
print('\nPASS: page, Connect, streaming, every setting change, saves, reconnects and reload worked without freezes or faults.')
