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

**Your turn**

1. **Roll:** tap the dice when ROLL blinks in the tray. The dice tumble, then settle.
2. **Move:** points you can move from get a yellow bar at their base (the bar too, when you have
   a checker on it). Tap a checker to select it, then tap where it goes:
   - a yellow **dot** = a move with one die,
   - a yellow **ring** = a move using several dice with that checker (e.g. 13 to 2 with 6-5),
   - the **tray** to bear off.

   Nothing moves until you tap a destination. Tap another movable checker to switch, or empty
   space to deselect. Used dice turn grey; with doubles a count shows how many are left.
3. **Finish:** after your last checker the dice grey out and DONE blinks under them. Tap the
   dice to hand over to the CPU - until then you can still undo (see the menu).

If a move is refused, the status bar says why ("Must play both dice", "Enter from the bar
first", ...). Taps snap to the nearest target or movable column (resistive touch is a few
pixels out), and a small white cross shows where each touch registered.

**The CPU's turn**

Its dice tumble, then its checkers slide one at a time; a hit blot flies to the bar. Afterwards
orange marks show its move: rings where checkers left, dots where they landed.

If your roll has no legal move, the dice grey out and PASS blinks under them: tap the dice to
hand over, as with DONE.

**Doubling cube** (money play)

- Before rolling, tap the cube in the bar to double. A yellow surround means you may (cube
  centred or yours). The CPU takes or drops.
- The CPU may double before its roll: a panel shows your cubeless chances, with Take / Drop.
- The cube sits at the owner's end of the bar; wins are multiplied by it, a drop scores the
  current value.

**Resigning**

- Menu > Resign offers a single, gammon or backgammon. The CPU accepts if that's at least what
  it expects from playing on (the net's equity with you on roll at the start of your turn) -
  e.g. it refuses a single when it's likely to gammon you - and says what it expected.
- A hopeless CPU (under 0.2% to win) resigns before its roll: a single, or a gammon /
  backgammon if you have real chances of one. Accept to take the points, or Refuse to play on
  (it won't offer again that game).

**Menu** (the three lines, top left; tap outside the panel to close it)

| Button | |
|---|---|
| Undo step | Undo the last checker you moved this turn and get its dice back; press again to go further |
| New game | Start again (the score is kept) |
| Undo move | Undo your whole turn so far; if you haven't moved (or the CPU has replied), go back to the start of your previous turn, undoing the CPU's move and any cube action |
| Hint | Your turn: the best move in cyan (rings = from, dots = to). Before rolling: cube advice |
| Resign | Offer a single, gammon or backgammon (times the cube), or cancel |
| Reset score | Score back to 0-0 |
| Calibrate touch | Re-run the 4-corner touch calibration |
| Close | Close the menu |

When a game ends the status message pulses with the result and score; tap to start the next
game. Touch calibration also runs on first boot and is stored in NVS.

### How it fits without PSRAM

- The frame is drawn in 480x20 bands through one 19 KB sprite. Animation redraws only the bands
  a moving piece crosses, clipped to its columns (~140 fps); fonts are anti-aliased DejaVu Sans
  rendered by `tools/make_fonts.py`.
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
`tools/grab.py`), `k` recalibrate. Test positions: `g` / `h` cube races, `p` closed out (pass),
`q` about to be gammoned (resign), `r` CPU hopeless (it resigns).

## Net export

After bumping the submodule, with a Backgammon-NN Python env (torch + `bgcore` bindings):

```
python tools/export_net.py         # data/net.bin
python tools/export_movetests.py   # data/movetests.bin
```
