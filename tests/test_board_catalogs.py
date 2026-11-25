"""Manufacturer catalogs share pinouts while preserving board identity."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from hydra import compose, initialize_config_dir
import yaml

from esp32_workspace.boards import load_board, load_catalog, write_board_header, write_board_sdkconfig

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
        tuning = {'wifi_tx_buffers': {'type': 'static', 'count': 16}, 'lwip_iram': False, 'camera_dma_buffer': 32768}
        self.entries = {'first': {'name': 'First "camera"', 'header': 'shared.h', 'target': 'esp32', 'flash_freq': '80m',
                                  'psram_freq': '40m', **tuning},
                        'second': {'name': 'Second camera', 'header': 'shared.h', 'target': 'esp32', 'flash_freq': '40m', **tuning}}
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

    def test_board_settings_replace_stale_sdkconfig_values(self):
        build = self.root / 'build/webrtc/first'
        build.mkdir(parents=True)
        sdkconfig = build / 'sdkconfig'
        sdkconfig.write_text('CONFIG_A=y\n# CONFIG_ESPTOOLPY_FLASHFREQ_80M is not set\nCONFIG_ESPTOOLPY_FLASHFREQ_40M=y\n'
                             'CONFIG_ESPTOOLPY_FLASHFREQ="40m"\nCONFIG_ESPTOOLPY_FLASHMODE="dio"\n'
                             'CONFIG_SPIRAM_SPEED_80M=y\nCONFIG_SPIRAM_SPEED=80\nCONFIG_SPIRAM=y\n'
                             'CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y\nCONFIG_ESP_WIFI_TX_BUFFER_TYPE=0\nCONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16\n'
                             'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32\n# CONFIG_LWIP_IRAM_OPTIMIZATION is not set\nCONFIG_SECRET="kept"\n')
        defaults = write_board_sdkconfig(self.root, 'first', build)
        self.assertEqual(defaults.read_text().splitlines()[1:], [
            'CONFIG_ESPTOOLPY_FLASHFREQ_80M=y', 'CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y', 'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16',
            '# CONFIG_LWIP_IRAM_OPTIMIZATION is not set', 'CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=32768', 'CONFIG_SPIRAM_SPEED_40M=y'])
        # Disagreeing option families (flash and PSRAM clocks) are removed so ESP-IDF re-reads them
        # from the board defaults; matching families and every other setting are kept.
        self.assertEqual(sdkconfig.read_text(), 'CONFIG_A=y\nCONFIG_ESPTOOLPY_FLASHMODE="dio"\nCONFIG_SPIRAM=y\n'
                         'CONFIG_ESP_WIFI_STATIC_TX_BUFFER=y\nCONFIG_ESP_WIFI_TX_BUFFER_TYPE=0\nCONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16\n'
                         'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32\n# CONFIG_LWIP_IRAM_OPTIMIZATION is not set\nCONFIG_SECRET="kept"\n')
        # Switching to dynamic buffers and lwIP IRAM replaces those families too.
        self.entries['first'].update(wifi_tx_buffers={'type': 'dynamic', 'count': 32}, lwip_iram=True)
        self.save()
        write_board_sdkconfig(self.root, 'first', build)
        self.assertEqual(sdkconfig.read_text(), 'CONFIG_A=y\nCONFIG_ESPTOOLPY_FLASHMODE="dio"\nCONFIG_SPIRAM=y\n'
                         'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32\nCONFIG_SECRET="kept"\n')
        self.assertIn('CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32', (build / 'board.defaults').read_text())
        # An sdkconfig that already matches every board value is left untouched.
        current = ('CONFIG_ESPTOOLPY_FLASHFREQ_80M=y\nCONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y\nCONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32\n'
                   'CONFIG_LWIP_IRAM_OPTIMIZATION=y\nCONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=32768\nCONFIG_SPIRAM_SPEED_40M=y\nCONFIG_SPIRAM_SPEED=40\n')
        sdkconfig.write_text(current)
        write_board_sdkconfig(self.root, 'first', build)
        self.assertEqual(sdkconfig.read_text(), current)

    def test_camera_psram_dma_on_s3_only(self):
        self.entries['first'].update(target='esp32s3', flash_freq='80m', psram_freq='80m', camera_psram_dma=True)
        self.entries['second'].update(target='esp32s3', flash_freq='80m')
        self.save()
        for board, line in (('first', 'CONFIG_CAMERA_PSRAM_DMA=y'), ('second', '# CONFIG_CAMERA_PSRAM_DMA is not set')):
            build = self.root / 'build/webrtc' / board
            self.assertIn(line, write_board_sdkconfig(self.root, board, build).read_text().splitlines())

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
                       {'image': '../boards/maker.yaml'}, {'image': '/tmp/outside.png'}, {'image': 3},
                       {'flash_freq': None}, {'flash_freq': '120m'}, {'flash_freq': 80},
                       {'psram_freq': '120m'}, {'flash_freq': '40m', 'psram_freq': '80m'},
                       {'wifi_tx_buffers': None}, {'wifi_tx_buffers': {'type': 'static', 'count': 65}},
                       {'wifi_tx_buffers': {'type': 'cached', 'count': 16}}, {'wifi_tx_buffers': {'type': 'static', 'count': True}},
                       {'wifi_tx_buffers': {'type': 'static'}}, {'lwip_iram': 'yes'}, {'lwip_iram': None},
                       {'camera_dma_buffer': None}, {'camera_dma_buffer': 4096}, {'camera_dma_buffer': 65536}, {'camera_dma_buffer': '16k'},
                       {'camera_psram_dma': True}, {'camera_psram_dma': 'yes'}):
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
            self.assertTrue(defaults.endswith(';'.join(['', str(ROOT / 'boards/esp32s3_n16r8.defaults'),
                                                        str(ROOT / 'build/webrtc' / config.board / 'board.defaults')])))
            self.assertTrue((ROOT / 'config/boards/images' / f'{config.board}.jpg').is_file())
