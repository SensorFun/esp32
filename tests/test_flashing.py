"""Validate host flashing without accessing serial hardware."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from omegaconf import OmegaConf

from esp32_workspace.flashing import detect_port, run_flash


def serial_port(device, serial="", vid=0x1A86):
    return SimpleNamespace(device=device, vid=vid, pid=0x55D3,
                           product="USB Single Serial", serial_number=serial)


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / "projects/hello_world").mkdir(parents=True)
        (self.root / "projects/hello_world/CMakeLists.txt").touch()
        (self.root / "boards").mkdir()
        (self.root / "config/board").mkdir(parents=True)
        (self.root / "config/board/test.yaml").write_text("board: test\nhardware:\n  target: esp32s3\n")
        self.build = self.root / "build/hello_world/test"
        self.build.mkdir(parents=True)
        self.metadata = {
            "flash_files": {"0x0": "bootloader.bin", "0x10000": "app.bin"},
            "extra_esptool_args": {"chip": "esp32s3", "before": "default-reset", "after": "hard-reset", "stub": True},
        }
        (self.build / "flasher_args.json").write_text(json.dumps(self.metadata))
        (self.build / "flash_args").write_text("--flash-mode dio\n0x0 bootloader.bin\n0x10000 app.bin\n")
        (self.build / "bootloader.bin").touch()
        (self.build / "app.bin").touch()
        self.config = OmegaConf.create(dict(project="hello_world", board="test", port=None, baud=460800, dry_run=False))

    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_single_supported_usb_port(self, ports):
        ports.return_value = [serial_port("/dev/ttyACM1"), serial_port("/dev/ttyS0", vid=None)]
        self.assertEqual(detect_port(self.root, "test"), "/dev/ttyACM1")

    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_board_identity_disambiguates(self, ports):
        ports.return_value = [serial_port("/dev/ttyUSB0", "other"), serial_port("/dev/ttyUSB1", "mine")]
        (self.root / "boards/programming_ports.tsv").write_text("# USB identities\ntest\t1a86:55d3\tUSB Single Serial\tmine\n")
        self.assertEqual(detect_port(self.root, "test"), "/dev/ttyUSB1")

    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_missing_or_ambiguous_ports_fail(self, ports):
        for connected, message in [([], "No supported"), ([serial_port("COM1"), serial_port("COM2")], "Multiple")]:
            with self.subTest(connected=connected):
                ports.return_value = connected
                with self.assertRaisesRegex(ValueError, message):
                    detect_port(self.root, "test")

    @patch("esp32_workspace.flashing.subprocess.call", return_value=19)
    @patch("esp32_workspace.flashing.list_ports.comports", return_value=[serial_port("/dev/ttyACM1")])
    def test_auto_flash_uses_host_esptool_and_generated_arguments(self, ports, call):
        self.assertEqual(run_flash(self.root, self.config), 19)
        args = call.call_args.args[0]
        self.assertEqual(args[:3], [sys.executable, "-m", "esptool"])
        self.assertEqual(args[args.index("--port") + 1], "/dev/ttyACM1")
        self.assertEqual(args[-2:], ["write-flash", "@flash_args"])
        self.assertEqual(call.call_args.kwargs["cwd"], self.build)
        self.assertNotIn("docker", args)
        self.assertNotIn("idf.py", args)

    @patch("esp32_workspace.flashing.subprocess.call")
    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_explicit_port_and_dry_run_never_access_hardware(self, ports, call):
        self.config.port = "/dev/serial device;literal"
        self.config.dry_run = True
        self.assertEqual(run_flash(self.root, self.config), 0)
        ports.assert_not_called()
        call.assert_not_called()

    @patch("esp32_workspace.flashing.subprocess.call", return_value=0)
    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_explicit_port_is_forwarded_unchanged(self, ports, call):
        self.config.port = "COM7"
        self.assertEqual(run_flash(self.root, self.config), 0)
        args = call.call_args.args[0]
        self.assertEqual(args[args.index("--port") + 1], "COM7")
        ports.assert_not_called()

    @patch("esp32_workspace.flashing.subprocess.call")
    def test_missing_artifact_fails_before_serial_detection(self, call):
        (self.build / "app.bin").unlink()
        with self.assertRaisesRegex(ValueError, "artifact"):
            run_flash(self.root, self.config)
        call.assert_not_called()

    @patch("esp32_workspace.flashing.subprocess.call")
    def test_wrong_chip_and_missing_build_fail(self, call):
        self.metadata["extra_esptool_args"]["chip"] = "esp32"
        metadata = self.build / "flasher_args.json"
        metadata.write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError, "chip"):
            run_flash(self.root, self.config)
        metadata.unlink()
        with self.assertRaisesRegex(ValueError, "not built"):
            run_flash(self.root, self.config)
        call.assert_not_called()

    @patch("esp32_workspace.flashing.subprocess.call")
    def test_wrong_camera_build_cannot_be_flashed(self, call):
        (self.root / "config/board/test.yaml").write_text("board: test\nhardware:\n  target: esp32s3\n  camera_sensors: [ov3660]\n")
        self.config.camera_sensor = "ov3660"
        selection = self.build / "firmware_selection.json"
        with self.assertRaisesRegex(ValueError, "metadata is missing"):
            run_flash(self.root, self.config)
        selection.write_text('{"board":"test","camera":"ov2640"}')
        with self.assertRaisesRegex(ValueError, "does not match"):
            run_flash(self.root, self.config)
        call.assert_not_called()

    @patch("esp32_workspace.flashing.list_ports.comports")
    def test_two_connected_board_profiles_select_distinct_ports(self, ports):
        root = Path(__file__).resolve().parents[1]
        ports.return_value = [
            SimpleNamespace(device="/dev/ttyUSB0",vid=0x0403,pid=0x6001,product="FT232R USB UART",serial_number=""),
            SimpleNamespace(device="/dev/ttyUSB1",vid=0x1A86,pid=0x7523,product="USB Serial",serial_number=""),
        ]
        self.assertEqual(detect_port(root, "esp32_ai_thinker_cam"), "/dev/ttyUSB0")
        self.assertEqual(detect_port(root, "esp32s3n16r8_espressif_cam"), "/dev/ttyUSB1")

    def test_flash_config_can_be_inspected_without_docker(self):
        root = Path(__file__).resolve().parents[1]
        result = subprocess.run([sys.executable, str(root / "tools/flash.py"), "--cfg", "job"],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("port: null", result.stdout)
        self.assertIn("baud: 460800", result.stdout)
