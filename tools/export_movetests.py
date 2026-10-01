"""Move-generation and 0-ply move-choice parity tests for the on-device port.

For each (position, roll): the exact set of legal resulting boards from
bgcore.legal_moves, and the net's 0-ply choice (game.rs EvalEngine: a won
child scores its points, otherwise -equity of the turn-passed child, scored
with PyTorch and no bear-off table, matching what the device runs).

data/movetests.bin
  u32 count, then per test 44 bytes:
  i8 points[24], u8 bar[2], u8 off[2], u8 d1, u8 d2, u16 n_children,
  u32 set_hash (sum of fmix32(fnv1a(child))), u32 best_hash, f32 best_score

Run with a Backgammon-NN venv (torch + bgcore):
  ..\\Backgammon-2026\\.venv\\Scripts\\python.exe tools/export_movetests.py
"""
import random, struct, sys
from pathlib import Path

import numpy as np
import torch

REPO = Path(__file__).resolve().parents[1] / "external" / "Backgammon-NN"
sys.path.insert(0, str(REPO / "trainer"))
import bgcore  # noqa: E402
from model import net_bucketed_from_state  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "data" / "movetests.bin"
M32 = 0xFFFFFFFF


def raw(b):
    return [b.point(i) for i in range(1, 25)], [b.bar(0), b.bar(1)], [b.off(0), b.off(1)]


def fnv(b):
    pts, bar, off = raw(b)
    h = 2166136261
    for v in [p & 0xFF for p in pts] + bar + off:
        h = ((h ^ v) * 16777619) & M32
    return h


def fmix(h):
    h ^= h >> 16; h = (h * 0x85EBCA6B) & M32
    h ^= h >> 13; h = (h * 0xC2B2AE35) & M32
    return h ^ (h >> 16)


ck = torch.load(REPO / "models" / "td_latest.pt", map_location="cpu", weights_only=False)
net = net_bucketed_from_state(ck["model"], ck["hidden"], ck["act"])
PTS = torch.tensor([1.0, 2.0, 3.0, -1.0, -2.0, -3.0])


def scores(kids):
    out = np.zeros(len(kids), dtype=np.float64)
    pend = [i for i, k in enumerate(kids) if not (k.winner_points() or 0) > 0]
    for i, k in enumerate(kids):
        if i not in pend:
            out[i] = k.winner_points()
    if pend:
        sw = [kids[i].swap_perspective() for i in pend]
        x = torch.tensor(np.stack([s.features() for s in sw]), dtype=torch.float32)
        r = torch.tensor([s.route_bucket() for s in sw])
        with torch.no_grad():
            eq = (net.probs_for(x, r) * PTS).sum(-1).numpy()
        out[pend] = -eq
    return out


rng = random.Random(7)
cases = []
# Every roll from the opening.
for d1 in range(1, 7):
    for d2 in range(d1, 7):
        cases.append((bgcore.Board.starting(), d1, d2))
# Positions from random games, both early and late.
while len(cases) < 420:
    b = bgcore.Board.starting()
    for _ in range(400):
        d1, d2 = rng.randint(1, 6), rng.randint(1, 6)
        if rng.random() < 0.2:
            cases.append((b, d1, d2))
        kids = bgcore.legal_moves(b, d1, d2)
        b = rng.choice(kids)
        if b.is_terminal():
            break
        b = b.swap_perspective()
cases = cases[:420]

stats = {"bar": 0, "race": 0, "bearoff": 0, "dance": 0}
with open(OUT, "wb") as f:
    f.write(struct.pack("<I", len(cases)))
    maxk = 0
    for b, d1, d2 in cases:
        kids = bgcore.legal_moves(b, d1, d2)
        assert len({fnv(k) for k in kids}) == len(kids), "hash collision or duplicates"
        sc = scores(kids)
        best = int(np.argmax(sc))
        maxk = max(maxk, len(kids))
        stats["bar"] += b.bar(0) > 0
        stats["race"] += b.no_contact()
        stats["bearoff"] += b.off(0) > 0
        stats["dance"] += len(kids) == 1 and kids[0] == b
        pts, bar, off = raw(b)
        f.write(struct.pack("<24b", *pts) + bytes(bar + off + [d1, d2]))
        f.write(struct.pack("<HIIf", len(kids), sum(fmix(fnv(k)) for k in kids) & M32,
                            fnv(kids[best]), float(sc[best])))
print(f"{OUT.name}: {len(cases)} tests, max children {maxk}, {stats}")
