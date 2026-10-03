// 4.0" ESP32-32E display (lcdwiki E32R40T, sold as Hosyond): ESP32-D0WD-V3,
// 4 MB flash, no PSRAM, ST7796S 480x320 SPI, XPT2046 resistive touch on the
// same SPI bus. Pins from lcdwiki "4.0inch ESP32-32E Display".
//
// LAYOUT_W / LAYOUT_H (build flags) draw a smaller layout in the panel's top-left
// corner - the e32r40t_320x240 env uses that to try the 320x240 layout here.
#pragma once
#include <LovyanGFX.hpp>

#define BOARD_NAME "E32R40T 4.0in ESP32-32E"
#define BOARD_ROTATION 1
#ifndef LAYOUT_W
#define LAYOUT_W 480
#define LAYOUT_H 320
#endif
#ifndef FONT_PX_S
#define FONT_PX_S 16
#define FONT_PX_L 21
#endif
static const int W = LAYOUT_W, H = LAYOUT_H;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_XPT2046 touch;

 public:
  LGFX() {
    {
      auto c = bus.config();
      c.spi_host = HSPI_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_dc = 2;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      bus.config(c);
      panel.setBus(&bus);
    }
    {
      auto c = panel.config();
      c.pin_cs = 15; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 320; c.panel_height = 480;
      c.readable = true;
      c.invert = false;
      c.rgb_order = false;
      c.bus_shared = true;
      panel.config(c);
    }
    {
      auto c = light.config();
      c.pin_bl = 27; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      light.config(c);
      panel.setLight(&light);
    }
    {
      auto c = touch.config();
      c.x_min = 300; c.x_max = 3900; c.y_min = 200; c.y_max = 3800;
      c.pin_int = 36;
      c.bus_shared = true;
      c.spi_host = HSPI_HOST;
      c.freq = 1000000;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_cs = 33;
      touch.config(c);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};

// ---- the board API the game uses (see include/board.h) ----
#define BOARD_TOUCH_CALIBRATION 1  // resistive: calibrated on the board, stored in NVS
#define BOARD_NET_PLACE NET_FLASH_SRAM
#define BOARD_SPRITE_PSRAM false
#define BOARD_ANIM_DEFAULT 0  // Slow: the pace tuned on the 4in board (Speed in the menu)
static LGFX lcd;

static void display_begin() {
  lcd.init();
  lcd.setRotation(BOARD_ROTATION);
  lcd.fillScreen(TFT_BLACK);  // a smaller test layout leaves the rest of the panel black
  lcd.setBrightness(200);
}
// Copy columns [x0, x1) of a rendered band (rows 0..band height) to screen row oy.
static void display_push(LGFX_Sprite& band, int oy, int x0, int x1) {
  lcd.setClipRect(x0, oy, x1 - x0, band.height());
  band.pushSprite(&lcd, 0, oy);
  lcd.clearClipRect();
}
static bool touch_get(int32_t* x, int32_t* y) { return lcd.getTouch(x, y); }
static void display_cross(int x, int y) {  // where a touch registered
  lcd.drawFastHLine(x - 6, y, 13, TFT_WHITE);
  lcd.drawFastVLine(x, y - 6, 13, TFT_WHITE);
}
static void touch_calibrate(uint16_t cal[8]) {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&fonts::FreeSans12pt7b);
  lcd.setTextDatum(middle_center);
  lcd.drawString("Touch calibration", W / 2, H / 2 - 16);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.drawString("Tap each corner arrow as it appears", W / 2, H / 2 + 14);
  lcd.calibrateTouch(cal, TFT_YELLOW, TFT_BLACK, 24);
}
static void touch_set_calibration(uint16_t cal[8]) { lcd.setTouchCalibrate(cal); }
static void touch_debug(bool on) {}  // raw touch logging: nothing extra on this board
