"""Export the live Backgammon-NN value net for the ESP32 boards.

Weights and model code come from the Backgammon-NN submodule (external/).
Run with a Backgammon-NN venv that has torch + the bgcore bindings built:
  ..\\Backgammon-2026\\.venv\\Scripts\\python.exe tools/export_net.py
Then re-run tools/export_movetests.py so the on-device parity check
(serial 'v') compares against the same net.

data/net.bin  (little-endian float32)
  magic "BGN1", u32 n_in=198, h1=256, h2=128, n_heads=12
  W1T [198][256]  first layer, input-major (sparse input accumulation)
  b1  [256]
  W2T [256][128]  second layer, input-major (skips ReLU-zero inputs)
  b2  [128]
  WH  [72][128]   heads, output-major (only the routed head's 6 rows are used)
  bH  [72]
"""
import struct
from pathlib import Path

import numpy as np
import torch

REPO = Path(__file__).resolve().parents[1] / "external" / "Backgammon-NN"
OUT = Path(__file__).resolve().parents[1] / "data"
OUT.mkdir(exist_ok=True)

ck = torch.load(REPO / "models" / "td_latest.pt", map_location="cpu", weights_only=False)
assert ck.get("class_aware") and ck["act"] == "relu", "expects the class-aware ReLU net"
sd = {k: v.numpy().astype("<f4") for k, v in ck["model"].items()}
W1, b1 = sd["body.0.weight"], sd["body.0.bias"]    # [256,198]
W2, b2 = sd["body.2.weight"], sd["body.2.bias"]    # [128,256]
WH, bH = sd["heads.weight"], sd["heads.bias"]      # [72,128]

with open(OUT / "net.bin", "wb") as f:
    f.write(b"BGN1" + struct.pack("<4I", 198, W1.shape[0], W2.shape[0], WH.shape[0] // 6))
    for a in (W1.T, b1, W2.T, b2, WH, bH):
        f.write(np.ascontiguousarray(a, dtype="<f4").tobytes())
print(f"net.bin: {(OUT / 'net.bin').stat().st_size} bytes")
