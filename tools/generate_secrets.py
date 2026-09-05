#!/usr/bin/env python3
"""Generate the local Arduino secrets.h header from the project .env file."""

from __future__ import annotations

import os
import re
import sys
import tempfile
import ipaddress
from pathlib import Path


REQUIRED_VARIABLES = (
    "WIFI_SSID",
    "WIFI_PASSWORD",
    "TELEGRAM_BOT_TOKEN",
    "TELEGRAM_CHAT_ID",
    "OTA_HOSTNAME",
    "OTA_PASSWORD",
    "AUTO_WAKE_WINDOWS",
    "AUTO_WAKE_LINUX",
    "AUTO_WAKE_DELAY_SECONDS",
)
TARGETS = (
    ("WINDOWS", "Windows"),
    ("LINUX", "Linux"),
)
HEADER_VARIABLES = (
    "WIFI_SSID",
    "WIFI_PASSWORD",
    "TELEGRAM_BOT_TOKEN",
    "TELEGRAM_CHAT_ID",
    "WINDOWS_MAC",
    "WINDOWS_IP",
    "LINUX_MAC",
    "LINUX_IP",
    "OTA_HOSTNAME",
    "OTA_PASSWORD",
    "AUTO_WAKE_WINDOWS",
    "AUTO_WAKE_LINUX",
    "AUTO_WAKE_DELAY_SECONDS",
)
BOOLEAN_VARIABLES = ("AUTO_WAKE_WINDOWS", "AUTO_WAKE_LINUX")
MAC_PATTERN = re.compile(r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")


def read_env(path: Path) -> dict[str, str]:
    """Read simple KEY=VALUE entries without importing development-machine env."""
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise ValueError("Could not read .env file.") from exc

    for line_number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        if "=" not in line:
            raise ValueError(f"Invalid .env entry on line {line_number}.")

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            raise ValueError(f"Invalid .env entry on line {line_number}.")
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        values[key] = value

    return values


def validate(values: dict[str, str]) -> None:
    for name in REQUIRED_VARIABLES:
        if not values.get(name):
            raise ValueError(f"Missing required environment variable: {name}")

    configured_targets: dict[str, bool] = {}
    for prefix, label in TARGETS:
        mac_name = f"{prefix}_MAC"
        ip_name = f"{prefix}_IP"
        mac_address = values.get(mac_name, "")
        ip_address = values.get(ip_name, "")
        values[mac_name] = mac_address
        values[ip_name] = ip_address

        if not mac_address and not ip_address:
            configured_targets[prefix] = False
            continue
        if mac_address and not ip_address:
            raise ValueError(f"{ip_name} is required when {mac_name} is configured.")
        if ip_address and not mac_address:
            raise ValueError(f"{mac_name} is required when {ip_name} is configured.")
        if not MAC_PATTERN.fullmatch(mac_address):
            raise ValueError(f"Invalid {mac_name}: expected format AA:BB:CC:DD:EE:FF")
        try:
            values[ip_name] = str(ipaddress.IPv4Address(ip_address))
        except ipaddress.AddressValueError as exc:
            raise ValueError(f"Invalid {ip_name}: expected an IPv4 address") from exc
        values[mac_name] = mac_address.upper()
        configured_targets[prefix] = True

    if not any(configured_targets.values()):
        raise ValueError("At least one target machine must be configured.")

    for name in BOOLEAN_VARIABLES:
        value = values[name].lower()
        if value not in ("true", "false"):
            raise ValueError(f"Invalid {name}: expected true or false")
        values[name] = value

    for prefix, label in TARGETS:
        auto_wake_name = f"AUTO_WAKE_{prefix}"
        if values[auto_wake_name] == "true" and not configured_targets[prefix]:
            raise ValueError(f"{auto_wake_name} cannot be true because {label} target is not configured.")

    try:
        delay_seconds = int(values["AUTO_WAKE_DELAY_SECONDS"], 10)
    except ValueError as exc:
        raise ValueError("Invalid AUTO_WAKE_DELAY_SECONDS: expected an integer from 0 to 300") from exc
    if not 0 <= delay_seconds <= 300:
        raise ValueError("Invalid AUTO_WAKE_DELAY_SECONDS: expected an integer from 0 to 300")
    values["AUTO_WAKE_DELAY_SECONDS"] = str(delay_seconds)


def c_string(value: str) -> str:
    """Return a C/C++ string literal with backslashes and quotes escaped."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def build_header(values: dict[str, str]) -> str:
    lines = [
        "#pragma once",
        "",
        "// Auto-generated from .env.",
        "// Do not edit manually.",
        "// Do not commit this file.",
        "",
    ]
    for name in HEADER_VARIABLES:
        if name in BOOLEAN_VARIABLES:
            lines.append(f"#define {name} {values[name]}")
        elif name == "AUTO_WAKE_DELAY_SECONDS":
            lines.append(f"#define {name} {values[name]}U")
        else:
            lines.append(f"#define {name} {c_string(values[name])}")
        if name in ("WIFI_PASSWORD", "TELEGRAM_CHAT_ID", "WINDOWS_IP", "LINUX_IP", "OTA_PASSWORD"):
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def write_header_safely(destination: Path, content: str) -> None:
    fd, temporary_name = tempfile.mkstemp(
        prefix=".secrets.", suffix=".tmp", dir=str(destination.parent), text=True
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as temporary_file:
            temporary_file.write(content)
            temporary_file.flush()
            os.fsync(temporary_file.fileno())
        os.replace(temporary_name, destination)
    except OSError:
        try:
            os.unlink(temporary_name)
        except OSError:
            pass
        raise


def main() -> int:
    project_root = Path(__file__).resolve().parent.parent
    try:
        values = read_env(project_root / ".env")
        validate(values)
        write_header_safely(project_root / "include" / "secrets.h", build_header(values))
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except OSError:
        print("Could not write include/secrets.h.", file=sys.stderr)
        return 1

    print("Generated secrets.h successfully.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
