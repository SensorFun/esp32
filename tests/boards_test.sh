#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"
exec uv run --no-project \
  --with 'pytest>=8,<10' --with pytest-xdist --with hydra-core \
  --with pyserial --with esptool --with playwright --with nodejs-wheel \
  python tests/boards_launcher.py "$@"
