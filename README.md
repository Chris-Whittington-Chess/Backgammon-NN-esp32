# Backgammon-NN-esp32

Play backgammon against the [Backgammon-NN](https://github.com/Chris-Whittington-Chess/Backgammon-NN)
value net on a cheap ESP32 touchscreen board.

Engine source and trained weights come from the public Backgammon-NN repo, pinned as a submodule
in `external/Backgammon-NN`:

```
git clone --recurse-submodules <this repo>
```

## Board: 4.0" ESP32-32E display (`e32r40t`)

lcdwiki "4.0inch ESP32-32E Display" (E32R40T, sold as Hosyond): ESP32-D0WD-V3 at 240 MHz, 4 MB
flash, **no PSRAM**, ST7796S 480x320 SPI panel, XPT2046 resistive touch, CH340 USB serial.

```
pio run -e e32r40t -t upload
```

### Playing

You are white, moving 24 -> 1 (home board bottom right); the CPU plays the net at 0-ply.

- Tap the dice (ROLL) to roll. Points you can move from get a yellow bar at their base.
- Tap a checker, then a yellow dot (one die) or ring (several dice with that checker); tap the
  tray to bear off. A checker with only one destination moves straight away. Taps snap to the
  nearest target / movable column, and a white cross shows where the touch registered.
- The CPU's last move is marked in orange: rings where checkers left, dots where they landed.
- **Doubling cube** (money play): before rolling, tap the cube in the bar (yellow rim = you may
  double). The CPU doubles before its roll and you get Take / Drop.
- **Menu** (top left): Resume, New game, Take back (this turn, or your previous turn including
  the CPU's reply and any cube action), Hint (best move in cyan; before rolling: cube advice),
  Reset score, Calibrate touch.

Touch calibration runs on first boot and is stored in NVS.

### How it fits without PSRAM

- The frame is drawn in 480x20 bands through one 19 KB sprite.
- The net is read from flash in place, except the dense layer-2 matrix (128 KB, read in full
  every eval), which is copied to SRAM as four 32 KB chunks - the heap is too fragmented for
  bigger blocks.

### Engine

- `src/common/nn.cpp`: the 198 -> 256 -> 128 -> 12x6 class-aware net (sparse first layer,
  ReLU-skipping second layer, routed head), mirroring `bgcore` features / routing.
- `src/common/bg.cpp`: rules and move generation ported from `bgcore` `moves.rs`
  (`genmoves_playout` + `next_submoves`), the 0-ply move choice from `game.rs`, and the cube.
- Cube decisions use Janowski's cubeful model on the net's cubeless outcome probabilities, cube
  efficiency x = 0.68 (gammonless: take point ~21%, initial double ~69%).
- The race EGTB is not used (484 MB vs 4 MB flash); it was 0-ply checker-play-neutral on the PC.

### Verification

Serial (921600 baud) `v` runs `data/movetests.bin`: 420 position/roll cases with the exact
legal-move set and the 0-ply choice from bgcore + PyTorch. Current result: 0 move-set and 0
move-choice mismatches; 3.7 ms/eval, ~84 ms per average CPU move, ~1 s for the largest roll
(344 legal moves).

Other serial commands: `n` new game, `t X Y` simulated tap, `d` dump the frame (see
`tools/grab.py`), `k` recalibrate, `g` / `h` load cube-test race positions.

## Net export

After bumping the submodule, with a Backgammon-NN Python env (torch + `bgcore` bindings):

```
python tools/export_net.py         # data/net.bin
python tools/export_movetests.py   # data/movetests.bin
```
