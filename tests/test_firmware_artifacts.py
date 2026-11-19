"""Firmware packaging for the browser flasher, without ESP-IDF or Docker."""
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'python'))
from esp32_workspace.boards import load_catalog  # noqa: E402

spec = importlib.util.spec_from_file_location('firmware_artifacts', ROOT / 'tools/firmware_artifacts.py')
artifacts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifacts)


class FirmwareArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'config/boards').mkdir(parents=True)
        (self.root / 'boards').mkdir()
        (self.root / 'boards/pins.h').write_text('#pragma once\n')
        (self.root / 'config/boards/maker.yaml').write_text('test_board:\n  name: Test\n  header: pins.h\n  target: esp32s3\n')
        (self.root / 'config/firmware').mkdir()
        (self.root / 'config/firmware/webrtc.yaml').write_text('name: WebRTC\ndescription: Camera firmware\nboards: all\nprivate_settings: [CONFIG_CAMERA_SIGNALING_API_SECRET]\n')
        (self.root / 'projects/webrtc').mkdir(parents=True)
        (self.root / 'projects/webrtc/CMakeLists.txt').touch()
        self.build = self.root / 'build/webrtc/test_board'
        (self.build / 'bootloader').mkdir(parents=True)
        self.args = {'flash_settings': {'flash_mode': 'dio', 'flash_size': '8MB', 'flash_freq': '80m'},
                     'flash_files': {'0x0': 'bootloader/bootloader.bin', '0x10000': 'webrtc.bin'},
                     'extra_esptool_args': {'chip': 'esp32s3', 'before': 'default-reset', 'after': 'hard-reset', 'stub': True}}
        (self.build / 'flasher_args.json').write_text(json.dumps(self.args))
        (self.build / 'firmware_selection.json').write_text('{"board":"test_board","frame_grabber":"ov"}')
        (self.build / 'bootloader/bootloader.bin').write_bytes(b'b' * 100)
        (self.build / 'webrtc.bin').write_bytes(b'a' * 2048)
        (self.build / 'sdkconfig').write_text('CONFIG_CAMERA_SIGNALING_API_SECRET=""\n')

    def test_board_ids_match_the_validated_catalog(self):
        pairs = artifacts.build_pairs(ROOT)
        self.assertEqual({pair['board'] for pair in pairs}, set(load_catalog(ROOT)))
        self.assertTrue(all(pair['project'] == 'webrtc' for pair in pairs))

    def test_package_copies_only_flashable_files(self):
        (self.build / 'build.ninja').write_text('not flashed')
        out = artifacts.package(self.root, 'webrtc', 'test_board', 'abc123')
        files = sorted(str(path.relative_to(out)) for path in out.rglob('*') if path.is_file())
        self.assertEqual(files, ['bootloader/bootloader.bin', 'build.json', 'firmware.bin', 'firmware_selection.json', 'flasher_args.json', 'webrtc.bin'])
        info = json.loads((out / 'build.json').read_text())
        self.assertEqual((info['board'], info['target'], info['commit'], info['private']), ('test_board', 'esp32s3', 'abc123', False))
        self.assertEqual((out / 'webrtc.bin').read_bytes(), b'a' * 2048)
        merged = (out / 'firmware.bin').read_bytes()
        self.assertEqual(merged[:100], b'b' * 100)
        self.assertEqual(merged[100:0x10000], b'\xff' * (0x10000-100))
        self.assertEqual(merged[0x10000:], b'a' * 2048)

    def test_secret_marks_package_private_without_copying_sdkconfig(self):
        (self.build / 'sdkconfig').write_text('CONFIG_CAMERA_SIGNALING_API_SECRET="not-a-real-secret"\n')
        out = artifacts.package(self.root, 'webrtc', 'test_board', 'abc')
        self.assertTrue(json.loads((out / 'build.json').read_text())['private'])
        self.assertFalse((out / 'sdkconfig').exists())

    def test_release_packaging_refuses_debug_builds(self):
        (self.build / 'sdkconfig').write_text('CONFIG_COMPILER_OPTIMIZATION_DEBUG=y\n')
        artifacts.package(self.root, 'webrtc', 'test_board', 'abc')  # Local packaging allows it.
        with self.assertRaisesRegex(ValueError, 'not a release build'):
            artifacts.package(self.root, 'webrtc', 'test_board', 'abc', release=True)
        (self.build / 'sdkconfig').write_text('CONFIG_COMPILER_OPTIMIZATION_PERF=y\n')
        artifacts.package(self.root, 'webrtc', 'test_board', 'abc', release=True)

    def test_every_project_and_ci_build_in_release_mode(self):
        for defaults in sorted((ROOT / 'projects').glob('*/sdkconfig.defaults')):
            with self.subTest(project=defaults.parent.name):
                self.assertIn(artifacts.RELEASE_SETTING, defaults.read_text().splitlines())
        workflow = (ROOT / '.github/workflows/site-pages.yml').read_text()
        self.assertRegex(workflow, r'tools/firmware_artifacts\.py [^\n]*--release')

    def test_invalid_builds_are_rejected(self):
        cases = {
            'chip': lambda: (self.build / 'flasher_args.json').write_text(json.dumps({**self.args, 'extra_esptool_args': {**self.args['extra_esptool_args'], 'chip': 'esp32'}})),
            'does not belong': lambda: (self.build / 'firmware_selection.json').write_text('{"board":"other"}'),
            'Missing or invalid': lambda: (self.build / 'flasher_args.json').write_text(json.dumps({**self.args, 'flash_files': {'0x0': '../../../outside.bin'}})),
            'not built': lambda: (self.build / 'flasher_args.json').unlink(),
        }
        original = {name: (self.build / name).read_text() for name in ('flasher_args.json', 'firmware_selection.json')}
        for message, damage in cases.items():
            with self.subTest(message=message):
                damage()
                with self.assertRaisesRegex(ValueError, message):
                    artifacts.package(self.root, 'webrtc', 'test_board', 'abc')
                for name, text in original.items():
                    (self.build / name).write_text(text)

    def test_host_build_mirrors_ci_with_read_only_sources(self):
        root = self.root
        (root / 'projects/webrtc').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'projects/webrtc/dependencies.lock', root / 'projects/webrtc/dependencies.lock')
        with patch.object(artifacts.subprocess, 'call', return_value=0) as call:
            artifacts.build_in_docker(root, 'webrtc', 'test_board', 'abc')
        command = call.call_args.args[0]
        self.assertEqual(command[:3], ['docker', 'run', '--rm'])
        self.assertIn(artifacts.IDF_IMAGE, command)
        mounts = [command[i + 1] for i, arg in enumerate(command) if arg == '--mount']
        writable = [mount for mount in mounts if not mount.endswith(',readonly')]
        self.assertEqual(sorted(mount.split('target=/workspace/')[1] for mount in writable),
                         ['build', 'projects/webrtc/dependencies.lock', 'projects/webrtc/managed_components'])
        self.assertIn(f'type=bind,source={root / "projects/webrtc"},target=/workspace/projects/webrtc,readonly', mounts)
        self.assertNotIn('-p', command)
        self.assertEqual(command[-3:], ['webrtc', 'test_board', 'abc'])

    def test_ci_mode_builds_fresh_and_copies_the_package(self):
        root = self.root
        (root / 'projects/webrtc').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'projects/webrtc/dependencies.lock', root / 'projects/webrtc/dependencies.lock')
        stale = root / 'build/ci/webrtc/test_board/sdkconfig'
        stale.parent.mkdir(parents=True)
        stale.write_text('CONFIG_CAMERA_SIGNALING_API_SECRET="stale"\n')

        def container(command, cwd):
            self.assertFalse(stale.exists())
            build = next(m for m in command if m.endswith('target=/workspace/build'))
            self.assertIn(f'source={root / "build/ci"},', build)
            package = root / 'build/ci/firmware/webrtc/test_board'
            package.mkdir(parents=True)
            (package / 'build.json').write_text('{"private": false}')
            return 0
        with patch.object(artifacts.subprocess, 'call', side_effect=container) as call:
            artifacts.build_in_docker(root, 'webrtc', 'test_board', 'abc', ci=True)
        self.assertTrue(any(arg.endswith(' --release') for arg in call.call_args.args[0]))
        self.assertEqual((root / 'build/firmware/webrtc/test_board/build.json').read_text(), '{"private": false}')


if __name__ == '__main__':
    unittest.main()
