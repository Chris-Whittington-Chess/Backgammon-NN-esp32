// 2.8" "Cheap Yellow Display" ESP32-2432S028R: ESP32-WROOM-32, 4 MB flash,
// no PSRAM, ILI9341 320x240 SPI, XPT2046 resistive touch on its OWN SPI bus.
// Pins from the community CYD documentation
// (github.com/witnessmenow/ESP32-Cheap-Yellow-Display).
//
// UNTESTED on hardware - an example of a second board. Some CYD revisions
// (often sold as "CYD2USB", with two USB ports) use an ST7789 panel instead:
// switch Panel_ILI9341 to Panel_ST7789 and try invert = true if the colours
// look wrong.
#pragma once
#include <LovyanGFX.hpp>

#define BOARD_NAME "CYD ESP32-2432S028R 2.8in"
#define BOARD_ROTATION 1
#define FONT_PX_S 11
#define FONT_PX_L 14
static const int W = 320, H = 240;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel;
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
      c.panel_width = 240; c.panel_height = 320;
      c.readable = true;
      c.invert = false;
      c.rgb_order = false;
      c.bus_shared = false;
      panel.config(c);
    }
    {
      auto c = light.config();
      c.pin_bl = 21; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      light.config(c);
      panel.setLight(&light);
    }
    {
      auto c = touch.config();  // the touch controller has its own bus on the CYD
      c.x_min = 300; c.x_max = 3900; c.y_min = 200; c.y_max = 3800;
      c.pin_int = 36;
      c.bus_shared = false;
      c.spi_host = VSPI_HOST;
      c.freq = 1000000;
      c.pin_sclk = 25; c.pin_mosi = 32; c.pin_miso = 39; c.pin_cs = 33;
      touch.config(c);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};
