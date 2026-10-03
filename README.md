# Backgammon-NN-esp32

Play backgammon against the [Backgammon-NN](https://github.com/Chris-Whittington-Chess/Backgammon-NN)
value net on a cheap ESP32 touchscreen board.

Website: [whittingtonchess.com/retro-backgammon.html](https://whittingtonchess.com/retro-backgammon.html).

The trained network ships ready to use in `data/net.bin`, so building and flashing needs only this
repo. The engine and its training live in the [Backgammon-NN](https://github.com/Chris-Whittington-Chess/Backgammon-NN)
repo, pinned as a submodule in `external/Backgammon-NN`; you need it only to re-export the net
(see [Net export](#net-export)):

```
git clone --recurse-submodules https://github.com/Chris-Whittington-Chess/Backgammon-NN-esp32
```

## Board: 4.0" ESP32-32E display (`e32r40t`)

lcdwiki "4.0inch ESP32-32E Display" (E32R40T, sold as Hosyond): ESP32-D0WD-V3 at 240 MHz, 4 MB
flash, **no PSRAM**, ST7796S 480x320 SPI panel, XPT2046 resistive touch, CH340 USB serial.

```
pio run -e e32r40t -t upload
```

### Ready-made firmware (no build needed)

Each [release](https://github.com/Chris-Whittington-Chess/Backgammon-NN-esp32/releases) has one
merged file, `retro-backgammon-<version>-e32r40t.bin`, to write at address 0:

- **From the browser:** the Install button on
  [whittingtonchess.com/retro-backgammon.html](https://whittingtonchess.com/retro-backgammon.html#install)
  (Chrome or Edge on a PC or Mac, board plugged in by USB).
- **With esptool:** `esptool.py --chip esp32 write_flash 0x0 retro-backgammon-<version>-e32r40t.bin`

Making one: `pio run -e e32r40t`, then `python tools/merge_firmware.py <version>` with
PlatformIO's Python (it has esptool); the file lands in `dist/`.

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

**Resigning and claiming a win**

The CPU judges both by what playing on is worth: the net's cubeless equity, with you on roll at
the start of your turn (or with the CPU on roll once your move is done, or you can't move).

- **Resign** (menu): offer a single, gammon or backgammon. The CPU accepts anything worth at
  least what it expects from playing on. If not, it says why ("It expects 1.82 by playing on")
  and counter-offers the smallest resignation it would take - e.g. *Resign gammon (2)* - or you
  can play on.
- **Claim win** (menu): ask the CPU to concede a single, gammon or backgammon. It concedes if
  you'd expect at least that much by playing on. If not, it says what you expect and offers the
  most it would concede - e.g. *Take single (1)* - or, if you aren't clearly winning, simply
  refuses.
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
| Claim win | Ask the CPU to concede a single, gammon or backgammon (times the cube), or cancel |

When a game ends (played out, dropped or resigned) the status bar shows the result and score
(you-CPU), and NEW GAME blinks in the tray: tap it to start the next game. Touch calibration also runs on first boot and is stored in NVS.

### How it fits without PSRAM

- The frame is drawn in 20-row bands through one small sprite (19 KB at 480 wide); checker faces
  are pre-rendered once into tiny sprites, so a full redraw takes ~0.35 s. Animation redraws only the bands
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
`tools/grab.py`), `k` recalibrate, `a N` self-play (the net plays both sides for N exchanges
with random dice, the last CPU move shown as usual - real-game positions for screenshots). Test positions: `g` / `h` cube races, `p` closed out (pass),
`q` about to be gammoned (resign), `r` CPU hopeless (it resigns).

## Porting to another display

The game itself (`src/game/main.cpp`) knows nothing about the hardware. Everything specific to a
board lives in one header under `include/boards/`, picked by a build flag:

| Build env | Board | Status |
|---|---|---|
| `e32r40t` | 4.0" ESP32-32E display, ST7796S 480x320, XPT2046 touch | reference board |
| `e32r40t_320x240` | the same board, drawing the 320x240 layout in its top-left corner | test of the small layout |
| `ws7` | Waveshare ESP32-S3-Touch-LCD-7: ESP32-S3, 8 MB PSRAM, 800x480 RGB panel, GT911 capacitive touch | tested |
| `cyd28` | 2.8" "Cheap Yellow Display" ESP32-2432S028R, ILI9341 320x240, XPT2046 touch | example, untested on hardware |

The game reaches the hardware only through a few functions each board header provides:
`display_begin()`, `display_push()` (put a rendered 20-row band on screen), `display_cross()`,
`touch_get()`, and `touch_calibrate()` / `touch_set_calibration()` for resistive screens. Boards
LovyanGFX drives directly (the SPI panels) implement them in a few lines; `ws7` drives its RGB
panel through ESP-IDF 5's `esp_lcd` driver (with bounce buffers, so flash reads don't disturb the
picture) and builds on the pioarduino platform, while the classic-ESP32 boards stay on
PlatformIO's espressif32 7.1.3 (arduino-esp32 2.0.17). A board header also says where the
network goes (`BOARD_NET_PLACE`: flash plus SRAM without PSRAM, PSRAM plus SRAM with it).

**A new board with a 480x320 or 320x240 screen**

1. Copy the nearest header in `include/boards/` and change the `LGFX` class: the panel type
   (LovyanGFX supports e.g. `Panel_ST7796`, `Panel_ILI9341`, `Panel_ILI9488`, `Panel_ST7789`,
   RGB parallel panels), the SPI bus and pins, the backlight pin, and the touch controller
   (`Touch_XPT2046`, `Touch_FT5x06`, `Touch_GT911`, `Touch_CST816S`, ...). Set `W`, `H`
   (landscape), `BOARD_ROTATION` and the two font sizes.
2. Add a `#elif defined(BOARD_xxx)` line for it in `include/board.h`.
3. Add a `[env:xxx]` to `platformio.ini` with `-DBOARD_xxx`, its board and pins, and the font
   files for its sizes.

If the colours come out wrong, try the panel's `invert` and `rgb_order` settings. Touch is
calibrated on the board at first boot, so the touch ranges in the header only need to be roughly
right.

**A different resolution**

The layout was designed at 480x320 and is computed from `W` and `H`: widths scale with `W`,
heights with `H`, and checkers, dice, text and menus with the smaller of the two, so the pieces
are always round and everything is drawn in code at the new size (there are no images). Two
compile-time checks stop a layout whose checkers wouldn't fit. Fonts are the one thing made in
advance: add the sizes to `SIZES` in `tools/make_fonts.py` and re-run it (16 / 21 px suit
480x320, 11 / 14 px suit 320x240, 24 / 32 px suit 800x480).

**What a board needs**

- A touch screen, landscape (or rotated to landscape), **320x240 or larger**. At 320x240 the points
  are about 21 px wide - a fingertip works, a stylus is easier. Below that the checkers and text
  get too small to use.
- An ESP32 with **4 MB flash** (the program is ~0.95 MB, including the 364 KB network) and
  ~128 KB of free internal RAM for the network's second layer. PSRAM isn't needed; an ESP32-S3
  with PSRAM is faster, and large parallel-RGB panels (800x480) need it for their frame buffer.

## Net export

After bumping the submodule, with a Backgammon-NN Python env (torch + `bgcore` bindings):

```
python tools/export_net.py         # data/net.bin
python tools/export_movetests.py   # data/movetests.bin
```

## Licence

Copyright 2026 Chris Whittington. Licensed under the [Apache License 2.0](LICENSE), including the
trained network in `data/net.bin`.

You may use, modify and share this code and the programs built from it, including commercially -
but anything you distribute, as source or as a compiled program, must carry the [NOTICE](NOTICE)
file crediting Chris Whittington (in a NOTICE file, its documentation, or a screen the program
shows). Third-party parts (the LovyanGFX library, the DejaVu fonts) keep their own licences: see
[THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES).

The Backgammon-NN submodule is a separate repo under its own licence, which this one does not change.
