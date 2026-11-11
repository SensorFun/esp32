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
        self.env.pop("ESP32_PORT", None)
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
