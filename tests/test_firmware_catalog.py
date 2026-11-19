"""config/firmware decides which projects CI builds and the flasher offers."""
from pathlib import Path
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'python'))
from esp32_workspace.boards import load_catalog  # noqa: E402
from esp32_workspace.firmware_catalog import build_pairs, is_private, load_firmware  # noqa: E402


class FirmwareCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'config/boards').mkdir(parents=True)
        (self.root / 'config/firmware').mkdir()
        (self.root / 'config/boards/maker.yaml').write_text(yaml.safe_dump({
            'cam': {'name': 'Cam', 'target': 'esp32s3', 'supported_frame_grabber': ['ov']},
            'plain': {'name': 'Plain', 'target': 'esp32'}}))
        for project in ('camera', 'blink'):
            (self.root / 'projects' / project).mkdir(parents=True)
            (self.root / 'projects' / project / 'CMakeLists.txt').write_text('')
        self.entries = {'camera': {'name': 'Camera', 'description': 'Streams video.', 'boards': 'all', 'requires_frame_grabber': True,
                                   'private_settings': ['CONFIG_SECRET'], 'after_flash': {'text': 'Open it.', 'link': '#/projects/camera'}},
                        'blink': {'name': 'Blink', 'description': 'Blinks.', 'boards': ['plain']}}
        self.save()

    def save(self):
        for project, entry in self.entries.items():
            (self.root / 'config/firmware' / f'{project}.yaml').write_text(yaml.safe_dump(entry))

    def test_real_catalog_offers_every_project_for_existing_boards(self):
        catalog = load_firmware(ROOT)
        boards = load_catalog(ROOT)
        self.assertIn('webrtc', catalog)
        self.assertNotIn('hello_world', catalog)
        for project, entry in catalog.items():
            with self.subTest(project=project):
                self.assertTrue((ROOT / 'projects' / project / 'CMakeLists.txt').is_file())
                self.assertTrue(entry['boards'])
                self.assertLessEqual(set(entry['boards']), set(boards))
        self.assertIn('CONFIG_CAMERA_SIGNALING_API_SECRET', catalog['webrtc']['private_settings'])

    def test_boards_resolve_with_frame_grabber_requirement(self):
        self.assertEqual(build_pairs(self.root), [{'project': 'blink', 'board': 'plain'}, {'project': 'camera', 'board': 'cam'}])
        self.assertEqual(build_pairs(self.root, projects=['camera']), [{'project': 'camera', 'board': 'cam'}])
        self.assertEqual(build_pairs(self.root, boards=['plain']), [{'project': 'blink', 'board': 'plain'}])
        with self.assertRaisesRegex(ValueError, 'Unknown or unflashable project'):
            build_pairs(self.root, projects=['missing'])
        with self.assertRaisesRegex(ValueError, 'Unknown or unflashable board'):
            build_pairs(self.root, boards=['missing'])

    def test_invalid_entries_are_rejected(self):
        for change, message in (({'boards': ['missing']}, 'unknown boards'), ({'boards': 'some'}, 'boards must'),
                                ({'name': ''}, 'name is required'), ({'extra': 1}, 'unknown fields'),
                                ({'private_settings': ['SECRET']}, 'private_settings'),
                                ({'requires_frame_grabber': 'yes'}, 'requires_frame_grabber'),
                                ({'after_flash': {'link': 'javascript:alert(1)'}}, 'after_flash.link'),
                                ({'after_flash': {'color': 'red'}}, 'after_flash takes')):
            with self.subTest(change=change):
                original = dict(self.entries['camera'])
                self.entries['camera'].update(change)
                self.save()
                with self.assertRaisesRegex(ValueError, message):
                    load_firmware(self.root)
                self.entries['camera'] = original
                self.save()

    def test_firmware_needs_a_project_folder_unless_skipped(self):
        (self.root / 'projects/blink/CMakeLists.txt').unlink()
        with self.assertRaisesRegex(ValueError, 'no firmware project'):
            load_firmware(self.root)
        self.assertIn('blink', load_firmware(self.root, check_sources=False))

    def test_private_settings_detection(self):
        settings = ['CONFIG_SECRET', 'CONFIG_TOKEN']
        self.assertFalse(is_private('CONFIG_SECRET=""\n# CONFIG_TOKEN is not set\n', settings))
        self.assertFalse(is_private('CONFIG_OTHER="x"\n', settings))
        self.assertTrue(is_private('CONFIG_SECRET="abc"\n', settings))
        self.assertTrue(is_private('CONFIG_TOKEN=y\n', settings))


if __name__ == '__main__':
    unittest.main()
