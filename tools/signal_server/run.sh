#!/usr/bin/env bash
set +x
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
action="${1-up}"
case "$action" in
  up|down|logs) ;;
  *) echo 'Usage: run.sh [up|down|logs]' >&2; exit 2 ;;
esac
export JANUS_SECRET_FILE="$SCRIPT_DIR/.secrets/janus_api_secret"
if [[ ! -s "$JANUS_SECRET_FILE" ]]; then
  echo 'Missing .secrets/janus_api_secret' >&2
  exit 1
fi
shopt -s nullglob
credentials=("$SCRIPT_DIR"/.secrets/*.json)
if [[ ${#credentials[@]} != 1 ]]; then
  echo 'Place exactly one tunnel credentials JSON in .secrets/' >&2
  exit 1
fi
export CLOUDFLARE_CREDENTIALS_FILE="${credentials[0]}"
export CLOUDFLARE_TUNNEL_ID
CLOUDFLARE_TUNNEL_ID="$(python3 - "$CLOUDFLARE_CREDENTIALS_FILE" <<'PY'
import json, sys
from uuid import UUID
try:
    with open(sys.argv[1]) as stream:
        print(UUID(json.load(stream)['TunnelID']))
except (OSError, ValueError, KeyError, TypeError):
    sys.exit('Invalid tunnel credentials JSON')
PY
)"
export SIGNAL_SERVER_UID="$(id -u)"
export SIGNAL_SERVER_GID="$(id -g)"
export JANUS_UPSTREAM_URL='ws://janus:8188'
compose=(docker compose -f "$SCRIPT_DIR/compose.yaml")
case "$action" in
  up) "${compose[@]}" stop; exec "${compose[@]}" up --build -d ;;
  down) exec "${compose[@]}" down ;;
  logs) exec "${compose[@]}" logs --tail 100 -f ;;
esac
