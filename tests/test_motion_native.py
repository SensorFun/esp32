"""Compile the firmware's pure motion code on the host and check its behaviour."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'projects/webrtc/main'


@unittest.skipUnless(shutil.which('g++'), 'g++ is required')
class MotionNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        folder = Path(cls.temp.name)
        binary = folder / 'motion_test'
        subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{MAIN}',
                        str(ROOT / 'tests/native/motion_test.cpp'), str(MAIN / 'optical_flow.cpp'), str(MAIN / 'avi_writer.cpp'),
                        '-o', str(binary)], check=True)
        cls.avi = folder / 'clip.avi'
        output = subprocess.run([str(binary), str(cls.avi)], check=True, capture_output=True, text=True).stdout
        cls.checks = {item['check']: item for item in map(json.loads, output.splitlines())}

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_global_shifts_are_detected_with_their_magnitude(self):
        one, three = self.checks['shift_1px'], self.checks['shift_3px']
        self.assertTrue(one['motion'] and three['motion'])
        self.assertGreater(one['area'], 80)
        self.assertAlmostEqual(one['flow'], 1.0, delta=0.25)
        self.assertAlmostEqual(three['flow'], (3 ** 2 + 2 ** 2) ** 0.5, delta=0.8)

    def test_noise_brightness_and_flat_scenes_are_not_motion(self):
        for name in ('noise_only', 'brightness', 'flat'):
            with self.subTest(name=name):
                self.assertFalse(self.checks[name]['motion'])
                self.assertLess(self.checks[name]['area'], 1)

    def test_moving_area_threshold(self):
        small, strict = self.checks['object_small_area'], self.checks['object_strict_area']
        self.assertTrue(small['motion'])
        self.assertLess(small['area'], 20)
        self.assertFalse(strict['motion'])

    def test_cell_size_sets_the_grid(self):
        self.assertEqual((self.checks['cell_20']['cols'], self.checks['cell_20']['rows']), (8, 6))
        self.assertEqual((self.checks['shift_1px']['cols'], self.checks['shift_1px']['rows']), (20, 15))
        self.assertTrue(self.checks['cell_20']['motion'])

    def test_gray_conversion(self):
        self.assertEqual(self.checks['gray']['value'], 200)
        self.assertEqual(self.checks['gray']['last'], 200)

    def test_avi_is_a_valid_indexed_mjpeg_file(self):
        info = self.checks['avi']
        self.assertTrue(info['ok'] and info['read'] and info['finalized'])
        self.assertEqual((info['frames'], info['micro'], info['width'], info['height']), (3, 100000, 320, 240))
        data = self.avi.read_bytes()
        self.assertEqual(data[:4], b'RIFF')
        self.assertEqual(struct.unpack_from('<I', data, 4)[0], len(data) - 8)
        movi = data.index(b'movi')
        self.assertEqual(struct.unpack_from('<I', data, movi - 4)[0], data.index(b'idx1') - movi)
        index = data.index(b'idx1')
        count = struct.unpack_from('<I', data, index + 4)[0] // 16
        sizes = []
        for i in range(count):
            fourcc, flags, offset, size = struct.unpack_from('<4sIII', data, index + 8 + 16 * i)
            self.assertEqual((fourcc, flags), (b'00dc', 0x10))
            self.assertEqual(data[movi + offset:movi + offset + 4], b'00dc')
            self.assertEqual(data[movi + offset + 8:movi + offset + 10], b'\xff\xd8')
            sizes.append(size)
        self.assertEqual(sizes, [7, 6, 3000])
        if shutil.which('ffprobe'):
            probe = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-show_entries', 'stream=codec_name,nb_read_frames',
                                    '-of', 'json', str(self.avi)], capture_output=True, text=True)
            self.assertEqual(json.loads(probe.stdout)['streams'][0]['codec_name'], 'mjpeg')


if __name__ == '__main__':
    unittest.main()
