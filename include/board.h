// Board selection: everything specific to one display board lives in its
// header under boards/, chosen by a build flag in platformio.ini. A board
// header provides:
//   - class LGFX            the LovyanGFX panel / bus / backlight / touch setup
//   - W, H                  the layout size in pixels (landscape)
//   - BOARD_ROTATION        LovyanGFX rotation that makes the panel landscape
//   - FONT_PX_S, FONT_PX_L  the small / large font sizes (data/fonts/*.vlw)
//   - BOARD_NAME            for the serial log
//   - BOARD_TOUCH_CALIBRATION  1 = resistive (4-corner calibration, NVS), 0 = none
//   - BOARD_NET_PLACE       where the network's weights go (NET_FLASH_SRAM without PSRAM)
//   - display_begin(), display_push(band, oy, x0, x1), display_cross(x, y),
//     touch_get(&x, &y), touch_calibrate(cal), touch_set_calibration(cal)
//     - the only way the game reaches the hardware
// The game's layout is computed from W and H (see "geometry" in main.cpp).
// To port to a new display, copy a board header, change its panel, touch and
// pins, and add a build env - see the README, "Porting to another display".
#pragma once

#if defined(BOARD_E32R40T)
#include "boards/e32r40t.h"
#elif defined(BOARD_CYD28)
#include "boards/cyd28.h"
#elif defined(BOARD_WS7)
#include "boards/ws7.h"
#else
#error "No board selected: add -DBOARD_xxx to build_flags (see include/board.h)"
#endif
