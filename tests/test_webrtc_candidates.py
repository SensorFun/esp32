"""Exercise the exact firmware SDP normalization without ESP-IDF/hardware."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LocalCandidateTests(unittest.TestCase):
    def test_mdns_mapping_preserves_ports_credentials_and_fingerprint(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        source = r'''
#include "local_candidates.h"
#include <cassert>
int main() {
    const std::string identity = "a=ice-ufrag:abcd\r\na=fingerprint:sha-256 AA:BB\r\n";
    const std::string numeric = "a=candidate:1 1 UDP 123 192.0.2.2 54321 typ host\r\n";
    std::string sdp = identity + numeric +
        "a=candidate:2 1 UDP 123 a-b.local 54322 typ host\r\n" +
        "a=candidate:3 1 UDP 122 c-d.LOCAL. 54323 typ host\r\n" +
        "a=candidate:4 1 UDP 121 not.local.example 54324 typ host\r\n";
    assert(normalize_local_candidates(sdp, "192.0.2.10") == 2);
    assert(sdp.find(identity + numeric) == 0);
    assert(sdp.find("192.0.2.10 54322 typ host") != std::string::npos);
    assert(sdp.find("192.0.2.10 54323 typ host") != std::string::npos);
    assert(sdp.find("not.local.example 54324") != std::string::npos);
    assert(normalize_local_candidates(sdp, "192.0.2.10") == 0);
    for (const char *bad : {"", "a=candidate:", "a=candidate:1 1 UDP\n", "x a=candidate:2 1 UDP 123 a.local 22 typ host"}) {
        std::string text = bad;
        assert(normalize_local_candidates(text, "192.0.2.10") == 0);
        assert(text == bad);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'check.cpp').write_text(source)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ROOT / 'projects/webrtc/main'), str(path / 'check.cpp'),
                            '-o', str(path / 'check')], check=True, capture_output=True)
            subprocess.run([str(path / 'check')], check=True)
