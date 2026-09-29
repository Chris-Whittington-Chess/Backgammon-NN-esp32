# m5-backgammon

The Backgammon-NN engine on an M5Stack CoreS3 (ESP32-S3, 320x240 touch, 16 MB flash, 8 MB PSRAM).

Engine source and trained weights come from the public
[Backgammon-NN](https://github.com/Chris-Whittington-Chess/Backgammon-NN) repo, pinned as a
submodule in `external/Backgammon-NN`:

```
git clone --recurse-submodules <this repo>
```

## Firmwares (PlatformIO envs)

- `board` - touch board display test: tap the status bar to cycle positions, the dice to roll,
  a point then a yellow dot to move.
- `nnbench` - runs the live value net on-device, checks parity against PyTorch on 600
  positions and times it. Serial `b` re-runs and prints the report.

```
pio run -e nnbench -t upload
```

## Net export

`tools/export_net.py` writes `data/net.bin` (weights) and `data/tests.bin` (parity positions)
from the submodule's `models/td_latest.pt`. Needs a Backgammon-NN Python env with torch and the
`bgcore` bindings. Re-run it after bumping the submodule.

## Results (v1.10.0 net, 240 MHz)

| Weights | us/eval | avg move (20.5 children) | parity vs PyTorch |
|---|---|---|---|
| f32, flash | 2898 | 59 ms | exact (1e-6) |
| f32, PSRAM | 2727 | 56 ms | exact |
| f32, layer 2 in on-chip SRAM | 1481 | 30 ms | exact |
| int8, on-chip SRAM | 881 | 18 ms | mean dEq 0.033 - too lossy |

Layer 2 (128 KB) is read in full every eval, so it is bandwidth-bound from PSRAM through the
32 KB cache; putting just that matrix in internal SRAM halves eval time.

Move generation is not ported yet (children counts come from the PC), and the race EGTB is not
used on-device.
