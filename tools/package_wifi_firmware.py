#!/usr/bin/env python3
"""Package only files needed for flashing; never copy credentials/source/ELF."""
import hashlib
import json
import os
from pathlib import Path
import shutil


def main() -> None:
    build, out = Path("build-ci"), Path("firmware")
    out.mkdir(exist_ok=False)
    info = json.loads((build / "flasher_args.json").read_text())
    for relative in info["flash_files"].values():
        source = (build / relative).resolve()
        if not source.is_relative_to(build.resolve()) or source.suffix != ".bin":
            raise SystemExit("Unexpected flash binary path.")
        dest = out / source.relative_to(build.resolve())
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
    for name in ("flash_args", "flasher_args.json"):
        shutil.copyfile(build / name, out / name)
    for name in ("partitions.csv", "sdkconfig", "dependencies.lock", "wifi-preserved.sha256"):
        shutil.copyfile(name, out / Path(name).name)
    enabled = os.environ.get("KOYODA_ENABLE_WIFI", "true") == "true"
    (out / "BUILD-INFO.txt").write_text(
        f"KOYODA Wi-Fi: {'ON' if enabled else 'OFF (comparison build)'}\n"
        f"Source commit: {os.environ['SOURCE_SHA']}\n"
        "ESP-IDF: v5.5.4\nTarget: esp32s3\nFactory: 12M at 0x10000\n"
        "Original animation and frame assets verified by wifi-preserved.sha256.\n"
        "An ON firmware contains your Wi-Fi credentials. Keep it private.\n", encoding="utf-8")
    (out / "FLASH-WINDOWS.txt").write_text(
        'Extract all files to C:\\Users\\peemm\\Downloads\\Koyoda (back up the old firmware first).\n'
        'Open the ESP-IDF terminal and close any serial monitor.\n\n'
        'cd "C:\\Users\\peemm\\Downloads\\Koyoda"\n'
        'python -m esptool --chip esp32s3 --port COM11 --baud 460800 '
        '--before default_reset --after hard_reset write_flash "@flash_args"\n\n'
        'Use your actual COM port if it has changed. No erase_flash needed.\n'
        'Swipe to Battery. Wi-Fi: <IP address> means Wi-Fi + DHCP connected.\n'
        'It does not confirm Internet or an AI server connection.\n', encoding="utf-8")
    sums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(out).as_posix()}"
            for p in sorted(out.rglob("*")) if p.is_file()]
    (out / "SHA256SUMS.txt").write_text("\n".join(sums) + "\n")
    print("Firmware package ready.")


if __name__ == "__main__":
    main()
