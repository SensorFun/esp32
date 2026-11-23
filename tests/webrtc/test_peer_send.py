"""Exercise bounded UDP backpressure retries using the actual firmware shim."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_send_backpressure_and_passthrough(tmp_path):
    compiler = shutil.which('cc')
    if compiler is None:
        pytest.skip('C compiler unavailable')
    (tmp_path / 'freertos').mkdir()
    headers = {
        'udp.h': '#include <stdint.h>\ntypedef struct { int unused; } udp_socket_t;\ntypedef struct { int unused; } esp_peer_addr_t;\n',
        'esp_timer.h': '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
        'esp_log.h': '#define ESP_LOGW(...) ((void)0)\n',
        'esp_heap_caps.h': '#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\n',
        'freertos/FreeRTOS.h': '#define pdMS_TO_TICKS(ms) (ms)\n',
        'freertos/task.h': 'void vTaskDelay(unsigned ticks);\n',
    }
    for name, content in headers.items():
        (tmp_path / name).write_text(content)
    (tmp_path / 'check.c').write_text('''
#include "udp.h"
#include <assert.h>
#include <errno.h>
static udp_socket_t socket;
static esp_peer_addr_t address;
static uint8_t buffer[8];
static int calls, congested, result;
static int64_t now;
int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(unsigned ticks) { now += ticks * 1000; }
int __real_udp_socket_sendto(udp_socket_t *s, esp_peer_addr_t *a, const uint8_t *b, int n) {
    assert(s == &socket && a == &address && b == buffer && n == 8);
    calls++;
    if (congested-- > 0) { now += 40000; errno = ENOMEM; return -200; }
    errno = EIO; return result;
}
int __wrap_udp_socket_sendto(udp_socket_t *, esp_peer_addr_t *, const uint8_t *, int);
uint32_t camera_transport_queue_full(void);
static int send(void) { return __wrap_udp_socket_sendto(&socket, &address, buffer, 8); }
int main(void) {
    result = 8; congested = 2;
    assert(send() == 8 && calls == 3 && now == 100000);
    // A full queue counts once per datagram, even when a retry succeeds.
    assert(camera_transport_queue_full() == 1);
    calls = 0; congested = 100; now = 0;
    assert(send() == -200 && calls == 6 && now == 290000 && errno == ENOMEM);
    assert(camera_transport_queue_full() == 2);
    calls = 0; congested = 0; result = -1;
    assert(send() == -1 && calls == 1 && errno == EIO);
    assert(camera_transport_queue_full() == 2);
    calls = 0; congested = 0; result = 0;
    assert(send() == 0 && calls == 1);
}
''')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(tmp_path), str(ROOT / 'projects/webrtc/main/peer_send.c'),
                    str(tmp_path / 'check.c'), '-o', str(tmp_path / 'check')], check=True)
    subprocess.run([str(tmp_path / 'check')], check=True)
