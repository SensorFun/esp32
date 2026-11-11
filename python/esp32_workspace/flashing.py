"""Flash existing ESP-IDF artifacts with host esptool; no build tools required."""
import csv
import json
from pathlib import Path
import shlex
import subprocess
import sys

from omegaconf import DictConfig, OmegaConf
from serial.tools import list_ports

from .cli import command

# Espressif native USB and common USB-to-UART bridge vendors.
USB_VENDORS = {0x303A, 0x10C4, 0x1A86, 0x0403, 0x067B}


def detect_port(root: Path, board: str) -> str:
    ports = sorted(list_ports.comports(), key=lambda port: port.device)
    candidates = [port for port in ports if port.vid in USB_VENDORS]
    identities = root / "boards" / "programming_ports.tsv"
    matches = []
    if identities.is_file():
        rows = csv.reader((line for line in identities.read_text().splitlines() if line and not line.startswith("#")), delimiter="\t")
        for row in rows:
            if row[0] != board:
                continue
            _, usb_id, product, serial = row
            vid, pid = (int(value, 16) for value in usb_id.split(":"))
            matches.extend(port for port in ports if
                port.vid == vid and port.pid == pid
                and (not product or port.product == product)
                and (not serial or port.serial_number == serial))
    matches = {port.device for port in matches}
    if len(matches) == 1:
        return matches.pop()
    if len(candidates) == 1 and not matches:
        return candidates[0].device
    if not candidates and not matches:
        raise ValueError("No supported ESP32 programming port found. Connect the board or set port=/dev/ttyACM0 explicitly.")
    devices = sorted(matches | {port.device for port in candidates})
    raise ValueError("Multiple programming ports found: " + ", ".join(devices) + ". Select one with port=... .")


def run_flash(root: Path, config: DictConfig) -> int:
    root = root.resolve()
    OmegaConf.to_container(config, resolve=True, throw_on_missing=True)
    if not isinstance(config.dry_run, bool):
        raise ValueError("dry_run must be true or false")
    if type(config.baud) is not int or config.baud <= 0:
        raise ValueError("baud must be a positive integer")
    # Reuse the firmware selection validation without running idf.py.
    command(root, config.project, config.board, "build", None)
    build = root / "build" / config.project / config.board
    metadata_file = build / "flasher_args.json"
    args_file = build / "flash_args"
    if not metadata_file.is_file() or not args_file.is_file():
        raise ValueError("Firmware is not built. Run python3 tools/build.py with the same project and board first.")
    metadata = json.loads(metadata_file.read_text())
    board = json.loads((root / "boards" / f"{config.board}.json").read_text())
    extra = metadata["extra_esptool_args"]
    if extra["chip"] != board["target"]:
        raise ValueError("Build chip does not match the selected board; rebuild before flashing.")
    for filename in metadata["flash_files"].values():
        artifact = (build / filename).resolve()
        if not artifact.is_relative_to(build.resolve()) or not artifact.is_file():
            raise ValueError(f"Missing or invalid firmware artifact: {filename}. Rebuild first.")
    port = config.port
    if port is None:
        port = detect_port(root, config.board)
        print(f"Detected programming port: {port}", flush=True)
    elif not isinstance(port, str) or not port.strip():
        raise ValueError("port must be null for automatic detection or a nonempty device name")
    cmd = [sys.executable, "-m", "esptool", "--chip", extra["chip"],
           "--port", port, "--baud", str(config.baud),
           "--before", extra["before"], "--after", extra["after"]]
    if not extra["stub"]:
        cmd.append("--no-stub")
    cmd += ["write-flash", "@flash_args"]
    print(f"Working directory: {build}", flush=True)
    print(shlex.join(cmd), flush=True)
    if config.dry_run:
        return 0
    return subprocess.call(cmd, cwd=build)
