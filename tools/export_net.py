"""Export the live Backgammon-NN value net for the CoreS3, plus parity tests.

Weights and model code come from the Backgammon-NN submodule (external/).
Run with a Backgammon-NN venv that has torch + the bgcore bindings built:
  ..\\Backgammon-2026\\.venv\\Scripts\\python.exe tools/export_net.py

data/net.bin  (little-endian float32 unless noted)
  magic "BGN1", u32 n_in=198, h1=256, h2=128, n_heads=12
  W1T [198][256]  first layer, input-major (sparse input accumulation)
  b1  [256]
  W2T [256][128]  second layer, input-major (skips ReLU-zero inputs)
  b2  [128]
  WH  [72][128]   heads, output-major (only the routed head's 6 rows are used)
  bH  [72]
data/tests.bin
  u32 count, then per position 56 bytes:
  i8 points[24] (mover-relative, + mover), u8 bar[2], u8 off[2], u8 route,
  u8 n_children (legal moves for a random roll, capped 255), u8 pad[2],
  f32 probs[6] (PyTorch softmax of the routed head)
"""
import random, struct, sys
from pathlib import Path

import numpy as np
import torch

REPO = Path(__file__).resolve().parents[1] / "external" / "Backgammon-NN"
sys.path.insert(0, str(REPO / "trainer"))
import bgcore  # noqa: E402
from model import net_bucketed_from_state  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "data"
OUT.mkdir(exist_ok=True)

ck = torch.load(REPO / "models" / "td_latest.pt", map_location="cpu", weights_only=False)
assert ck.get("class_aware") and ck["act"] == "relu", "expects the class-aware ReLU net"
net = net_bucketed_from_state(ck["model"], ck["hidden"], ck["act"])
sd = {k: v.numpy().astype("<f4") for k, v in ck["model"].items()}
W1, b1 = sd["body.0.weight"], sd["body.0.bias"]    # [256,198]
W2, b2 = sd["body.2.weight"], sd["body.2.bias"]    # [128,256]
WH, bH = sd["heads.weight"], sd["heads.bias"]      # [72,128]
n_heads = WH.shape[0] // 6

with open(OUT / "net.bin", "wb") as f:
    f.write(b"BGN1" + struct.pack("<4I", 198, W1.shape[0], W2.shape[0], n_heads))
    for a in (W1.T, b1, W2.T, b2, WH, bH):
        f.write(np.ascontiguousarray(a, dtype="<f4").tobytes())
print(f"net.bin: {(OUT / 'net.bin').stat().st_size} bytes")

# Test positions: every position met in random-play games (mover-relative),
# which covers contact, crashed and race heads.
rng = random.Random(2026)
boards = [bgcore.Board.starting()]
while len(boards) < 600:
    b = bgcore.Board.starting()
    for _ in range(400):
        kids = bgcore.legal_moves(b, rng.randint(1, 6), rng.randint(1, 6))
        b = rng.choice(kids)
        if b.is_terminal():
            break
        b = b.swap_perspective()
        if rng.random() < 0.25:
            boards.append(b)
boards = boards[:600]

feats = torch.tensor(np.stack([b.features() for b in boards]), dtype=torch.float32)
routes = torch.tensor([b.route_bucket() for b in boards])
with torch.no_grad():
    probs = net.probs_for(feats, routes).numpy()

nkids = []
with open(OUT / "tests.bin", "wb") as f:
    f.write(struct.pack("<I", len(boards)))
    for b, r, p in zip(boards, routes.tolist(), probs):
        n = len(bgcore.legal_moves(b, rng.randint(1, 6), rng.randint(1, 6)))
        nkids.append(n)
        f.write(struct.pack("<24b", *[b.point(i) for i in range(1, 25)]))
        f.write(struct.pack("<6B", b.bar(0), b.bar(1), b.off(0), b.off(1), r, min(n, 255)))
        f.write(b"\0\0" + struct.pack("<6f", *p))
heads = np.bincount(routes.numpy(), minlength=n_heads)
print(f"tests.bin: {len(boards)} positions, heads used {heads.tolist()}")
print(f"legal moves per roll: mean {np.mean(nkids):.1f}, max {max(nkids)}")
