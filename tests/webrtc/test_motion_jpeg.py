"""The motion decoder (tjpgd, luma only) at every scale against an exact reference.

A small baseline JPEG encoder writes synthetic frames in the OV2640's 4:2:2
layout and in 4:2:0. Because the test knows every quantized coefficient, its
reference is the exact inverse DCT of those coefficients, box-averaged to each
decoder scale. Saturated black and white areas check clipping, which grayscale
output must do itself.
"""
import math
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / 'projects/webrtc/main'

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60,
          61, 54, 47, 55, 62, 63]
LUMA_Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29,
          51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121,
          120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)] for u in range(8)]
C = [1 / math.sqrt(2)] + [1.0] * 7


def quant_table(quality):
    scale = 5000 // quality if quality < 50 else 200 - 2 * quality
    return [min(255, max(1, (q * scale + 50) // 100)) for q in LUMA_Q]


def fdct(block):
    out = []
    for v in range(8):
        for u in range(8):
            s = sum(block[y * 8 + x] * COS[u][x] * COS[v][y] for y in range(8) for x in range(8))
            out.append(0.25 * C[u] * C[v] * s)
    return out


def idct(coefficients):
    pixels = []
    for y in range(8):
        for x in range(8):
            s = sum(C[u] * C[v] * coefficients[v * 8 + u] * COS[u][x] * COS[v][y]
                    for v in range(8) for u in range(8) if coefficients[v * 8 + u])
            pixels.append(min(255, max(0, round(0.25 * s + 128))))
    return pixels


def category(value):
    return 0 if value == 0 else abs(value).bit_length()


class Bits:
    def __init__(self):
        self.data, self.acc, self.count = bytearray(), 0, 0

    def put(self, code, length):
        for i in range(length - 1, -1, -1):
            self.acc = self.acc << 1 | (code >> i & 1)
            self.count += 1
            if self.count == 8:
                self.data.append(self.acc)
                if self.acc == 0xFF:
                    self.data.append(0)
                self.acc = self.count = 0

    def flush(self):
        if self.count:
            self.put((1 << (8 - self.count)) - 1, 8 - self.count)


def table(symbols):
    # Every used symbol gets one code length L with 2^L > N, so no code is all ones.
    symbols = sorted(symbols)
    length = max(1, len(symbols).bit_length())
    return {s: (i, length) for i, s in enumerate(symbols)}, length, symbols


def encode(width, height, y_plane, c_plane, h_sampling, v_sampling, quality):
    """Baseline JPEG; returns (bytes, dequantized luma coefficients per block, in raster block order)."""
    q = quant_table(quality)
    mcu_w, mcu_h = 8 * h_sampling, 8 * v_sampling
    blocks, luma = [], {}
    for my in range(0, height, mcu_h):
        for mx in range(0, width, mcu_w):
            for by in range(v_sampling):
                for bx in range(h_sampling):
                    x0, y0 = mx + bx * 8, my + by * 8
                    pixels = [y_plane[(y0 + y) * width + x0 + x] - 128 for y in range(8) for x in range(8)]
                    levels = [round(c / q[i]) for i, c in enumerate(fdct(pixels))]
                    luma[(x0 // 8, y0 // 8)] = [levels[i] * q[i] for i in range(64)]
                    blocks.append((0, levels))
            for _ in range(2):  # Cb and Cr: one subsampled block each (flat chroma here)
                pixels = [c_plane - 128] * 64
                blocks.append((1, [round(c / q[i]) for i, c in enumerate(fdct(pixels))]))
    # Symbols per table, then the entropy-coded data.
    previous, runs = [0, 0], []
    for component, levels in blocks:
        dc = levels[0] - previous[component]
        previous[component] = levels[0]
        ac, run = [], 0
        for i in ZIGZAG[1:]:
            if levels[i] == 0:
                run += 1
                continue
            while run > 15:
                ac.append((0xF0, 0)); run -= 16
            ac.append((run << 4 | category(levels[i]), levels[i])); run = 0
        if run:
            ac.append((0x00, 0))
        runs.append((component, dc, ac))
    tables = {}
    for kind in (0, 1):  # DC, AC
        for component in (0, 1):
            used = {category(dc) for c, dc, _ in runs if c == component} if kind == 0 else \
                   {s for c, _, ac in runs if c == component for s, _ in ac}
            tables[kind, component] = table(used or {0})
    bits = Bits()
    for component, dc, ac in runs:
        code = tables[0, component][0]
        size = category(dc)
        bits.put(*code[size])
        if size:
            bits.put(dc if dc > 0 else dc + (1 << size) - 1, size)
        for symbol, value in ac:
            bits.put(*tables[1, component][0][symbol])
            if symbol & 0x0F:
                size = symbol & 0x0F
                bits.put(value if value > 0 else value + (1 << size) - 1, size)
    bits.flush()

    def segment(marker, payload):
        return bytes([0xFF, marker]) + (len(payload) + 2).to_bytes(2, 'big') + payload
    out = bytearray(b'\xff\xd8')
    for table_id in (0, 1):
        out += segment(0xDB, bytes([table_id]) + bytes(q[i] for i in ZIGZAG))
    out += segment(0xC0, bytes([8]) + height.to_bytes(2, 'big') + width.to_bytes(2, 'big') + bytes(
        [3, 1, h_sampling << 4 | v_sampling, 0, 2, 0x11, 1, 3, 0x11, 1]))
    for kind in (0, 1):
        for component in (0, 1):
            _, length, symbols = tables[kind, component]
            counts = [0] * 16
            counts[length - 1] = len(symbols)
            out += segment(0xC4, bytes([kind << 4 | component]) + bytes(counts) + bytes(symbols))
    out += segment(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]))
    out += bits.data + b'\xff\xd9'
    return bytes(out), luma


def scene(width, height, seed):
    """Gradients, a bright disc, a checker and noise, with saturated black and white bars."""
    pixels, state = [], seed
    for y in range(height):
        for x in range(width):
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            value = 40 + 150 * x / width + 30 * math.sin(y / 5.0) + (state % 21) - 10
            if (x - width * 0.6) ** 2 + (y - height * 0.4) ** 2 < (height / 4) ** 2:
                value += 70
            if (x // 8 + y // 8) % 2 and y > height * 0.7:
                value -= 60
            if x < width // 10:
                value = 0
            if x >= width - width // 10:
                value = 255
            pixels.append(min(255, max(0, int(value))))
    return pixels


HARNESS = r'''
#include "motion_jpeg.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>
struct Source { std::vector<unsigned char> jpeg; size_t offset = 0; std::vector<unsigned char> out; int width = 0; };
static size_t input(JDEC *d, uint8_t *buffer, size_t bytes) {
    auto &s = *static_cast<Source *>(d->device);
    bytes = std::min(bytes, s.jpeg.size() - s.offset);
    if (buffer) std::memcpy(buffer, s.jpeg.data() + s.offset, bytes);
    s.offset += bytes;
    return bytes;
}
static int output(JDEC *d, void *pixels, JRECT *r) {
    auto &s = *static_cast<Source *>(d->device);
    const int stride = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; ++y)
        std::memcpy(&s.out[y * s.width + r->left], static_cast<unsigned char *>(pixels) + (y - r->top) * stride, stride);
    return 1;
}
int main(int argc, char **argv) {
    Source s;
    s.jpeg.assign(std::istreambuf_iterator<char>(std::cin), {});
    static uint8_t work[16384];
    JDEC d{};
    if (jd_prepare(&d, input, work, sizeof(work), &s) != JDR_OK) return 2;
    const int scale = argc > 1 ? argv[1][0] - '0' : 0;
    s.width = d.width >> scale;
    s.out.assign(size_t(s.width) * (d.height >> scale), 0);
    if (jd_decomp(&d, output, scale) != JDR_OK) return 3;
    fwrite(s.out.data(), 1, s.out.size(), stdout);
}
'''


class MotionJpegTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('c++'):
            raise unittest.SkipTest('C++ compiler required')
        import tempfile
        cls.temp = tempfile.TemporaryDirectory()
        folder = Path(cls.temp.name)
        (folder / 'sdkconfig.h').write_text('#pragma once\n')
        (folder / 'harness.cpp').write_text(HARNESS)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{folder}', f'-I{MAIN}', '-c',
                        str(MAIN / 'motion_jpeg.c'), '-o', str(folder / 'motion_jpeg.o')], check=True)
        cls.decoder = folder / 'decode'
        subprocess.run(['c++', '-std=c++17', '-O2', f'-I{folder}', f'-I{MAIN}', str(folder / 'harness.cpp'),
                        str(folder / 'motion_jpeg.o'), '-o', str(cls.decoder)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, h_sampling, v_sampling, quality):
        width, height = 64, 48
        jpeg, luma = encode(width, height, scene(width, height, quality), 120, h_sampling, v_sampling, quality)
        exact = [0] * (width * height)
        for (bx, by), coefficients in luma.items():
            for i, value in enumerate(idct(coefficients)):
                exact[(by * 8 + i // 8) * width + bx * 8 + i % 8] = value
        for scale in range(4):
            with self.subTest(sampling=f'{h_sampling}x{v_sampling}', quality=quality, scale=scale):
                result = subprocess.run([str(self.decoder), str(scale)], input=jpeg, capture_output=True)
                self.assertEqual(result.returncode, 0)
                f = 1 << scale
                ow, oh = width // f, height // f
                expected = [sum(exact[(y * f + j) * width + x * f + i] for j in range(f) for i in range(f)) / (f * f)
                            for y in range(oh) for x in range(ow)]
                diffs = [abs(a - b) for a, b in zip(expected, result.stdout)]
                self.assertEqual(len(result.stdout), ow * oh)
                # Fixed-point Arai IDCT and float reference differ by rounding only; 1/8 uses the
                # block's DC, the exact mean. A missing clip would be off by up to 255.
                self.assertLess(sum(diffs) / len(diffs), 1.0)
                self.assertLessEqual(max(diffs), 6)

    def test_ov2640_422_layout(self):
        for quality in (50, 85):
            self.check(2, 1, quality)

    def test_420_layout(self):
        for quality in (50, 85):
            self.check(2, 2, quality)


if __name__ == '__main__':
    unittest.main()
