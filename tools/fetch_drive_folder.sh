#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo 'Usage: tools/fetch_drive_folder.sh [GOOGLE_DRIVE_FOLDER_LINK_OR_ID [DESTINATION]]'
}

if [[ $# == 1 && ( $1 == --help || $1 == -h ) ]]; then
  usage
  exit 0
fi
if (( $# > 2 )); then
  usage >&2
  exit 2
fi

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if (( $# == 0 )); then
  config="${root}/.secrets/drive_folder_id"
  if [[ ! -s $config ]]; then
    echo 'Set the default folder ID in .secrets/drive_folder_id or pass a folder link.' >&2
    exit 2
  fi
  folder=$(<"$config")
  destination="${root}/drive-downloads"
else
  folder=$1
  destination="${root}/build/google-drive/"
fi

if [[ ! $folder =~ ^https://drive\.google\.com/drive/([Uu]/[0-9]+/)?folders/[A-Za-z0-9_-]+/?([?#].*)?$ && ! $folder =~ ^[A-Za-z0-9_-]+$ ]]; then
  echo 'Use the folder’s Share link (drive.google.com/drive/folders/…) or its folder ID.' >&2
  exit 2
fi

if (( $# == 2 )); then
  if [[ -z $2 ]]; then
    echo 'Destination cannot be empty.' >&2
    exit 2
  fi
  destination=$(realpath -m -- "$2")
fi

# A trailing slash asks gdown to retain the Drive folder name under this path.
exec uvx --from 'gdown==6.5.2' gdown --folder --no-cookies --continue \
  --timeout 45 --retries 3 --output "$destination" "$folder"
