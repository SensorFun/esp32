#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'HELP'
Usage: ./docker/docker.sh [--build] [--] COMMAND [ARG...]
       ./docker/docker.sh --build

Run a command in a temporary development container, removed when it exits.
  --build  Build the development image before running, or build and exit.
  --help   Show this help.
  --       End launcher options; remaining arguments are the command.

Examples:
  ./docker/docker.sh --build
  ./docker/docker.sh esp32 build
  ./docker/docker.sh python --version
HELP
}

build=false
while (( $# )); do
    case "$1" in
        --build) build=true; shift ;;
        --help|-h) usage; exit 0 ;;
        --) shift; break ;;
        -*) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
        *) break ;;
    esac
done
if (( $# == 0 )) && ! "${build}"; then
    usage >&2
    exit 2
fi

export WORKSPACE_SOURCE="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
export HOST_USER="$(id -un)"
export HOST_UID="$(id -u)"
export HOST_GID="$(id -g)"
compose=(docker compose
    -f "${SCRIPT_DIR}/docker-compose.yml"
    --profile development)

if "${build}"; then
    "${compose[@]}" build esp32-dev
fi
if (( $# == 0 )); then
    exit 0
fi

run_options=(--rm --no-deps)
if [[ ! -t 0 || ! -t 1 ]]; then
    run_options+=(-T)
fi
# The upstream entrypoint activates ESP-IDF before executing the command.
exec "${compose[@]}" run "${run_options[@]}" esp32-dev "$@"
