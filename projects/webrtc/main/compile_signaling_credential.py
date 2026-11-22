"""Compile the CI credential from standard input into an ESP32 object file."""
import os
import subprocess
import sys


def main() -> None:
    compiler, output = sys.argv[1:]
    credential = os.environ["CAMERA_SIGNALING_API_SECRET"].encode("ascii")
    values = ", ".join(str(byte) for byte in credential + b"\0")
    source = f"const char ci_camera_signaling_secret[] = {{{values}}};\n"
    subprocess.run([compiler, "-x", "c", "-c", "-o", output, "-"], input=source, text=True, check=True)


if __name__ == "__main__":
    main()
