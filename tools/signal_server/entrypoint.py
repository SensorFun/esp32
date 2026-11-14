"""Validate runtime credentials and write private Janus configuration to tmpfs."""
import json
import os
from pathlib import Path
import re
import sys


def main():
    secret = os.environ.get("JANUS_API_SECRET", "")
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", secret):
        sys.exit("JANUS_API_SECRET must contain 32–128 URL-safe characters")
    config = Path("/tmp/janus-config")
    config.mkdir(mode=0o700, exist_ok=True)
    core = config / "janus.jcfg"
    core.write_text('general: {\n'
                    ' configs_folder = "/tmp/janus-config"\n'
                    ' plugins_folder = "/opt/janus/lib/janus/plugins"\n'
                    ' transports_folder = "/opt/janus/lib/janus/transports"\n'
                    ' debug_level = 3\n'
                    ' session_timeout = 60\n'
                    f' api_secret = {json.dumps(secret)}\n'
                    '}\n')
    core.chmod(0o600)
    (config / "janus.transport.websockets.jcfg").write_text(
        'general: { ws = true ws_port = 8188 wss = false json = "compact" }\n'
        'admin: { admin_ws = false admin_wss = false }\n')
    # The API secret is never placed in process arguments or logs.
    os.environ.pop("JANUS_API_SECRET")
    os.execv("/opt/janus/bin/janus", ["janus", "-F", str(config)])


if __name__ == "__main__":
    main()
