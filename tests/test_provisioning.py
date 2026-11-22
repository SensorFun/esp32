"""Exercise shared credential parsing and the provisioning UI without hardware."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "projects/common/provisioning"


class ProvisioningTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Native C++ compiler required")
    def test_credentials_and_json_encoding(self):
        source = r'''
#include "credentials.h"
#include <cassert>
#include <string>
using namespace provisioning;
int main() {
    Credentials result{};
    assert(parse_credentials("ssid=Example+network&password=example%26key", result));
    assert(std::string(result.ssid) == "Example network");
    assert(std::string(result.password) == "example&key");
    assert(parse_credentials("ssid=Hidden%2Bnetwork&password=", result));
    assert(std::string(result.ssid) == "Hidden+network");
    assert(valid_credentials(std::string(32, 's'), std::string(63, 'p')));
    assert(!valid_credentials(std::string(33, 's'), ""));
    assert(!valid_credentials("", ""));
    assert(!valid_credentials("Example", "short"));
    assert(valid_credentials("Example", std::string(64, 'a')));
    assert(!valid_credentials("Example", std::string(64, 'z')));
    assert(!valid_credentials("Example", std::string(65, 'a')));
    assert(!valid_credentials(std::string("n\0x", 3), ""));
    assert(!valid_credentials("Example", std::string("pass\0word", 9)));
    assert(!parse_credentials("ssid=Example&password=example%", result));
    assert(!parse_credentials("ssid=Example&password=example%GG", result));
    assert(!parse_credentials("ssid=Example&password=example%00key", result));
    assert(!parse_credentials("ssid=Example&ssid=Other&password=", result));
    assert(!parse_credentials("ssid=Example", result));
    assert(!parse_credentials("ssid=&password=", result));
    assert(!parse_credentials("ssid=Example&password=&extra=1", result));
    assert(json_quote("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
    std::string camera_password;
    assert(parse_credentials("ssid=Example&password=&camera_password=synthetic-test-vector", result, &camera_password));
    assert(camera_password == "synthetic-test-vector");
    assert(!parse_credentials("ssid=Example&password=", result, &camera_password));
    camera_password.clear();
    assert(parse_credentials("ssid=Example&password=", result, &camera_password, false));
    assert(camera_password.empty());
    assert(!parse_credentials("ssid=Example&password=&camera_password=short", result, &camera_password));
    assert(!parse_credentials("ssid=Example&password=&camera_password=synthetic-test-vector&camera_password=duplicate", result, &camera_password));
    assert(!parse_credentials("ssid=Example&password=&camera_password=abc%00defghijkl", result, &camera_password));
    const std::string form = "ssid=Example&password=&camera_password=synthetic-test-vector";
    assert(parse_credentials(form, result, &camera_password));
    assert(!parse_credentials(form + "&signal_server=builtin", result, &camera_password));
    assert(!parse_credentials(form + "&signal_server_url=wss%3A%2F%2Fsignal.example.test%2Fcamera", result, &camera_password));
    assert(!parse_credentials(form + "&signal_server=custom&signal_server_url=wss%3A%2F%2Fsignal.example.test%2Fcamera", result, &camera_password));
    assert(json_quote("<script>") == "\"<script>\"");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.cpp").write_text(source)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(COMPONENT),
                            str(path / "test.cpp"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    @unittest.skipUnless(shutil.which("c++"), "Native C++ compiler required")
    def test_setup_address_accepts_ipv4_and_mapped_ipv6(self):
        source = r'''
#include "request_address.h"
#include <arpa/inet.h>
#include <cassert>
int main() {
    in_addr expected{};
    assert(inet_pton(AF_INET, "192.168.4.1", &expected) == 1);
    sockaddr_storage address{};
    auto &ipv4 = reinterpret_cast<sockaddr_in &>(address);
    ipv4.sin_family = AF_INET;
    ipv4.sin_addr = expected;
    assert(provisioning::matches_setup_address(address, expected.s_addr));
    assert(inet_pton(AF_INET, "192.168.1.1", &ipv4.sin_addr) == 1);
    assert(!provisioning::matches_setup_address(address, expected.s_addr));
    address = {};
    auto &ipv6 = reinterpret_cast<sockaddr_in6 &>(address);
    ipv6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:192.168.4.1", &ipv6.sin6_addr) == 1);
    assert(provisioning::matches_setup_address(address, expected.s_addr));
    assert(inet_pton(AF_INET6, "::ffff:192.168.1.1", &ipv6.sin6_addr) == 1);
    assert(!provisioning::matches_setup_address(address, expected.s_addr));
    assert(inet_pton(AF_INET6, "::1", &ipv6.sin6_addr) == 1);
    assert(!provisioning::matches_setup_address(address, expected.s_addr));
    address.ss_family = AF_UNSPEC;
    assert(!provisioning::matches_setup_address(address, expected.s_addr));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.cpp").write_text(source)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(COMPONENT),
                            str(path / "test.cpp"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    @unittest.skipUnless(shutil.which("node"), "Node.js required for browser logic tests")
    def test_refresh_submit_and_status_browser_flow(self):
        html = (COMPONENT / "index.html").read_text()
        self.assertIn('<select id="ssid"', html)
        self.assertNotIn("<datalist", html)
        self.assertNotIn('id="reset"', html)
        self.assertNotIn('id="signal-server"', html)
        self.assertNotIn('id="signal-server-url"', html)
        script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
        harness = r'''
const vm=require('node:vm'),assert=require('node:assert/strict');
const elements={};
for(const id of ['setup','ssid','manual-ssid','manual-network','password','camera-access','camera-password','camera-password-option','change-camera-password','refresh','save','status','local-viewer','local-viewer-link','remote-viewer','remote-viewer-link']) {
    elements[id]={value:'',disabled:false,textContent:'',children:[],handlers:{},
        addEventListener(name,fn){this.handlers[name]=fn;},
        replaceChildren(){this.children=[];},append(child){this.children.push(child);}};
}
const replies=[],calls=[],timers=[],navigations=[];
const context=vm.createContext({URLSearchParams,AbortSignal,
    window:{addEventListener(){}},location:{assign:url=>navigations.push(url)},
    document:{querySelector:selector=>elements[selector.slice(1)],createElement:()=>({})},
    setTimeout:fn=>timers.push(fn),
    fetch:async(path,options)=>{calls.push({path,options});assert.ok(replies.length,'unexpected fetch');return replies.shift();}});
const reply=(data,status=200)=>replies.push({ok:status<400,status,json:async()=>data});
const flush=async()=>{for(let i=0;i<8;i++)await Promise.resolve();};
(async()=>{
    reply({ssids:['Example network','<img src=x onerror=alert(1)>']});
    vm.runInContext(SCRIPT,context);
    await flush();
    assert.equal(elements.ssid.children.length,4);
    assert.equal(elements.ssid.children[2].textContent,'<img src=x onerror=alert(1)>');
    assert.equal(elements.ssid.children[2].value,'1');
    assert.equal(elements['manual-network'].hidden,true);
    assert.equal(elements['camera-password-option'].hidden,true);
    elements.ssid.value='0';
    reply({ssids:['Other network'],camera_password_required:true});
    await elements.refresh.handlers.click();
    assert.equal(elements.ssid.children.length,3);
    assert.equal(elements.ssid.children[1].textContent,'Other network');
    assert.equal(elements.ssid.value,'');
    elements.ssid.value='0';elements.password.value='example&key';
    assert.equal(elements['camera-password'].required,true);
    elements['camera-password'].value='synthetic-test-vector';
    reply({state:'connecting'},202);
    await elements.setup.handlers.submit({preventDefault(){}});
    const request=calls.at(-1);
    assert.equal(request.path,'/api/connect');
    assert.equal(request.options.method,'POST');
    assert.equal(request.options.body.get('ssid'),'Other network');
    assert.equal(request.options.body.get('password'),'example&key');
    assert.equal(elements.password.value,'');
    assert.equal(request.options.body.get('camera_password'),'synthetic-test-vector');
    assert.equal(request.options.body.has('signal_server'),false);
    assert.equal(request.options.body.has('signal_server_url'),false);
    assert.equal(elements['camera-password'].value,'');
    reply({state:'provisioning'});await timers.shift()();
    assert.equal(elements.save.disabled,false);
    assert.match(elements.status.textContent,/Could not connect/);
    elements.ssid.value='manual';elements.ssid.handlers.change();
    assert.equal(elements['manual-network'].hidden,false);
    assert.equal(elements['manual-ssid'].required,true);
    elements['manual-ssid'].value='Hidden network';
    reply({ssids:[],camera_password_required:false,camera_password_saved:true});await elements.refresh.handlers.click();
    assert.equal(elements.ssid.value,'manual');
    assert.equal(elements['manual-ssid'].value,'Hidden network');
    assert.equal(elements['camera-password-option'].hidden,false);
    const change=elements['change-camera-password'],cameraPassword=elements['camera-password'];
    assert.equal(cameraPassword.disabled,true);
    change.checked=true;change.handlers.change();
    assert.equal(cameraPassword.required,true);
    cameraPassword.value='synthetic-test-vector';
    change.checked=false;change.handlers.change();
    assert.equal(cameraPassword.value,'');
    reply({state:'connecting'},202);await elements.setup.handlers.submit({preventDefault(){}});
    assert.equal(calls.at(-1).options.body.get('ssid'),'Hidden network');
    assert.equal(calls.at(-1).options.body.has('camera_password'),false);
    assert.equal(calls.at(-1).options.body.has('signal_server'),false);
    assert.equal(calls.at(-1).options.body.has('signal_server_url'),false);
    reply({state:'connected',ip:'192.0.2.25',local_url:'https://192.0.2.25/'});await timers.shift()();
    assert.match(elements.status.textContent,/settings are saved/);
    assert.equal(elements['local-viewer'].hidden,false);
    assert.equal(elements['local-viewer-link'].href,'https://192.0.2.25/');
    assert.equal(elements['local-viewer-link'].textContent,'https://192.0.2.25/');
    reply({});
    const open=elements['local-viewer-link'].handlers.click({preventDefault(){}});
    await flush();
    assert.equal(calls.at(-1).path,'/api/finish');
    assert.equal(calls.at(-1).options.method,'POST');
    assert.deepEqual(navigations,[]);
    timers.shift()();await open;
    assert.deepEqual(navigations,['https://192.0.2.25/']);
    reply({error:'Setup closed'},409);
    await elements['local-viewer-link'].handlers.click({preventDefault(){}});
    assert.deepEqual(navigations,['https://192.0.2.25/','https://192.0.2.25/']);
    vm.runInContext('showLocalViewer({ip:"192.0.2.25",local_url:"http://192.0.2.25/"})',context);
    assert.equal(elements['local-viewer-link'].href,'http://192.0.2.25/');
    assert.equal(elements['local-viewer'].hidden,false);
    vm.runInContext('showLocalViewer({ip:"192.0.2.25",local_url:"http://example.test/"})',context);
    assert.equal(elements['local-viewer'].hidden,true);
    vm.runInContext('showLocalViewer({ip:"192.0.2.25",local_url:"javascript:alert(1)"})',context);
    assert.equal(elements['local-viewer'].hidden,true);
    assert.ok(!calls.some(call=>call.path==='/api/reset'));
})().catch(error=>{console.error(error);process.exitCode=1;});
'''
        import json
        subprocess.run(["node", "-e", harness.replace("SCRIPT", json.dumps(script))], check=True)
