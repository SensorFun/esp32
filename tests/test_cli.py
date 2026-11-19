"""Check the boundary between host tooling and ESP-IDF."""
import unittest
from pathlib import Path

from esp32_workspace.cli import command

ROOT = Path(__file__).resolve().parents[1]


class CommandTests(unittest.TestCase):
    def test_boards_get_separate_configs_and_correct_targets(self):
        for board, target in [("esp32_ai-thinker_cam", "esp32"), ("esp32s3n16r8_espressif_cam", "esp32s3")]:
            with self.subTest(board=board):
                cmd = command(ROOT, "webrtc", board, "flash", "/dev/ttyUSB0")
                self.assertIn(f"-DIDF_TARGET={target}", cmd)
                self.assertIn(f"-DSDKCONFIG={ROOT / 'build' / 'webrtc' / board / 'sdkconfig'}", cmd)
                self.assertEqual(cmd[-3:], ["-p", "/dev/ttyUSB0", "flash"])
                self.assertIn(f"-DSDKCONFIG_DEFAULTS={ROOT / 'projects/webrtc/sdkconfig.defaults'}", cmd)

    def test_unknown_inputs_fail_before_invoking_idf(self):
        for project, board in [("missing", "esp32_ai-thinker_cam"), ("webrtc", "missing"), ("../server", "esp32_ai-thinker_cam")]:
            with self.subTest(project=project, board=board), self.assertRaises(ValueError):
                command(ROOT, project, board, "build", None)
