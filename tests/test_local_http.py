"""Check the local HTTP boundary independently of ESP-IDF and hardware."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_local_http_network_and_origin_policy(tmp_path):
    compiler = shutil.which('c++')
    if not compiler:
        pytest.skip('Native C++ compiler required')
    source = tmp_path / 'policy.cpp'
    source.write_text(r'''
#include "local_http.h"
#include <cassert>
using namespace camera_http;
uint32_t address(const char *text) {
    in_addr value{}; assert(inet_pton(AF_INET, text, &value) == 1); return value.s_addr;
}
sockaddr_storage peer(const char *text) {
    sockaddr_storage value{};
    auto &v4 = reinterpret_cast<sockaddr_in &>(value);
    v4.sin_family = AF_INET; v4.sin_addr.s_addr = address(text); return value;
}
int main() {
    auto mask = address("255.255.255.0"), camera = address("192.168.4.1");
    assert(local_peer(peer("192.168.4.2"), camera, mask));
    assert(!local_peer(peer("192.168.0.2"), camera, mask));
    assert(!local_peer(peer("203.0.113.2"), camera, mask));
    assert(!local_peer(peer("127.0.0.1"), camera, mask));
    assert(!local_peer(peer("192.168.4.2"), camera, 0));
    assert(!local_peer(peer("203.0.113.2"), address("203.0.113.1"), mask));
    assert(local_peer(peer("10.2.3.4"), address("10.2.3.1"), mask));
    assert(local_peer(peer("172.16.0.2"), address("172.16.0.1"), mask));
    assert(!private_ipv4(address("172.32.0.1")));
    sockaddr_storage mapped{};
    auto &v6 = reinterpret_cast<sockaddr_in6 &>(mapped); v6.sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:192.168.4.2", &v6.sin6_addr) == 1);
    assert(local_peer(mapped, camera, mask));
    assert(inet_pton(AF_INET6, "2001:db8::2", &v6.sin6_addr) == 1);
    assert(!local_peer(mapped, camera, mask));
    assert(origin_matches("192.168.4.1", "http://192.168.4.1", "192.168.4.1", false));
    assert(origin_matches("192.168.4.1:80", "http://192.168.4.1:80", "192.168.4.1", false));
    assert(origin_matches("192.168.4.1", "", "192.168.4.1", false));
    assert(!origin_matches("192.168.4.1", "https://192.168.4.1", "192.168.4.1", false));
    assert(!origin_matches("192.168.4.1:443", "", "192.168.4.1", false));
    assert(!origin_matches("camera.example.test", "", "192.168.4.1", false));
    assert(!origin_matches("192.168.4.1", "http://example.test", "192.168.4.1", false));
    assert(origin_matches("192.168.4.1:443", "https://192.168.4.1:443", "192.168.4.1", true));
    assert(!origin_matches("192.168.4.1:80", "http://192.168.4.1:80", "192.168.4.1", true));
}
''')
    binary = tmp_path / 'policy'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'projects/common/provisioning/include'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
