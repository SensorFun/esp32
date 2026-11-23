"""Camera-profile generation fails closed rather than dropping or widening settings."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'projects/webrtc/main/generate_profiles.py'
PROFILES = ROOT / 'config/frame_grabber'


def load(sensor):
    return yaml.safe_load((PROFILES / 'sensors' / f'{sensor}.yaml').read_text())


class CameraProfileTests(unittest.TestCase):
    def generate(self, sensor=None, change=None, families=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / 'config/frame_grabber'
            shutil.copytree(PROFILES, target)
            if sensor:
                profile = load(sensor)
                change(profile)
                (target / 'sensors' / f'{sensor}.yaml').write_text(yaml.safe_dump(profile))
            for family, sensors in (families or {}).items():
                (target / f'{family}.yaml').write_text(yaml.safe_dump({'name': family, 'sensors': sensors}))
            output = root / 'profiles.h'
            result = subprocess.run([sys.executable, str(GENERATOR), str(root), str(output)], capture_output=True, text=True)
            return result, output.read_text() if output.exists() else ''

    def section(self, content, sensor):
        return content.split(f'controls_{sensor}[] = {{', 1)[1].split('};', 1)[0]

    def test_each_sensor_has_its_own_profile(self):
        result, content = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('{"ov2640", 1, controls_ov2640', content)
        self.assertIn('{"ov3660", 1, controls_ov3660', content)
        self.assertIn('{"gc0308", -1, controls_gc0308', content)
        self.assertIn('s->set_whitebal(s, v)', content)
        ov2640, ov3660, gc0308 = (self.section(content, s) for s in ('ov2640', 'ov3660', 'gc0308'))
        # The OV2640 driver implements neither sharpness nor denoise.
        self.assertNotIn('"sharpness"', ov2640)
        self.assertNotIn('"denoise"', ov2640)
        self.assertIn('{"denoise", Kind::list, 1, 0, 8, options_ov3660_denoise, 9}', ov3660)
        self.assertNotIn('"jpeg_quality"', gc0308)
        self.assertNotIn('"brightness"', gc0308)

    def test_driver_ranges_per_sensor(self):
        _, content = self.generate()
        ov2640, ov3660, gc0308 = (self.section(content, s) for s in ('ov2640', 'ov3660', 'gc0308'))
        self.assertIn('{"brightness", Kind::list, 0, -2, 2,', ov2640)
        self.assertIn('{"brightness", Kind::list, 0, -3, 3,', ov3660)
        self.assertIn('{"saturation", Kind::list, 0, -4, 4,', ov3660)
        self.assertIn('{"ae_level", Kind::list, 0, -5, 5,', ov3660)
        self.assertIn('{"agc_gain", Kind::range, 0, 0, 30, nullptr, 0}', ov2640)
        self.assertIn('{"agc_gain", Kind::range, 0, 0, 64, nullptr, 0}', ov3660)
        self.assertIn('{"aec_value", Kind::range, 300, 0, 1536, nullptr, 0}', ov3660)
        # OV2640 uses the 2x-128x scale; OV3660 takes a raw ceiling (reset value 248).
        self.assertIn('{"gain_ceiling", Kind::list, 0, 0, 6, options_ov2640_gain_ceiling, 7}', ov2640)
        self.assertIn('{"gain_ceiling", Kind::range, 248, 0, 255, nullptr, 0}', ov3660)
        self.assertIn('{"agc_gain", Kind::range, 10, 0, 30, nullptr, 0}', gc0308)
        self.assertIn('{"sharpness", Kind::list, 0, -2, 2,', gc0308)

    def test_switches_lists_and_labels(self):
        _, content = self.generate()
        self.assertIn('{"automatic_gain", Kind::toggle, 1, 0, 1, options_ov2640_automatic_gain, 2}', content)
        self.assertIn('options_ov2640_automatic_gain[] = {{0, "Off"}, {1, "On"}}', content)
        self.assertIn('options_gc0308_white_balance_mode[] = {{0, "Auto"}, {1, "Sunny"}, {2, "Cloudy"}, {3, "Office"}, {4, "Home"}}', content)
        self.assertIn('options_ov2640_contrast[] = {{-2, "-2"}, {-1, "-1"}, {0, "0"}, {1, "+1"}, {2, "+2"}}', content)
        self.assertIn('options_ov2640_gain_ceiling[] = {{0, "2x"}', content)

    def test_generated_header_compiles(self):
        if not shutil.which('c++'):
            self.skipTest('Native C++ compiler required')
        _, content = self.generate()
        source = ('#include <cstddef>\nstruct sensor_t { struct { int brightness, contrast, saturation, sharpness, denoise, quality, gainceiling, '
                  'colorbar, hmirror, vflip, awb, awb_gain, wb_mode, agc, agc_gain, aec, aec_value, ae_level, aec2, dcw, bpc, wpc, raw_gma, lenc; } status;'
                  + ''.join(f' int (*set_{m})(sensor_t *, int);' for m in ('brightness', 'contrast', 'saturation', 'sharpness', 'denoise', 'quality',
                            'colorbar', 'hmirror', 'vflip', 'whitebal', 'awb_gain', 'wb_mode', 'gain_ctrl', 'agc_gain', 'exposure_ctrl',
                            'aec_value', 'ae_level', 'aec2', 'dcw', 'bpc', 'wpc', 'raw_gma', 'lenc'))
                  + ' int (*set_gainceiling)(sensor_t *, int); };\nusing gainceiling_t = int;\n'
                  'struct Control { const char *name; int (*set)(sensor_t *, int); int (*get)(sensor_t *); };\n' + content
                  + 'static_assert(sizeof(profiles) / sizeof(profiles[0]) == 3);\n')
        result = subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-fsyntax-only', '-Wall', '-Werror', '-Wno-unused-variable', '-'],
                                input=source, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_profiles_rejected(self):
        def control(name, spec):
            return lambda profile: profile['controls'].__setitem__(name, spec)
        cases = {
            'unknown control': ('ov2640', control('new_control', {'default': 0, 'switch': True})),
            'default not allowed': ('ov2640', control('contrast', {'default': 3, 'options': [-2, -1, 0, 1, 2]})),
            'fractional default': ('ov2640', control('contrast', {'default': 0.5, 'options': [-2, -1, 0, 1, 2]})),
            'default outside range': ('ov2640', control('aec_value', {'default': 1300, 'range': [0, 1200]})),
            'two kinds': ('ov2640', control('contrast', {'default': 0, 'options': [0, 1], 'range': [0, 1]})),
            'no kind': ('ov2640', control('contrast', {'default': 0})),
            'unsorted options': ('ov2640', control('contrast', {'default': 0, 'options': [1, 0]})),
            'one option': ('ov2640', control('contrast', {'default': 0, 'options': [0]})),
            'unquoted Off label': ('ov3660', control('denoise', {'default': 0, 'options': {0: False, 1: '1'}})),
            'reversed range': ('ov2640', control('aec_value', {'default': 300, 'range': [1200, 0]})),
            'unknown timing': ('ov2640', lambda profile: profile.__setitem__('timing_profile', 'unknown')),
            'GC timing': ('gc0308', lambda profile: profile.__setitem__('timing_profile', 'mid_light')),
            'OV without timing': ('ov3660', lambda profile: profile.pop('timing_profile')),
            'wrong name': ('gc0308', lambda profile: profile.__setitem__('name', 'gc')),
            'unknown key': ('gc0308', lambda profile: profile.__setitem__('extra', 1)),
        }
        for label, (sensor, change) in cases.items():
            with self.subTest(label):
                result, _ = self.generate(sensor, change)
                self.assertNotEqual(result.returncode, 0)
        with self.subTest('sensor missing from families'):
            result, _ = self.generate(families={'ov': ['ov2640']})
            self.assertNotEqual(result.returncode, 0)

    def test_supported_timing_defaults(self):
        for name, index in [('high_light', 0), ('mid_light', 1), ('low_light', 2)]:
            with self.subTest(profile=name):
                result, content = self.generate('ov2640', lambda profile: profile.__setitem__('timing_profile', name))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('{"ov2640", ' + str(index) + ', controls_ov2640', content)


class SharedSettingsPanelTests(unittest.TestCase):
    def test_one_settings_component_everywhere(self):
        # The board's page, the website and the firmware all use one file.
        component = 'projects/common/camera_ui/settings.js'
        defined = [path for path in list((ROOT / 'projects').rglob('*.js')) + list((ROOT / 'projects').rglob('*.html')) +
                   list((ROOT / 'site/src').rglob('*.js*')) + list((ROOT / 'site/src').rglob('*.mjs'))
                   if 'managed_components' not in path.parts and "customElements.define('sensorfan-camera-settings'" in path.read_text()]
        self.assertEqual([path.relative_to(ROOT).as_posix() for path in defined], [component])
        self.assertIn('"../../common/camera_ui/settings.js"', (ROOT / 'projects/webrtc/main/CMakeLists.txt').read_text())
        viewer = (ROOT / 'projects/webrtc/main/viewer.html').read_text()
        self.assertIn('src="/camera-settings.js"', viewer)
        self.assertIn('<sensorfan-camera-settings', viewer)
        remote = (ROOT / 'site/src/projects/webrtc/RemoteCamera.jsx').read_text()
        self.assertIn("import '../../../../projects/common/camera_ui/settings.js';", remote)
        self.assertIn('<sensorfan-camera-settings', remote)

    def test_panel_renders_controls_by_kind(self):
        script = (ROOT / 'projects/common/camera_ui/settings.js').read_text()
        self.assertIn("c.kind === 'switch'", script)
        self.assertIn("setAttribute('role', 'switch')", script)
        self.assertIn("c.kind === 'list'", script)
        self.assertIn('Number(control.checked)', script)
