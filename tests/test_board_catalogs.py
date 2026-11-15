"""Manufacturer catalogs share pinouts while preserving board identity."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from hydra import compose, initialize_config_dir
import yaml

from esp32_workspace.boards import load_board, load_catalog, write_board_header

ROOT = Path(__file__).resolve().parents[1]


class BoardCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'config/boards').mkdir(parents=True)
        (self.root / 'boards').mkdir()
        (self.root / 'boards/shared.h').write_text(
            '#pragma once\n#define TEST_PIN 12\n'
            'struct BoardConfig { const char *name; };\n'
            'inline constexpr BoardConfig common_board_config{board_display_name};\n')
        self.entries = {'first': {'name': 'First "camera"', 'header': 'shared.h', 'target': 'esp32'},
                        'second': {'name': 'Second camera', 'header': 'shared.h', 'target': 'esp32'}}
        self.save()

    def save(self):
        (self.root / 'config/boards/maker.yaml').write_text(yaml.safe_dump(self.entries))

    def test_shared_header_keeps_names_and_paths_distinct(self):
        for board in self.entries:
            output = self.root / f'{board}.h'
            write_board_header(self.root, board, output)
            self.assertIn(json.dumps(self.entries[board]['name']), output.read_text())
            self.assertIn('boards/shared.h', output.read_text())
            result = subprocess.run(['c++', '-x', 'c++', '-std=c++17', '-fsyntax-only', '-'],
                                    input=f'#include "{output}"\nstatic_assert(TEST_PIN == 12);\n'
                                          f"static_assert(common_board_config.name[0] == '{self.entries[board]['name'][0]}');\n",
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(load_catalog(self.root)), {'first', 'second'})
        with self.assertRaisesRegex(ValueError, 'Unknown board'):
            load_board(self.root, 'missing')

    def test_frame_grabber_must_reference_existing_profile(self):
        profiles = self.root / 'config/frame_grabber'
        profiles.mkdir()
        (profiles / 'ov.yaml').write_text('name: ov\n')
        self.entries['first']['supported_frame_grabber'] = ['ov']
        self.save()
        self.assertEqual(load_board(self.root, 'first')['supported_frame_grabber'], ['ov'])
        (profiles / 'ov.yaml').unlink()
        with self.assertRaisesRegex(ValueError, 'frame grabber'):
            load_catalog(self.root)

    def test_board_images_exist(self):
        catalog = load_catalog(ROOT)
        for board, metadata in catalog.items():
            with self.subTest(board=board):
                self.assertTrue((ROOT / 'config/boards' / metadata['image']).is_file())

    def test_duplicate_board_rejected(self):
        (self.root / 'config/boards/other.yaml').write_text(yaml.safe_dump(self.entries))
        with self.assertRaisesRegex(ValueError, 'Duplicate board'):
            load_catalog(self.root)

    def test_invalid_entries_rejected(self):
        for change in ({'name': ''}, {'target': 'wrong'}, {'header': 'missing.h'},
                       {'header': '../outside.h'}, {'header': '/tmp/outside.h'},
                       {'supported_frame_grabber': ['unknown']}, {'supported_frame_grabber': 'ov'},
                       {'sdkconfig_defaults': ['../outside']}, {'image': 'images/missing.png'},
                       {'image': '../boards/maker.yaml'}, {'image': '/tmp/outside.png'}, {'image': 3}):
            with self.subTest(change=change):
                original = self.entries['first'].copy()
                self.entries['first'].update(change)
                self.save()
                with self.assertRaises(ValueError):
                    load_catalog(self.root)
                self.entries['first'] = original

    def test_cross_manufacturer_board_override(self):
        with initialize_config_dir(version_base='1.3', config_dir=str(ROOT / 'config')):
            config = compose(config_name='build', overrides=['device=esp32_cam', 'board=esp32s3n16r8_espressif_cam'])
            self.assertEqual(config.hardware.target, 'esp32s3')
            self.assertEqual(config.frame_grabber.name, 'ov')
            self.assertEqual(dict(config.hardware), load_board(ROOT, config.board))

    def test_goouuu_device_uses_shared_pinout_and_memory_defaults(self):
        from esp32_workspace.cli import command
        with initialize_config_dir(version_base='1.3', config_dir=str(ROOT / 'config')):
            config = compose(config_name='build', overrides=['device=esp32s3_goouuu'])
            self.assertEqual(config.board, 'esp32s3n16r8_goouuu_cam')
            self.assertEqual(config.hardware.target, 'esp32s3')
            self.assertEqual(config.hardware.supported_frame_grabber, ['ov', 'gc'])
            self.assertEqual(config.frame_grabber.name, 'ov')
            self.assertEqual(dict(config.lens), {})
            self.assertEqual(config.hardware.header, load_board(ROOT, 'esp32s3n16r8_espressif_cam')['header'])
            args = command(ROOT, 'webrtc', config.board, 'build', None, 'ov')
            defaults = next(arg for arg in args if arg.startswith('-DSDKCONFIG_DEFAULTS='))
            self.assertTrue(defaults.endswith(';'+str(ROOT / 'boards/esp32s3_n16r8.defaults')))
            self.assertTrue((ROOT / 'config/boards/images' / f'{config.board}.jpg').is_file())
