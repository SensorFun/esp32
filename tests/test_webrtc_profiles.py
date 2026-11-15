"""Camera-profile generation fails closed rather than dropping settings."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'projects/webrtc/main/generate_profiles.py'


class CameraProfileTests(unittest.TestCase):
    def generate(self, overrides, family="ov"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profiles = root / 'config/frame_grabber'
            profiles.mkdir(parents=True)
            for name in ('ov', 'gc'):
                profile = yaml.safe_load((ROOT / 'config/frame_grabber' / f'{name}.yaml').read_text())
                if name == family:
                    profile.update(overrides)
                (profiles / f'{name}.yaml').write_text(yaml.safe_dump(profile))
            output = root / 'profiles.h'
            result = subprocess.run([sys.executable, str(GENERATOR), str(root), str(output)], capture_output=True, text=True)
            return result, output.read_text() if output.exists() else ''

    def test_existing_profiles_generate(self):
        result, content = self.generate({})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('defaults_ov[]', content)
        self.assertIn('{"timing_profile", 0}', content)
        self.assertIn('s->set_whitebal(s, v)', content)

    def test_unknown_or_invalid_control_rejected(self):
        for override in ({'new_control': 1}, {'contrast': 3}, {'contrast': 0.5}, {'timing_profile': 'unknown'}):
            with self.subTest(override=override):
                result, _ = self.generate(override)
                self.assertNotEqual(result.returncode, 0)

    def test_mid_and_low_defaults(self):
        for name, index in [('balanced', 1), ('low_light', 2)]:
            with self.subTest(profile=name):
                result, content = self.generate({'timing_profile': name})
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('{"timing_profile", ' + str(index) + '}', content)

    def test_gc_controls_and_ranges(self):
        result, content = self.generate({})
        self.assertEqual(result.returncode, 0, result.stderr)
        gc = content.split('defaults_gc[] = {', 1)[1]
        self.assertIn('{"contrast", 0}', gc)
        self.assertNotIn('timing_profile', gc)
        self.assertNotIn('jpeg_quality', gc)
        for override in ({'jpeg_quality': 12}, {'brightness': 0}, {'timing_profile': 'balanced'}, {'sharpness': -3}, {'sharpness': 3}, {'name': 'wrong'}):
            with self.subTest(override=override):
                result, _ = self.generate(override, 'gc')
                self.assertNotEqual(result.returncode, 0)
