#!/usr/bin/env python3
"""Generate private build input from environment; never print credentials."""
import argparse
import os
from pathlib import Path
import string


def render(ssid: str, password: str, enabled: bool = True) -> str:
    if not enabled:
        ssid, password = "", ""
    else:
        ssid_bytes = ssid.encode("utf-8")
        if not 1 <= len(ssid_bytes) <= 32 or any(c in ssid for c in "\0\r\n"):
            raise ValueError("KOYODA_WIFI_SSID must be 1-32 UTF-8 bytes without line breaks.")
        size = len(password.encode("utf-8"))
        hex_psk = size == 64 and all(c in string.hexdigits for c in password)
        ascii_pass = 8 <= size <= 63 and all(32 <= ord(c) <= 126 for c in password)
        if not (hex_psk or ascii_pass):
            raise ValueError("KOYODA_WIFI_PASSWORD must be 8-63 printable ASCII characters or 64 hex digits.")
    # Fixed-width octal escapes preserve spaces, quotes, backslashes, $, and
    # UTF-8 bytes without accidental C escapes or shell interpolation.
    def literal(value: str) -> str:
        return '"' + ''.join(f'\\{b:03o}' for b in value.encode('utf-8')) + '"'
    return ("/* Private generated build input. Do not commit. */\n#pragma once\n"
            f"#define KOYODA_WIFI_SSID {literal(ssid)}\n"
            f"#define KOYODA_WIFI_PASSWORD {literal(password)}\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("main/koyoda_wifi_secrets.h"))
    parser.add_argument("--disabled", action="store_true")
    args = parser.parse_args()
    # Invalidate an older header first, so failed validation cannot build
    # with yesterday's network by mistake.
    args.output.unlink(missing_ok=True)
    try:
        contents = render(os.environ.get("KOYODA_WIFI_SSID", ""),
                          os.environ.get("KOYODA_WIFI_PASSWORD", ""), not args.disabled)
    except ValueError as error:
        raise SystemExit(str(error)) from None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii", newline="\n") as handle:
        os.chmod(args.output, 0o600)
        handle.write(contents)
    print("Wi-Fi build input ready: " + ("OFF" if args.disabled else "ON"))


if __name__ == "__main__":
    main()
