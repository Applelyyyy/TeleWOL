"""Generate private firmware settings before every PlatformIO build."""

import sys
from pathlib import Path

Import("env")

tools_dir = Path(env.subst("$PROJECT_DIR")) / "tools"
sys.path.insert(0, str(tools_dir))

from generate_secrets import main as generate_secrets, read_env


if generate_secrets() != 0:
    env.Exit(1)

if env.subst("$UPLOAD_PROTOCOL") == "espota":
    values = read_env(Path(env.subst("$PROJECT_DIR")) / ".env")
    upload_host = values["ESP_STATIC_IP"] or f'{values["OTA_HOSTNAME"]}.local'
    env.Replace(
        UPLOAD_PORT=upload_host,
        UPLOAD_FLAGS=[
            f'--auth={values["OTA_PASSWORD"]}',
            f'--port={values["OTA_PORT"]}',
        ],
    )
    print(f'OTA upload target: {upload_host}:{values["OTA_PORT"]}')
