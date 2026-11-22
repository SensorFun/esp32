"""Observe a real camera's continuous local stream without requesting a reset.

Checks decoded JPEG progress, setting replies, HTTPS responsiveness, channel
identity, and serial reset/watchdog diagnostics. Does not change camera settings.
"""
import argparse
from collections import deque
import re
import threading
import time

from playwright.sync_api import sync_playwright
from test_local_webrtc import CameraConsole, add_password_argument, camera_password, connect_local, wait_js

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url', required=True)
parser.add_argument('--port', required=True)
parser.add_argument('--browser', required=True)
parser.add_argument('--seconds', type=int, default=180)
parser.add_argument('--reconnects', type=int, default=2)
parser.add_argument('--reset', action='store_true', help='Pulse reset before the observation window')
add_password_argument(parser)
args = parser.parse_args()
if args.seconds < 30 or args.reconnects < 0:
    parser.error('Use at least 30 seconds and a nonnegative reconnect count')
password = camera_password(args)

# Set line levels before opening. Some USB adapters still pulse reset on open;
# the observation baseline begins after the application and stream are ready.
console = CameraConsole(port=None, baudrate=115200, timeout=.2)
console.dtr = False
console.rts = False
console.port = args.port
console.open()
if args.reset:
    console.rts = True
    time.sleep(.1)
    console.rts = False
diagnostics = deque(maxlen=80)
resets = []
faults = []


def drain():
    pending = ''
    while not console.drain_stop.is_set():
        try:
            pending += console.read(4096).decode(errors='replace')
        except Exception:
            if console.drain_stop.is_set():
                return
            raise
        parts = pending.split('\n')
        pending = parts.pop()
        for raw in parts:
            line = re.sub(r'\x1b\[[0-9;]*m', '', raw).strip()
            if 'rst:' in line:
                resets.append(line)
            if any(term in line.lower() for term in ('guru meditation', 'task_wdt', 'abort() was called', 'corrupt heap', 'brownout', 'backtrace:')):
                faults.append(line)
            if re.search(r'(?:local_webrtc|camera_transport|DTLS|SCTP|cam_hal):|rst:|Guru Meditation|Backtrace:', line):
                diagnostics.append(line)


console.diagnostic_thread = threading.Thread(target=drain, daemon=True)
console.diagnostic_thread.start()
with console, sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox', '--ignore-certificate-errors'])
    context = browser.new_context(ignore_https_errors=True)
    page = context.new_page()
    try:
        deadline = time.monotonic() + 90
        while True:
            try:
                page.goto(args.url, wait_until='domcontentloaded', timeout=30000)
                break
            except Exception:
                if time.monotonic() >= deadline:
                    raise
                page.wait_for_timeout(1000)

        def connect():
            wait_js(page, "!document.querySelector('#connect').disabled")
            connect_local(page, password)
            page.evaluate("""()=>{
              window.frames=0; window.replies=0; window.lastSettings=null;
              window.originalPeer=pc; window.originalChannel=channel;
              document.querySelector('#preview').onload=()=>frames++;
              channel.addEventListener('message', event=>{
                if(typeof event.data==='string'){
                  const message=JSON.parse(event.data);
                  if(message.type==='settings'){replies++;lastSettings=message;}
                }
              });
              command({op:'get'});
            }""")
            wait_js(page, 'frames>0 && replies>0')

        connect()
        initial_resets = len(resets)
        start = last_frame = time.monotonic()
        previous_frames = 0
        for step in range((args.seconds + 4) // 5):
            page.wait_for_timeout(5000)
            result = page.evaluate("""({channel:channel?.readyState, peer:pc?.connectionState,
              ice:pc?.iceConnectionState, frames, replies,
              width:document.querySelector('#preview').naturalWidth,
              height:document.querySelector('#preview').naturalHeight,
              same:pc===originalPeer && channel===originalChannel})""")
            if result['frames'] > previous_frames:
                last_frame = time.monotonic()
                previous_frames = result['frames']
            print(f'{round(time.monotonic()-start)}s: {result}', flush=True)
            assert result['same'] and result['channel'] == 'open', 'Unexpected disconnect/reconnect'
            assert time.monotonic() - last_frame < 20, 'JPEG stream stalled for 20 seconds'
            assert not faults and len(resets) == initial_resets, f'Board fault/reset: {faults}'
            before = result['replies']
            page.evaluate("command({op:'get'})")
            wait_js(page, 'n=>replies>n', arg=before, timeout=10000)
            if step % 6 == 5:
                response = context.request.get(args.url, timeout=10000)
                assert response.status == 200 and len(response.body()) > 0, 'Local HTTPS stopped responding'
        for _ in range(args.reconnects):
            page.locator('#disconnect').click()
            wait_js(page, '!document.querySelector("#connect").disabled')
            connect()
            assert not faults and len(resets) == initial_resets, 'Board reset during reconnect'
        print(f'PASS: {args.seconds}s continuous local JPEG/control traffic, HTTPS responses, '
              f'{args.reconnects} reconnects; no board reset/watchdog.', flush=True)
    except Exception:
        print('Recent USB diagnostics:', *diagnostics, sep='\n', flush=True)
        raise
    finally:
        if not page.is_closed():
            try:
                page.evaluate('stop()')
            except Exception:
                pass
        browser.close()
