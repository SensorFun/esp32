"""Check the boundary between host tooling and ESP-IDF."""
import unittest
from pathlib import Path

from esp32_workspace.cli import command

ROOT = Path(__file__).resolve().parents[1]


class CommandTests(unittest.TestCase):
    def test_boards_get_separate_configs_and_correct_targets(self):
        for board, target in [("esp32_ai_thinker_cam", "esp32"), ("esp32s3_seeed_cam", "esp32s3"), ("esp32p4_waveshare_wifi6_dev_kit", "esp32p4")]:
            with self.subTest(board=board):
                cmd = command(ROOT, "hello_world", board, "flash", "/dev/ttyUSB0")
                self.assertIn(f"-DIDF_TARGET={target}", cmd)
                self.assertIn(f"-DSDKCONFIG={ROOT / 'build' / 'hello_world' / board / 'sdkconfig'}", cmd)
                self.assertEqual(cmd[-3:], ["-p", "/dev/ttyUSB0", "flash"])
                self.assertIn(f"-DSDKCONFIG_DEFAULTS={ROOT / 'projects/hello_world/sdkconfig.defaults'}", cmd)

    def test_unknown_inputs_fail_before_invoking_idf(self):
        for project, board in [("missing", "esp32s3_seeed_cam"), ("hello_world", "missing"), ("../server", "esp32s3_seeed_cam")]:
            with self.subTest(project=project, board=board), self.assertRaises(ValueError):
                command(ROOT, project, board, "build", None)
