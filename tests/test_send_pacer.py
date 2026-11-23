"""Adaptive send pacing: rises while the link keeps up, backs off on congestion."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]

TEST = r'''
#include "send_pacer.h"
#include <cassert>
#include <cstdio>
constexpr int64_t KiB = 1024;
constexpr SendPacer::Config CONFIG{128 * KiB, 256 * KiB, 1536 * KiB, 32 * KiB, 100000, 200000, 2048};

// Sends 2 KiB chunks whenever allowed for `us`, as the worker does; returns bytes.
int64_t stream(SendPacer &pacer, int64_t &now, int64_t us, int64_t link = 0) {
    int64_t bytes = 0, end = now + us, drained = now;
    while (now < end) {
        if (pacer.due(now)) {
            // A link slower than the pacer fills its queue: that is congestion.
            if (link && bytes * 1000000 > link * (now - drained + 1) + 16 * KiB * 1000000) pacer.congested(now);
            else { pacer.sent(2048, now); bytes += 2048; }
        }
        now += 1000;
    }
    return bytes;
}

int main() {
    int64_t now = 0;
    SendPacer pacer(CONFIG);
    assert(pacer.rate() == 256 * KiB && pacer.due(0));

    // A busy stream on a good link climbs to the ceiling within a few seconds.
    stream(pacer, now, 6000000);
    assert(pacer.rate() == 1536 * KiB);
    const int64_t fast = stream(pacer, now, 1000000);
    assert(fast > 1300 * KiB && fast < 1700 * KiB);

    // One congestion signal cuts to 70%; repeats within the cooldown do not.
    pacer.congested(now);
    assert(pacer.rate() == 1536 * KiB * 7 / 10);
    pacer.congested(now + 100000);
    assert(pacer.rate() == 1536 * KiB * 7 / 10 && pacer.cuts() == 1);
    // Sustained congestion bottoms out at the floor.
    for (int i = 1; i <= 20; ++i) pacer.congested(now + i * 200000);
    assert(pacer.rate() == 128 * KiB);
    now += 5000000;

    // An idle or light stream does not inflate the rate.
    SendPacer light(CONFIG);
    now = 0;
    for (int i = 0; i < 300; ++i) { now += 20000; if (light.due(now)) light.sent(2048, now); }
    assert(light.rate() == 256 * KiB);

    // On a 400 KiB/s link the rate settles around the link, not the ceiling.
    SendPacer limited(CONFIG);
    now = 0;
    stream(limited, now, 10000000, 400 * KiB);
    const int64_t settled = stream(limited, now, 5000000, 400 * KiB);
    assert(limited.rate() < 800 * KiB && limited.cuts() > 0);
    assert(settled > 300 * KiB * 5 && settled < 520 * KiB * 5);

    // A new viewer starts again from the starting rate.
    pacer.reset(now);
    assert(pacer.rate() == 256 * KiB && pacer.cuts() == 0);
    std::puts("ok");
}
'''


def test_send_pacer(tmp_path):
    compiler = shutil.which('c++')
    if compiler is None:
        pytest.skip('C++ compiler unavailable')
    (tmp_path / 'test.cpp').write_text(TEST)
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'projects/webrtc/main'),
                    str(tmp_path / 'test.cpp'), '-o', str(tmp_path / 'test')], check=True)
    result = subprocess.run([str(tmp_path / 'test')], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
