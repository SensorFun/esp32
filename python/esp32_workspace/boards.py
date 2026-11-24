"""Load board catalogs for host tools and CMake."""
import json
from pathlib import Path
import re
import sys

from omegaconf import OmegaConf

# SPI flash clocks ESP-IDF offers per chip (CONFIG_ESPTOOLPY_FLASHFREQ_*). Each
# board declares the fastest clock its flash chip and wiring run reliably.
FLASH_FREQS = {'esp32': ('80m', '40m', '26m', '20m'), 'esp32s3': ('120m', '80m', '40m', '20m')}
# PSRAM clocks per chip (CONFIG_SPIRAM_SPEED_*). On the ESP32, 80 MHz PSRAM
# requires an 80 MHz flash clock.
PSRAM_FREQS = {'esp32': ('80m', '40m'), 'esp32s3': ('120m', '80m', '40m')}
# Wi-Fi TX buffer types and their count limits (CONFIG_ESP_WIFI_*_TX_BUFFER_NUM).
WIFI_TX_BUFFER_LIMITS = {'static': 64, 'dynamic': 128}
# Generated in each build directory from the board entry; applied after every
# other sdkconfig defaults file so the board's value wins.
BOARD_DEFAULTS = 'board.defaults'


def load_catalog(root: Path) -> dict:
    catalog = {}
    for path in sorted((root / 'config/boards').glob('*.yaml')):
        entries = OmegaConf.to_container(OmegaConf.load(path), resolve=True, throw_on_missing=True)
        if not isinstance(entries, dict):
            raise ValueError(f'Invalid board catalog: {path.name}')
        for board, metadata in entries.items():
            if not isinstance(board, str) or not re.fullmatch(r'[a-zA-Z0-9_-]+', board):
                raise ValueError(f'Invalid board name: {board}')
            if board in catalog:
                raise ValueError(f'Duplicate board: {board}')
            if not isinstance(metadata, dict) or not re.fullmatch(r'esp32[a-z0-9]*', str(metadata.get('target', ''))):
                raise ValueError(f'Invalid chip target for {board}')
            freqs = FLASH_FREQS.get(metadata['target'])
            if freqs is None:
                raise ValueError(f'No known flash clocks for chip {metadata["target"]} ({board}); add them to FLASH_FREQS')
            if metadata.get('flash_freq') not in freqs:
                raise ValueError(f'Invalid flash_freq for {board}: {metadata.get("flash_freq")!r}; {metadata["target"]} supports {", ".join(freqs)}')
            validate_tuning(board, metadata)
            if not isinstance(metadata.get('name'), str) or not metadata['name'].strip():
                raise ValueError(f'Missing display name for {board}')
            header = metadata.get('header')
            if not isinstance(header, str) or not header or Path(header).is_absolute():
                raise ValueError(f'Invalid header for {board}')
            header_path = (root / 'boards' / header).resolve()
            if not header_path.is_relative_to((root / 'boards').resolve()) or not header_path.is_file():
                raise ValueError(f'Missing or invalid header for {board}: {header}')
            image = metadata.get('image')
            if image is not None:
                images = (root / 'config/boards/images').resolve()
                image_path = (root / 'config/boards' / str(image)).resolve()
                if not isinstance(image, str) or Path(image).is_absolute() or not image_path.is_relative_to(images) or not image_path.is_file():
                    raise ValueError(f'Missing or invalid image for {board}: {image}')
            grabbers = metadata.get('supported_frame_grabber', [])
            if not isinstance(grabbers, list) or any(
                not isinstance(name, str) or not re.fullmatch(r'[a-zA-Z0-9_]+', name)
                or not (root / 'config/frame_grabber' / f'{name}.yaml').is_file()
                for name in grabbers
            ):
                raise ValueError(f'Invalid frame grabber profiles for {board}')
            defaults = metadata.get('sdkconfig_defaults', [])
            if not isinstance(defaults, list) or any(not isinstance(item, str) or Path(item).is_absolute() or not (root / 'boards' / item).resolve().is_relative_to((root / 'boards').resolve()) or not (root / 'boards' / item).is_file() for item in defaults):
                raise ValueError(f'Invalid SDK defaults for {board}')
            catalog[board] = metadata
    return catalog


def validate_tuning(board: str, metadata: dict) -> None:
    """Check the board's performance settings: PSRAM clock, Wi-Fi TX buffers, lwIP IRAM."""
    target = metadata['target']
    psram = metadata.get('psram_freq')
    if psram is not None:
        if psram not in PSRAM_FREQS.get(target, ()):
            raise ValueError(f'Invalid psram_freq for {board}: {psram!r}; {target} supports {", ".join(PSRAM_FREQS.get(target, ()))}')
        if target == 'esp32' and psram == '80m' and metadata['flash_freq'] != '80m':
            raise ValueError(f'psram_freq 80m for {board} requires flash_freq 80m')
    buffers = metadata.get('wifi_tx_buffers')
    if (not isinstance(buffers, dict) or set(buffers) != {'type', 'count'}
            or buffers['type'] not in WIFI_TX_BUFFER_LIMITS or type(buffers['count']) is not int
            or not 1 <= buffers['count'] <= WIFI_TX_BUFFER_LIMITS[buffers['type']]):
        raise ValueError(f'Invalid wifi_tx_buffers for {board}: use {{type: static, count: 1-64}} or {{type: dynamic, count: 1-128}}')
    if not isinstance(metadata.get('lwip_iram'), bool):
        raise ValueError(f'lwip_iram for {board} must be true or false')


def board_settings(metadata: dict) -> list[tuple[str, list[str]]]:
    """sdkconfig lines for the board's YAML values, grouped with the option family each replaces."""
    buffers = metadata['wifi_tx_buffers']
    kind = buffers['type'].upper()
    groups = [(r'CONFIG_ESPTOOLPY_FLASHFREQ(_\w+)?', [f'CONFIG_ESPTOOLPY_FLASHFREQ_{metadata["flash_freq"].upper()}=y']),
              (r'CONFIG_ESP_WIFI_(STATIC|DYNAMIC)_TX_BUFFER(_NUM)?|CONFIG_ESP_WIFI_TX_BUFFER_TYPE',
               [f'CONFIG_ESP_WIFI_{kind}_TX_BUFFER=y', f'CONFIG_ESP_WIFI_{kind}_TX_BUFFER_NUM={buffers["count"]}']),
              (r'CONFIG_LWIP_IRAM_OPTIMIZATION',
               ['CONFIG_LWIP_IRAM_OPTIMIZATION=y' if metadata['lwip_iram'] else '# CONFIG_LWIP_IRAM_OPTIMIZATION is not set'])]
    if metadata.get('psram_freq') is not None:
        groups.append((r'CONFIG_SPIRAM_SPEED(_\w+)?', [f'CONFIG_SPIRAM_SPEED_{metadata["psram_freq"].upper()}=y']))
    return groups


def load_board(root: Path, board: str) -> dict:
    if not isinstance(board, str) or not re.fullmatch(r'[a-zA-Z0-9_-]+', board):
        raise ValueError(f'Invalid board name: {board}')
    catalog = load_catalog(root)
    if board not in catalog:
        raise ValueError(f'Unknown board: {board}')
    return catalog[board]


def write_board_sdkconfig(root: Path, board: str, build: Path) -> Path:
    """Write the board's sdkconfig values (flash and PSRAM clocks, Wi-Fi TX
    buffers, lwIP IRAM) into a build directory.

    ESP-IDF applies defaults only to options an existing sdkconfig lacks. When an
    existing sdkconfig disagrees with the board, that option family is removed
    and re-read from the board defaults; every other setting is kept.
    """
    groups = board_settings(load_board(root, board))
    build.mkdir(parents=True, exist_ok=True)
    defaults = build / BOARD_DEFAULTS
    content = f'# Generated from config/boards ({board}); do not edit.\n' + ''.join(line + '\n' for _, lines in groups for line in lines)
    if not defaults.is_file() or defaults.read_text() != content:
        defaults.write_text(content)
    sdkconfig = build / 'sdkconfig'
    if sdkconfig.is_file():
        text = sdkconfig.read_text()
        current = set(text.splitlines())
        updated = text
        for pattern, lines in groups:
            if not current.issuperset(lines):
                updated = re.sub(rf'^(# )?({pattern})[= ].*\n', '', updated, flags=re.MULTILINE)
        if updated != text:
            sdkconfig.write_text(updated)
    return defaults


def write_board_header(root: Path, board: str, output: Path) -> None:
    metadata = load_board(root, board)
    # JSON string quoting safely preserves quotes and backslashes in C++ names.
    name = json.dumps(metadata['name'], ensure_ascii=False)
    header = (root / 'boards' / metadata['header']).resolve().as_posix()
    content = f'#pragma once\ninline constexpr char board_display_name[] = {name};\n#include {json.dumps(header)}\n'
    if not output.exists() or output.read_text() != content:
        output.write_text(content)


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        raise SystemExit('Usage: python -m esp32_workspace.boards WORKSPACE BOARD [OUTPUT_HEADER]')
    root, board = Path(sys.argv[1]), sys.argv[2]
    metadata = load_board(root, board)
    if len(sys.argv) == 4:
        write_board_header(root, board, Path(sys.argv[3]))
    print(json.dumps(metadata))
