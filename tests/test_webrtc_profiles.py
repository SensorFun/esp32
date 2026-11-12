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
    def generate(self, overrides):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profiles = root / 'config/frame_grabber'
            profiles.mkdir(parents=True)
            for sensor in ('ov2640', 'ov3660'):
                profile = yaml.safe_load((ROOT / 'config/frame_grabber' / f'{sensor}.yaml').read_text())
                if sensor == 'ov3660':
                    profile.update(overrides)
                (profiles / f'{sensor}.yaml').write_text(yaml.safe_dump(profile))
            output = root / 'profiles.h'
            result = subprocess.run([sys.executable, str(GENERATOR), str(root), str(output)], capture_output=True, text=True)
            return result, output.read_text() if output.exists() else ''

    def test_existing_profiles_generate(self):
        result, content = self.generate({})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('defaults_ov2640', content)
        self.assertIn('defaults_ov3660', content)
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
