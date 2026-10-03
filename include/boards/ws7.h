// Waveshare ESP32-S3-Touch-LCD-7: ESP32-S3, 16 MB flash, 8 MB OPI PSRAM,
// ST7262 800x480 RGB panel, GT911 capacitive touch.
//
// Hardware notes (from the board's documentation and the Chess System Tal
// Retro port, which runs on the same board):
//   - The panel is driven by ESP-IDF 5's esp_lcd RGB driver with bounce
//     buffers: DMA reads two small internal buffers that an interrupt refills
//     from the PSRAM frame buffer, so flash reads don't make the picture jump.
//     Hence this board builds on arduino-esp32 3.x (the pioarduino platform).
//   - A CH422G I2C expander drives the backlight, the LCD and touch resets and
//     the USB/CAN switch, which MUST select USB or the PC loses the board.
//   - The GT911 touch controller shares the I2C bus. Capacitive: no calibration.
//   - Flash through the "USB" socket; if the port is missing, hold BOOT while
//     plugging in (then power-cycle after flashing).
#pragma once
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-7 800x480"
#define BOARD_ROTATION 0
#define FONT_PX_S 24
#define FONT_PX_L 32
static const int W = 800, H = 480;

#define BOARD_TOUCH_CALIBRATION 0
#define BOARD_ANIM_DEFAULT 1  // Normal; changeable in the menu (Speed)
#define BOARD_NET_PLACE NET_INTERNAL  // PSRAM board: layer 2 in SRAM, the rest in PSRAM
#define BOARD_SPRITE_PSRAM false      // the 32 KB band sprite fits internal SRAM (drawing there is fast)

// CH422G: not register based - each I2C "address" is a command.
enum { WS7_CH422_SET = 0x24, WS7_CH422_OUT = 0x38 };
enum { WS7_TP_RST = 1 << 1, WS7_BL = 1 << 2, WS7_LCD_RST = 1 << 3, WS7_SD_CS = 1 << 4,
       WS7_USB_SEL = 1 << 5 };  // USB_SEL low = USB, high = CAN
enum { WS7_TP_INT = 4, WS7_SDA = 8, WS7_SCL = 9 };
static uint8_t ws7_exio;
static esp_lcd_panel_handle_t ws7_panel;
static uint8_t ws7_gt911 = 0x5D;  // 0x5D with INT held low through reset, else 0x14

static void ws7_exio_write(uint8_t v) {
  ws7_exio = v;
  Wire.beginTransmission(WS7_CH422_OUT);
  Wire.write(v);
  Wire.endTransmission();
}

// ST7262 800x480 timings from the board's official ESP32_Display_Panel config.
static bool ws7_lcd_begin() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 16 * 1000 * 1000;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = W * 8;  // 2 x 12.5 KB internal; must divide the frame
  cfg.dma_burst_size = 64;
  cfg.timings.h_res = W;
  cfg.timings.v_res = H;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width = 16;
  cfg.hsync_gpio_num = 46;
  cfg.vsync_gpio_num = 3;
  cfg.de_gpio_num = 5;
  cfg.pclk_gpio_num = 7;
  static const int data[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};  // B3-7, G2-7, R3-7
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
  cfg.disp_gpio_num = -1;
  cfg.flags.fb_in_psram = 1;
  if (esp_lcd_new_rgb_panel(&cfg, &ws7_panel) != ESP_OK) return false;
  esp_lcd_panel_reset(ws7_panel);
  esp_lcd_panel_init(ws7_panel);
  return true;
}

static void display_begin() {
  Wire.begin(WS7_SDA, WS7_SCL, 400000);
  Wire.beginTransmission(WS7_CH422_SET);
  Wire.write(0x01);  // IO0..7 as outputs
  Wire.endTransmission();
  ws7_exio_write(WS7_SD_CS);  // resets low, backlight off, SD deselected, USB selected
  pinMode(WS7_TP_INT, OUTPUT);
  digitalWrite(WS7_TP_INT, LOW);  // GT911 address 0x5D
  delay(10);
  ws7_exio_write(ws7_exio | WS7_TP_RST | WS7_LCD_RST);
  delay(100);
  pinMode(WS7_TP_INT, INPUT);
  delay(50);
  Wire.beginTransmission(ws7_gt911);
  if (Wire.endTransmission() != 0) ws7_gt911 = 0x14;
  if (!ws7_lcd_begin()) Serial.println("RGB panel init failed");
  ws7_exio_write(ws7_exio | WS7_BL);  // backlight on
}

// Copy columns [x0, x1) of a rendered band to screen row oy. The sprite holds
// RGB565 byte-swapped (LovyanGFX's SPI order); the RGB bus wants it native.
// Ten rows at a time through a 16 KB buffer (small enough to leave internal
// SRAM for the band sprite itself - drawing into PSRAM is several times slower).
static void display_push(LGFX_Sprite& band, int oy, int x0, int x1) {
  const int ROWS = 10;
  static uint16_t buf[W * 10];
  int h = band.height(), w = x1 - x0;
  if (oy + h > H) h = H - oy;
  if (w <= 0 || h <= 0) return;
  const uint16_t* src = (const uint16_t*)band.getBuffer();
  for (int r0 = 0; r0 < h; r0 += ROWS) {
    int n = min(ROWS, h - r0);
    for (int r = 0; r < n; r++) {
      const uint16_t* s = src + (r0 + r) * band.width() + x0;
      uint16_t* d = buf + r * w;
      for (int i = 0; i < w; i++) d[i] = __builtin_bswap16(s[i]);
    }
    esp_lcd_panel_draw_bitmap(ws7_panel, x0, oy + r0, x1, oy + r0 + n, buf);
  }
}

static bool ws7_gt_read(uint16_t reg, uint8_t* buf, int n) {
  Wire.beginTransmission(ws7_gt911);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(int(ws7_gt911), n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// The GT911 reports new data with bit 7 of 0x814E; between reports the last
// state holds. Returns whether a finger is down, and where.
//
// While a finger is down the GT911 reports about every 10 ms. It also keeps
// its last report until we acknowledge it, so if the game was busy (an
// animation) when the finger lifted, the "lifted" report is never written and
// the last "down" one is read afterwards - the board would think the finger
// was still there and miss the next tap. So: no fresh report for 60 ms while
// down means the finger has gone.
static bool ws7_raw;  // serial 'R': log every GT911 report
static void touch_debug(bool on) { ws7_raw = on; }
static bool touch_get(int32_t* x, int32_t* y) {
  static bool down;
  static int32_t lx, ly;
  static uint32_t lastReport;
  uint8_t st;
  if (ws7_gt_read(0x814E, &st, 1) && (st & 0x80)) {
    down = false;
    if (st & 0x0F) {
      uint8_t p[5];  // track id, x lo/hi, y lo/hi of the first point
      if (ws7_gt_read(0x814F, p, 5)) { lx = p[1] | p[2] << 8; ly = p[3] | p[4] << 8; down = true; }
    }
    Wire.beginTransmission(ws7_gt911);
    Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
    Wire.endTransmission();
    if (ws7_raw) Serial.printf("%u gt st=%02x %s %d,%d (gap %u)\n", millis(), st, down ? "down" : "up", lx, ly, millis() - lastReport);
    lastReport = millis();
  } else if (down && millis() - lastReport > 60) {
    down = false;  // reports stopped: the finger lifted while we weren't looking
    if (ws7_raw) Serial.printf("%u gt timeout -> up\n", millis());
  }
  *x = lx; *y = ly;
  return down;
}

static void display_cross(int x, int y) {  // where a touch registered
  static uint16_t white[13] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                               0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
  if (x < 6 || y < 6 || x + 7 > W || y + 7 > H) return;
  esp_lcd_panel_draw_bitmap(ws7_panel, x - 6, y, x + 7, y + 1, white);
  esp_lcd_panel_draw_bitmap(ws7_panel, x, y - 6, x + 1, y + 7, white);
}

static void touch_calibrate(uint16_t cal[8]) {}  // capacitive: nothing to calibrate
static void touch_set_calibration(uint16_t cal[8]) {}
