"""Hardware regression: GUI settings under continuous motion analysis.

Keeps one WebRTC channel open and checks JPEG progress and USB watchdog/reset
logs while changing camera and motion controls. Restores the starting settings.
"""
import argparse

from playwright.sync_api import sync_playwright
from test_local_webrtc import identity, wait_js

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--browser', required=True)
parser.add_argument('--rounds', type=int, default=2)
args = parser.parse_args()
if args.rounds < 1:
    parser.error('--rounds must be positive')

url, _, console = identity(args.port)
with console, sync_playwright() as p:
    browser = p.chromium.launch(executable_path=args.browser, args=['--no-sandbox'])
    page = browser.new_page(ignore_https_errors=True, viewport={'width': 390, 'height': 844})
    page.goto(url, wait_until='domcontentloaded')
    page.locator('#connect').click()
    wait_js(page, "channel?.readyState==='open'")
    page.evaluate("""()=>{
      window.responses={}; window.frames=0; window.originalChannel=channel;
      channel.addEventListener('message', event=>{
        if(typeof event.data==='string'){const m=JSON.parse(event.data);responses[m.type]=m;}
      });
      document.querySelector('#preview').addEventListener('load',()=>frames++);
      command({op:'get'}); command({op:'motion_get'});
    }""")
    wait_js(page, 'responses.settings && responses.motion')
    metadata = page.evaluate('responses.settings')
    original_motion = page.evaluate('responses.motion.values')
    original_camera = dict(resolution=metadata['resolution'], timing_profile=metadata['timing_profile'],
                           **{c['name']: c['value'] for c in metadata['controls'] if c['supported']})

    def check(expression, arg=None, timeout=20000):
        wait_js(page, expression, arg=arg, timeout=timeout)
        assert not console.runtime_faults, console.runtime_faults
        assert page.evaluate("channel===originalChannel && channel.readyState==='open'"), 'Camera reconnected'

    def motion(values):
        page.evaluate("s=>{responses.motion=null;command({op:'motion_apply',settings:s})}", values)
        check('responses.motion?.saved===true')

    def progress():
        before = page.evaluate('frames')
        check('n=>frames>n', before)

    try:
        motion({'enabled': True, 'analysis_fps': 15, 'blur': 2, 'cell': 5})
        page.get_by_role('tab', name='Camera settings', exact=True).click()
        for iteration in range(args.rounds):
            for size in metadata['resolutions']:
                print(f'Round {iteration + 1}: {size["width"]} x {size["height"]}', flush=True)
                page.evaluate('responses.settings=null')
                page.get_by_role('combobox', name='Resolution', exact=True).select_option(str(size['value']))
                check('v=>responses.settings?.resolution===v', size['value'])
                check('size=>{const img=document.querySelector("#preview");return img.naturalWidth===size.width && img.naturalHeight===size.height}', size)
                for profile in metadata['timing_profiles']:
                    page.evaluate('responses.settings=null')
                    page.get_by_role('combobox', name='Light timing', exact=True).select_option(profile)
                    check('v=>responses.settings?.timing_profile===v', profile)
                for name, value in [('brightness', iteration % 3 - 1), ('contrast', 1 - iteration % 3), ('jpeg_quality', 12 + iteration % 3)]:
                    page.evaluate('responses.settings=null')
                    page.get_by_role('spinbutton', name=name.replace('_', ' '), exact=True).fill(str(value))
                    check('v=>responses.settings?.controls.find(c=>c.name===v.name)?.value===v.value', {'name': name, 'value': value})
                progress()
            page.evaluate('responses.settings=null')
            page.get_by_role('button', name='Save', exact=True).first.click()
            check('responses.settings?.saved===true')
            progress()
            page.get_by_role('tab', name='Motion detection', exact=True).click()
            panel = page.locator('sensorfan-motion-settings[group=detection]')
            panel.get_by_label('Minimum speed', exact=True).fill(str(2.5 + iteration * 0.25))
            page.evaluate('responses.motion=null')
            panel.get_by_role('button', name='Save', exact=True).click()
            check('responses.motion?.saved===true')
            page.get_by_role('tab', name='Camera settings', exact=True).click()
            progress()
        print('PASS: GUI resolution/timing/image controls and both Save paths; uninterrupted channel, live frames, no watchdog/reset.', flush=True)
    except Exception:
        print('Stability failure diagnostics:', page.evaluate('({status:document.querySelector("#status").textContent,channel:channel?.readyState,frames,settings:responses.settings?.resolution})'), flush=True)
        raise
    finally:
        # A fresh viewer can restore settings even if a failed run lost its channel.
        if page.evaluate("channel?.readyState!=='open'"):
            page.evaluate('stop()')
            wait_js(page, '!document.querySelector("#connect").disabled')
            page.locator('#connect').click()
            wait_js(page, "channel?.readyState==='open'")
            page.evaluate('window.originalChannel=channel')
        motion(original_motion)
        page.evaluate("s=>{responses.settings=null;command({op:'apply',settings:s})}", original_camera)
        check('responses.settings?.saved===true')
        page.locator('#disconnect').click()
        browser.close()
