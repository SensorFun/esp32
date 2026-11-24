"""config/motion/motion.yaml supplies the firmware's motion and recording defaults."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'projects/webrtc/main'))
import generate_motion  # noqa: E402


class MotionConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / 'config/motion').mkdir(parents=True)
        shutil.copy(ROOT / 'config/motion/motion.yaml', self.root / 'config/motion/motion.yaml')
        self.data = yaml.safe_load((self.root / 'config/motion/motion.yaml').read_text())

    def tearDown(self):
        self.temp.cleanup()

    def generate(self):
        (self.root / 'config/motion/motion.yaml').write_text(yaml.safe_dump(self.data))
        return generate_motion.generate(self.root)

    def test_workspace_defaults_are_embedded(self):
        header = generate_motion.generate(ROOT)
        self.assertIn('constexpr float min_area = 1.0f;', header)
        self.assertIn('constexpr float min_speed = 4.0f;', header)
        self.assertIn('constexpr Range min_area_range{1.0, 30.0, 0.5};', header)
        self.assertIn('constexpr int cell_options[] = {5, 8, 10, 20};', header)
        self.assertIn('constexpr bool record = true;', header)

    def test_edited_default_reaches_the_header(self):
        self.data['recording']['post_seconds']['default'] = 30
        self.assertIn('constexpr int post_seconds = 30;', self.generate())

    def test_invalid_values_fail_the_build(self):
        cases = {
            'off grid': ('detection', 'min_area', {'default': 1.2, 'range': [1, 30, 0.5]}),
            'out of range': ('detection', 'trigger_seconds', {'default': 11, 'range': [0.1, 10.9, 0.1]}),
            'not an option': ('detection', 'cell', {'default': 7, 'options': [5, 8, 10, 20]}),
            'switch default': ('recording', 'record', {'default': 1, 'switch': True}),
            'number as switch': ('detection', 'blur', {'default': True, 'switch': True}),
            'fractional integer': ('recording', 'post_seconds', {'default': 10, 'range': [1, 300, 0.5]}),
            'two kinds': ('detection', 'min_texture', {'default': 5, 'range': [0, 50, 0.5], 'options': [5]}),
        }
        for name, (group, key, spec) in cases.items():
            with self.subTest(name):
                original = self.data[group][key]
                self.data[group][key] = spec
                with self.assertRaises(ValueError):
                    self.generate()
                self.data[group][key] = original

    def test_missing_and_unknown_settings_fail(self):
        del self.data['detection']['blur']
        with self.assertRaisesRegex(ValueError, 'blur'):
            self.generate()
        self.data['detection']['blur'] = {'default': 1, 'range': [0, 2, 1]}
        self.data['recording']['record_fps'] = {'default': 5, 'range': [1, 30, 1]}
        with self.assertRaisesRegex(ValueError, 'record_fps'):
            self.generate()


if __name__ == '__main__':
    unittest.main()
