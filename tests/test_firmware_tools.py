"""Exercise Hydra composition and the subprocess boundary without hardware."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FirmwareToolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.scratch = Path(self.directory.name)
        self.log = self.scratch / "idf-args.json"
        fake = self.scratch / "idf.py"
        fake.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "Path(os.environ['IDF_TEST_LOG']).write_text(json.dumps(sys.argv[1:]))\n"
            "sys.exit(int(os.environ['IDF_TEST_EXIT']))\n"
        )
        fake.chmod(0o755)
        self.env = os.environ.copy()
        self.env.update(
            PATH=str(self.scratch) + os.pathsep + self.env["PATH"],
            PYTHONPATH=str(ROOT / "python"),
            IDF_TEST_LOG=str(self.log),
            IDF_TEST_EXIT="0",
            ESP32_IN_DOCKER="1",
        )

    def run_tool(self, tool, *overrides):
        return subprocess.run(
            [sys.executable, str(ROOT / "tools" / f"{tool}.py"), *overrides],
            cwd=self.scratch, env=self.env, capture_output=True, text=True,
        )

    def test_build_override_from_another_directory(self):
        result = self.run_tool("build", "board=esp32_ai_thinker_cam")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads(self.log.read_text())
        self.assertIn("-DIDF_TARGET=esp32", args)
        self.assertIn(str(ROOT / "build/hello_world/esp32_ai_thinker_cam"), args)
        self.assertEqual(args[-1], "build")

    def test_dry_run_does_not_execute_idf(self):
        result = self.run_tool("build", "dry_run=true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("idf.py", result.stdout)
        self.assertFalse(self.log.exists())

    def test_device_profiles_compose_frame_grabber_and_lens_directly(self):
        from hydra import compose, initialize_config_dir
        with initialize_config_dir(version_base="1.3", config_dir=str(ROOT / "config")):
            ai = compose(config_name="build", overrides=["device=esp32_cam_ov2640_62"])
            self.assertEqual(ai.board, "esp32_ai_thinker_cam")
            self.assertEqual(ai.hardware.target, "esp32")
            self.assertEqual(ai.camera_sensor, "ov2640")
            self.assertIn("lens", ai)
            self.assertNotIn("camera", ai)
            s3 = compose(config_name="build", overrides=["device=esp32s3_wroom_ov3660_62"])
            self.assertEqual(s3.board, "esp32s3n16r8_espressif_cam")
            self.assertEqual(s3.hardware.target, "esp32s3")
            self.assertEqual(s3.camera_sensor, "ov3660")
            self.assertEqual(s3.frame_grabber.timing_profile, "high_light")
            self.assertEqual(s3.frame_grabber.vertical_flip, 1)
            self.assertIn("lens", s3)
            self.assertNotIn("camera", s3)

    def test_both_device_profiles_run_in_one_hydra_sweep(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/build.py"), "--multirun",
             "device=esp32_cam_ov2640_62,esp32s3_wroom_ov3660_62", "dry_run=true"],
            cwd=self.scratch, env=self.env, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for selection in ("-DIDF_TARGET=esp32 ", "-DIDF_TARGET=esp32s3 ",
                          "-DCAMERA_SENSOR=ov2640", "-DCAMERA_SENSOR=ov3660"):
            self.assertIn(selection, result.stdout)
        self.assertFalse(self.log.exists())

    def test_incompatible_camera_fails_before_idf(self):
        result = self.run_tool("build", "device=esp32_cam_ov2640_62", "frame_grabber=ov3660")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not supported", result.stderr)
        self.assertFalse(self.log.exists())

    def test_idf_failure_status_is_preserved(self):
        self.env["IDF_TEST_EXIT"] = "19"
        result = self.run_tool("build")
        self.assertEqual(result.returncode, 19, result.stderr)

    def test_host_bootstrap_builds_and_runs_in_docker(self):
        self.env.pop("ESP32_IN_DOCKER", None)
        # Intercept Docker itself; the real launcher must still supply the host
        # identity, build the image, and forward the script's Hydra overrides.
        fake = self.scratch / "docker"
        fake.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "log = Path(os.environ['IDF_TEST_LOG'])\n"
            "calls = json.loads(log.read_text()) if log.exists() else []\n"
            "calls.append(sys.argv[1:])\n"
            "log.write_text(json.dumps(calls))\n"
            "sys.exit(17 if 'run' in sys.argv else 0)\n"
        )
        fake.chmod(0o755)
        for tool in ("build",):
            with self.subTest(tool=tool):
                self.log.unlink(missing_ok=True)
                result = self.run_tool(tool, "project=hello_world", "board=esp32_ai_thinker_cam", "dry_run=true")
                self.assertEqual(result.returncode, 17, result.stderr)
                calls = json.loads(self.log.read_text())
                self.assertEqual(len(calls), 2)
                self.assertIn("build", calls[0])
                self.assertIn("run", calls[1])
                self.assertEqual(calls[1][-5:], ["python", f"/workspace/tools/{tool}.py", "project=hello_world", "board=esp32_ai_thinker_cam", "dry_run=true"])
                self.assertFalse(any("idf.py" in arg for call in calls for arg in call))
