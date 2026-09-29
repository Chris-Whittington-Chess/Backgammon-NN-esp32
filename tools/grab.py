"""Grab the CoreS3 framebuffer over serial and save it as a PNG.

Usage: python tools/grab.py out.png [cmd ...]
Each cmd (e.g. "t 200 50", "n", "r") is sent before the dump.
"""
import sys, time
import serial
from PIL import Image

out = sys.argv[1]
s = serial.Serial()
s.port, s.baudrate, s.timeout = "COM3", 115200, 3
s.dtr = s.rts = False  # asserted DTR/RTS reboots the CoreS3
s.open()
time.sleep(0.3)
s.reset_input_buffer()
for cmd in sys.argv[2:]:
    s.write((cmd + "\n").encode())
    time.sleep(0.3)
    print(s.read(s.in_waiting).decode(errors="replace").strip())
s.write(b"d")
buf = b""
while b"FRAME\n" not in buf:
    chunk = s.read(64)
    if not chunk:
        sys.exit("no FRAME header")
    buf += chunk
data = buf.split(b"FRAME\n", 1)[1]
need = 320 * 240 * 2
while len(data) < need:
    chunk = s.read(need - len(data))
    if not chunk:
        sys.exit(f"short frame: {len(data)}/{need}")
    data += chunk
s.close()
img = Image.new("RGB", (320, 240))
px = []
for i in range(0, need, 2):
    v = data[i] | data[i + 1] << 8
    # M5GFX sprites store RGB565 byte-swapped; readPixel returns native order.
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    px.append((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
img.putdata(px)
img.resize((640, 480), Image.NEAREST).save(out)
print("saved", out)
