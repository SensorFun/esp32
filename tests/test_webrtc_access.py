"""Check the firmware's authentication serialization independently of the browser."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AccessProtocolTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'Native C++ compiler required')
    def test_authentication_transcript(self):
        source = r'''
#include "auth_protocol.h"
#include <cassert>
int main() {
    using namespace access_protocol;
    uint8_t bytes[2]{};
    assert(unhex("aB01", bytes, 2));
    assert(hex(bytes, 2) == "ab01");
    assert(!unhex("ab0z", bytes, 2));
    assert(!unhex("ab", bytes, 2));
    std::string cert;
    for (int i = 0; i < 32; ++i) cert += i ? ":AB" : "AB";
    const std::string sdp = "v=0\r\na=fingerprint:sha-256 " + cert + "\r\n";
    std::string fp;
    for (int i = 0; i < 32; ++i) fp += "ab";
    assert(fingerprint(sdp) == fp);
    assert(fingerprint(sdp + sdp).empty());
    assert(fingerprint(sdp + "a=fingerprint:sha-1 AB\r\n").empty());
    assert(fingerprint("x" + sdp.substr(5)).empty());
    assert(fingerprint("v=0\r\na=fingerprint:sha-256 AB\r\n").empty());
    assert(fingerprint("v=0\r\na=fingerprint:sha-1 " + cert).empty());
    assert(transcript("020000000001", "salt", 210000, "nonce", "offer", "answer") ==
        "camera-scram-v2\n020000000001\nsalt\n210000\nnonce\noffer\nanswer");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'projects/webrtc/main'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
