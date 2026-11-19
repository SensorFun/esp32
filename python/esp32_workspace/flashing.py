"""Flash existing ESP-IDF artifacts with host esptool; no build tools required."""
import json
from pathlib import Path
import shlex
import subprocess
import sys

from omegaconf import DictConfig, OmegaConf
from serial.tools import list_ports

from .boards import load_board
from .cli import command

# Espressif native USB and common USB-to-UART bridge vendors.
USB_VENDORS = {0x303A, 0x10C4, 0x1A86, 0x0403, 0x067B}

# ESP-IDF's standard single-app partition table places nvs here; projects
# without a custom partitions.csv use that default table.
DEFAULT_NVS_PARTITION = (0x9000, 0x6000)


def _parse_partition_size(value: str) -> int:
    value = value.strip()
    if value.lower().startswith("0x"):
        return int(value, 16)
    if value[-1:].upper() == "K":
        return int(value[:-1]) * 1024
    if value[-1:].upper() == "M":
        return int(value[:-1]) * 1024 * 1024
    return int(value)


def nvs_partition(root: Path, project: str) -> tuple[int, int]:
    path = root / "projects" / project / "partitions.csv"
    if not path.is_file():
        return DEFAULT_NVS_PARTITION
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[2] == "nvs":
            return _parse_partition_size(fields[3]), _parse_partition_size(fields[4])
    raise ValueError(f"No nvs partition found in {path}")


def detect_port(root: Path, board: str) -> str:
    ports = sorted(list_ports.comports(), key=lambda port: port.device)
    candidates = [port for port in ports if port.vid in USB_VENDORS]
    matches = []
    for path in sorted((root / "config/device").glob("*.yaml")):
        profile = OmegaConf.load(path)
        identity = profile.get("programming_port")
        if profile.get("board") != board or identity is None:
            continue
        vid, pid = (int(value, 16) for value in identity.usb_id.split(":"))
        product = identity.get("product")
        serial = identity.get("serial")
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
    if not isinstance(config.clear_nvs, bool):
        raise ValueError("clear_nvs must be true or false")
    # Reuse the firmware selection validation without running idf.py.
    command(root, config.project, config.board, "build", None, config.frame_grabber.name if config.get("frame_grabber") is not None else None)
    build = root / "build" / config.project / config.board
    metadata_file = build / "flasher_args.json"
    args_file = build / "flash_args"
    if not metadata_file.is_file() or not args_file.is_file():
        raise ValueError("Firmware is not built. Run python3 tools/build.py with the same project and board first.")
    if config.get("frame_grabber") is not None:
        selection_file = build / "firmware_selection.json"
        if not selection_file.is_file():
            raise ValueError("Frame grabber selection metadata is missing; rebuild this device profile first.")
        selection = json.loads(selection_file.read_text())
        if selection.get("board") != config.board or selection.get("frame_grabber") != config.frame_grabber.name:
            raise ValueError("Built board/frame grabber does not match this device profile; rebuild before flashing.")
    metadata = json.loads(metadata_file.read_text())
    board = load_board(root, config.board)
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
    base = [sys.executable, "-m", "esptool", "--chip", extra["chip"], "--port", port, "--baud", str(config.baud)]
    if not extra["stub"]:
        base.append("--no-stub")
    cmd = [*base, "--before", extra["before"], "--after", extra["after"], "write-flash", "@flash_args"]
    print(f"Working directory: {build}", flush=True)
    if config.clear_nvs:
        offset, size = nvs_partition(root, config.project)
        erase_cmd = [*base, "--before", extra["before"], "--after", "no-reset", "erase-region", hex(offset), hex(size)]
        print(shlex.join(erase_cmd), flush=True)
        if not config.dry_run:
            result = subprocess.call(erase_cmd, cwd=build)
            if result:
                return result
    print(shlex.join(cmd), flush=True)
    if config.dry_run:
        return 0
    return subprocess.call(cmd, cwd=build)
