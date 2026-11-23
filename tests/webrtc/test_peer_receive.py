"""Exercise the firmware's setup/streaming receive policy without hardware."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_receive_policy_and_passthrough(tmp_path):
    compiler = shutil.which('cc')
    if compiler is None:
        pytest.skip('C compiler unavailable')
    # Only transport declarations are stubbed; compile the actual firmware shim.
    (tmp_path / 'udp.h').write_text('''
#include <stdbool.h>
#include <stdint.h>
typedef struct { int unused; } udp_socket_t;
typedef struct { int unused; } esp_peer_addr_t;
void udp_blocking_timeout(udp_socket_t *, long long int);
''')
    (tmp_path / 'check.c').write_text('''
#include "peer_receive.h"
#include "udp.h"
#include <assert.h>
static udp_socket_t socket;
static esp_peer_addr_t address;
static uint8_t buffer[8];
static int timeout_calls, receives, result = 7;
static long long timeout;
void udp_blocking_timeout(udp_socket_t *s, long long ms) {
    assert(s == &socket); timeout = ms; timeout_calls++;
}
int __real_udp_socket_recvfrom_nowait(udp_socket_t *s, esp_peer_addr_t *a,
                                    uint8_t *b, int len, bool nowait) {
    assert(s == &socket && a == &address && b == buffer && len == 8);
    assert(nowait == (receives == 2)); receives++; return result;
}
int __wrap_udp_socket_recvfrom_nowait(udp_socket_t *, esp_peer_addr_t *, uint8_t *, int, bool);
int main(void) {
    assert(__wrap_udp_socket_recvfrom_nowait(&socket, &address, buffer, 8, false) == 7);
    assert(timeout == 500 && timeout_calls == 1);
    camera_peer_transport_ready(true);
    result = -1;
    assert(__wrap_udp_socket_recvfrom_nowait(&socket, &address, buffer, 8, false) == -1);
    assert(timeout == 10 && timeout_calls == 2);
    __wrap_udp_socket_recvfrom_nowait(&socket, &address, buffer, 8, true);
    assert(timeout_calls == 2);
    camera_peer_transport_ready(false);
    __wrap_udp_socket_recvfrom_nowait(&socket, &address, buffer, 8, false);
    assert(timeout == 500 && timeout_calls == 3 && receives == 4);
}
''')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(tmp_path), '-I', str(ROOT / 'projects/webrtc/main'),
                    str(ROOT / 'projects/webrtc/main/peer_receive.c'), str(tmp_path / 'check.c'),
                    '-o', str(tmp_path / 'check')], check=True)
    subprocess.run([str(tmp_path / 'check')], check=True)
