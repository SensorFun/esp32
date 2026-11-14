#!/usr/bin/env python3
"""Launch the local signaling stack with explicitly validated environment."""
import argparse
import base64
import binascii
import json
import os
from pathlib import Path
import re
import subprocess
from uuid import UUID


def credentials_environment(path):
    """Validate credentials without disclosing their values in errors or logs."""
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError("Credentials must be a JSON file")
    try:
        credentials = json.loads(path.read_text())
        if not isinstance(credentials, dict) or not isinstance(credentials.get("TunnelID"), str):
            raise ValueError
        tunnel_id = str(UUID(credentials["TunnelID"]))
        account = credentials["AccountTag"]
        secret = credentials["TunnelSecret"]
        if not isinstance(account, str) or not account:
            raise ValueError
        if not isinstance(secret, str) or len(base64.b64decode(secret, validate=True)) != 32:
            raise ValueError
    except (ValueError, TypeError, KeyError, binascii.Error):
        raise ValueError("Invalid Cloudflare tunnel credentials JSON") from None
    return {
        "CLOUDFLARE_CREDENTIALS_FILE": str(path),
        "CLOUDFLARE_TUNNEL_ID": tunnel_id,
        "SIGNAL_SERVER_UID": str(os.getuid()),
        "SIGNAL_SERVER_GID": str(os.getgid()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["up", "down", "logs"], nargs="?", default="up")
    parser.add_argument("--credentials", type=Path, help="Tunnel credentials JSON; defaults to the only JSON file in .secrets/")
    parser.add_argument("--api-secret-file", type=Path, help="Janus API secret file; otherwise use JANUS_API_SECRET or .secrets/janus_api_secret")
    args = parser.parse_args()
    api_secret = os.environ.get("JANUS_API_SECRET", "")
    secret_file = args.api_secret_file
    if secret_file is None and not api_secret:
        secret_file = Path(__file__).with_name(".secrets") / "janus_api_secret"
    if secret_file is not None:
        try:
            api_secret = secret_file.read_text().strip()
        except OSError:
            parser.error("Cannot read the Janus API secret file")
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", api_secret):
        parser.error("JANUS_API_SECRET must contain 32–128 URL-safe characters")
    path = args.credentials
    if path is None:
        candidates = list(Path(__file__).with_name(".secrets").glob("*.json"))
        if len(candidates) != 1:
            parser.error("Place one credentials JSON in .secrets/ or select it with --credentials")
        path = candidates[0]
    try:
        environment = dict(os.environ, **credentials_environment(path))
        environment["JANUS_API_SECRET"] = api_secret
        environment["JANUS_UPSTREAM_URL"] = "ws://janus:8188"
    except (OSError, ValueError):
        parser.error("Cannot read valid Cloudflare tunnel credentials from the selected file")
    command = ["docker", "compose", "-f", str(Path(__file__).with_name("compose.yaml"))]
    command += {"up": ["up", "--build", "-d"], "down": ["down"], "logs": ["logs", "--tail", "100", "-f"]}[args.action]
    raise SystemExit(subprocess.call(command, env=environment))


if __name__ == "__main__":
    main()
