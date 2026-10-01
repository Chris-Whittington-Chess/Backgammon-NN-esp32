"""Merge the e32r40t build into one ready-to-flash image, written at address 0.

Usage (after `pio run -e e32r40t`), with PlatformIO's Python, which has esptool:
    ..\\m5-llm\\.venv\\Scripts\\python.exe tools/merge_firmware.py 1.0.0

Writes dist/retro-backgammon-<version>-e32r40t.bin: bootloader, partition table, boot_app0
and the app at the offsets PlatformIO uses, so one file flashes the whole board:
    esptool.py --chip esp32 write_flash 0x0 dist/retro-backgammon-<version>-e32r40t.bin
It is also what the website's browser installer (ESP Web Tools) flashes.
"""
import subprocess, sys
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit(__doc__)
version = sys.argv[1]

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "e32r40t"
PIO = Path.home() / ".platformio" / "packages"
ESPTOOL = PIO / "tool-esptoolpy" / "esptool.py"
BOOT_APP0 = PIO / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"

parts = [  # offset, file - as in `pio run -e e32r40t -t upload -v`
    ("0x1000", BUILD / "bootloader.bin"),
    ("0x8000", BUILD / "partitions.bin"),
    ("0xe000", BOOT_APP0),
    ("0x10000", BUILD / "firmware.bin"),
]
for _, f in parts:
    if not f.exists():
        sys.exit(f"missing {f} - build first with: pio run -e e32r40t")

out = ROOT / "dist" / f"retro-backgammon-{version}-e32r40t.bin"
out.parent.mkdir(exist_ok=True)
cmd = [sys.executable, str(ESPTOOL), "--chip", "esp32", "merge_bin", "-o", str(out),
       "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"]
for off, f in parts:
    cmd += [off, str(f)]
subprocess.run(cmd, check=True)
print(f"{out}  ({out.stat().st_size:,} bytes)")
