"""Exercise a real camera via public signaling from the local static frontend."""
import argparse
import getpass
import sys
import time
from playwright.sync_api import sync_playwright, expect
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', required=True)
parser.add_argument('--url', required=True)
parser.add_argument('--mac', help='Camera Wi-Fi MAC; connect without USB dropdown discovery')
parser.add_argument('--camera', help='Camera label from the launcher')
parser.add_argument('--password-stdin', action='store_true', help='Read password from stdin instead of an interactive prompt')
parser.add_argument('--reject', action='store_true', help='Expect authentication rejection and no frames/settings')
parser.add_argument('--unauthenticated', action='store_true', help='Attempt camera commands without authenticating')
parser.add_argument('--replay', action='store_true', help='Replay a previous session proof when reconnecting')
args = parser.parse_args()
password = sys.stdin.readline().rstrip('\r\n') if args.password_stdin else getpass.getpass('Camera password: ')
with sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox'])
    page = browser.new_page()
    requests = []
    page.on('request', lambda request: requests.append(request.url))
    page.on('pageerror', lambda error: print('PAGE ERROR', error))
    page.add_init_script("""const NativePeer=window.RTCPeerConnection;
window.__exposed=0;window.__sessions=0;window.__frameStats={messages:0,bytes:0,frameStarts:0,frameEnds:0};
window.RTCPeerConnection=class extends NativePeer{
  constructor(config){super(config);window.__testPeer=this;window.__sessions++;}
  createDataChannel(...args){const channel=super.createDataChannel(...args),send=channel.send.bind(channel);
    channel.addEventListener('message',event=>{if(typeof event.data!=='string'){
      window.__exposed++;window.__frameStats.messages++;window.__frameStats.bytes+=event.data.byteLength||event.data.size||0;
      if(event.data instanceof ArrayBuffer&&event.data.byteLength>=16){const v=new DataView(event.data),offset=v.getUint32(12,true),total=v.getUint32(8,true);if(!offset)window.__frameStats.frameStarts++;if(offset+event.data.byteLength-16===total)window.__frameStats.frameEnds++;window.__frameStats.last={offset,total};}
    }else if(JSON.parse(event.data).type==='settings')window.__exposed++;});
    channel.send=data=>{const msg=JSON.parse(data);if(msg.op==='authenticate'){
      if(window.__unauthenticated){send(JSON.stringify({op:'set',name:'contrast',value:2}));send(JSON.stringify({op:'get'}));return;}
      if(window.__replay && window.__sessions>1){msg.proof=window.__savedProof;data=JSON.stringify(msg);}
      else window.__savedProof=msg.proof;
    }send(data);};return channel;}
};""")
    page.add_init_script('window.__unauthenticated=' + str(args.unauthenticated).lower() + ';window.__replay=' + str(args.replay).lower())
    page.goto(args.url)
    if args.mac:
        page.get_by_label('Camera MAC address', exact=True).fill(args.mac)
    else:
        page.get_by_role('button', name='Load cameras', exact=True).click()
        expect(page.get_by_role('combobox', name='Camera').locator('option')).not_to_have_count(1, timeout=15000)
        if args.camera: page.get_by_role('combobox', name='Camera').select_option(label=args.camera)
    page.get_by_label('Camera password', exact=True).fill(password)
    page.get_by_role('button', name='Connect camera', exact=True).click()
    try:
        if args.reject or args.unauthenticated:
            expect(page.get_by_test_id('remote-status')).to_contain_text('authentication required' if args.unauthenticated else 'Camera not found', timeout=60000)
            assert not page.get_by_test_id('remote-image').evaluate('(img)=>img.naturalWidth > 0')
            assert page.locator('.remote-settings').count() == 0
            if args.reject: assert page.evaluate('window.__sessions') == 0, 'Wrong password reached camera negotiation'
            assert page.evaluate('window.__exposed') == 0, 'Camera sent protected data before authentication'
            print('PASS: unauthenticated commands rejected; no camera data exposed' if args.unauthenticated else 'PASS: wrong password found no camera room; no negotiation or camera data exposed')
            browser.close()
            sys.exit(0)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if page.get_by_test_id('remote-image').evaluate('(img)=>img.naturalWidth > 0'): break
            if page.locator('.remote-camera').get_attribute('data-state') == 'idle':
                raise RuntimeError(page.get_by_test_id('remote-status').inner_text())
            page.wait_for_timeout(200)
        else: raise TimeoutError('No camera image within 60 seconds')
        assert not any('localhost:8787' in url or '/cameras' in url or '/session/' in url for url in requests), 'Unexpected signaling bridge request'
        expect(page.get_by_role('spinbutton', name='contrast', exact=True)).to_be_visible(timeout=10000)
        before = page.get_by_role('spinbutton', name='contrast', exact=True).input_value()
        page.get_by_role('spinbutton', name='contrast', exact=True).fill('1')
        expect(page.get_by_role('spinbutton', name='contrast', exact=True)).to_have_value('1')
        page.get_by_role('combobox', name='Light timing', exact=True).select_option('balanced')
        expect(page.get_by_role('combobox', name='Light timing', exact=True)).to_have_value('balanced')
        page.get_by_role('combobox', name='Light timing', exact=True).select_option('high_light')
        page.get_by_role('spinbutton', name='contrast', exact=True).fill(before)
        frame_count = page.evaluate('window.__frameStats.frameEnds')
        page.wait_for_function('window.__frameStats.frameEnds >= ' + str(frame_count + 3), timeout=20000)
        expect(page.locator('.monitor-top')).to_contain_text('Direct · DTLS', timeout=15000)
        relay = page.evaluate("Array.from(__testPeer.getConfiguration().iceServers).some(s=>(Array.isArray(s.urls)?s.urls:[s.urls]).some(u=>/^turns?:/.test(u)))")
        assert relay is False, 'TURN server present in browser ICE configuration'
        stats = page.evaluate("async()=>Array.from((await __testPeer.getStats()).values()).filter(s=>s.type==='local-candidate' || s.type==='remote-candidate').map(s=>s.candidateType)")
        assert 'relay' not in stats, stats
        assert not any('192.168.' in url or '/api/session' in url for url in requests), requests
        print('PASS: direct public WebSocket signaling, live JPEG, contrast, timing, no board HTTP requests; STUN only, no relay candidates')
        page.get_by_role('button', name='Disconnect', exact=True).click()
        expect(page.get_by_test_id('remote-status')).to_have_text('Disconnected')
        page.wait_for_timeout(2000)  # Allow the hosted service to release membership.
        before_replay = page.evaluate('window.__exposed')
        page.get_by_role('button', name='Connect camera', exact=True).click()
        if args.replay:
            expect(page.get_by_test_id('remote-status')).to_contain_text('authentication failed', timeout=60000)
            assert not page.get_by_test_id('remote-image').evaluate('(img)=>img.naturalWidth > 0')
            assert page.evaluate('window.__exposed') == before_replay, 'Camera sent data to replaying peer'
            print('PASS: proof from a previous WebRTC connection rejected')
        else:
            page.wait_for_function("document.querySelector('[data-testid=remote-image]').naturalWidth > 0", timeout=60000)
            frame_count = page.evaluate('window.__frameStats.frameEnds')
            page.wait_for_function('window.__frameStats.frameEnds >= ' + str(frame_count + 3), timeout=20000)
            print('PASS: reconnect to the same camera session')
            page.get_by_role('button', name='Disconnect', exact=True).click()
    except Exception:
        print('STATUS:', page.get_by_test_id('remote-status').inner_text())
        print('ICE:', page.evaluate("window.__testPeer && ({state:__testPeer.iceConnectionState,relay:__testPeer.localDescription?.sdp.includes('typ relay'),signaling:__testPeer.signalingState})"))
        print('STREAM:', page.evaluate('({frames:window.__frameStats,settings:document.querySelectorAll(".remote-settings").length})'))
        raise
    finally:
        browser.close()
