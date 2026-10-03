"""Make the ready-to-flash release images, one file per board, written at address 0.

Usage (after building the boards):
    python tools/merge_firmware.py 1.0.3 [e32r40t] [ws7]
esptool is run from PlatformIO's own Python environment (~/.platformio/penv), which has it.
(no board names = both)

Writes dist/retro-backgammon-<version>-<board>.bin, flashed whole with e.g.
    esptool.py --chip esp32 write_flash 0x0 dist/retro-backgammon-<version>-e32r40t.bin
    esptool.py --chip esp32s3 write_flash 0x0 dist/retro-backgammon-<version>-ws7.bin
They are also what the website's browser installer (ESP Web Tools) flashes.

- e32r40t: bootloader, partition table, boot_app0 and the app merged at the offsets
  PlatformIO uses (`pio run -e e32r40t -t upload -v`).
- ws7: the pioarduino platform already writes that merged image (firmware.factory.bin).
"""
import shutil, subprocess, sys
from pathlib import Path

if len(sys.argv) < 2:
    sys.exit(__doc__)
version = sys.argv[1]
boards = sys.argv[2:] or ["e32r40t", "ws7"]

ROOT = Path(__file__).resolve().parents[1]
PIO = Path.home() / ".platformio" / "packages"
PENV_PY = Path.home() / ".platformio" / "penv" / "Scripts" / "python.exe"  # has esptool (5.x)
DIST = ROOT / "dist"
DIST.mkdir(exist_ok=True)


def need(f, env):
    if not f.exists():
        sys.exit(f"missing {f} - build first with: pio run -e {env}")


for board in boards:
    build = ROOT / ".pio" / "build" / board
    out = DIST / f"retro-backgammon-{version}-{board}.bin"
    if board == "e32r40t":
        boot_app0 = next(PIO.glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin"), None)
        if boot_app0 is None:
            sys.exit("boot_app0.bin not found under the PlatformIO framework packages")
        parts = [("0x1000", build / "bootloader.bin"), ("0x8000", build / "partitions.bin"),
                 ("0xe000", boot_app0), ("0x10000", build / "firmware.bin")]
        for _, f in parts:
            need(f, board)
        cmd = [str(PENV_PY), "-m", "esptool", "--chip", "esp32", "merge-bin", "-o", str(out),
               "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", "4MB"]
        for off, f in parts:
            cmd += [off, str(f)]
        subprocess.run(cmd, check=True)
    elif board == "ws7":
        factory = build / "firmware.factory.bin"
        need(factory, board)
        shutil.copyfile(factory, out)
    else:
        sys.exit(f"unknown board {board}")
    print(f"{out}  ({out.stat().st_size:,} bytes)")
