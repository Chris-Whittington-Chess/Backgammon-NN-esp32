// Backgammon board on the 4.0" ESP32-32E display (lcdwiki E32R40T, sold as
// Hosyond): ESP32-D0WD-V3, no PSRAM, ST7796S 480x320 SPI, XPT2046 resistive
// touch on the same bus. Pins from lcdwiki "4.0inch ESP32-32E Display".
//
// No PSRAM means no full-frame sprite (300 KB), so the frame is rendered in
// four 480x80 bands through one 77 KB sprite.
//
// Touch: first boot (or serial 'k') runs a 4-corner calibration, saved in NVS.
// Tap the status bar to cycle positions, the dice to roll, a point then a
// yellow dot to move (the tray to bear off).
// Serial (921600): 'n' next preset, 'r' roll, 't X Y' simulated tap,
// 'k' recalibrate, 'd' dump frame ("FRAME\n" + 480*320 LE RGB565).
#include <LovyanGFX.hpp>
#include <Preferences.h>

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

static LGFX lcd;
static LGFX_Sprite cv(&lcd);
static Preferences prefs;
static const int W = 480, H = 320, BAND = 80;
static int oy = 0;  // y offset of the band being rendered

// ---- position + rules (same as the CoreS3 board sketch) ----
struct Pos {
  int pts[25];
  int bar[2];
  int off[2];
  int cube, cubeOwner;
  const char* eval;
};
static Pos pos;
static int dice[4], ndice = 0;
static int sel = -1;
static int preset = 1;

static void loadPreset(int i) {
  memset(&pos, 0, sizeof pos);
  pos.cube = 64;
  ndice = 0;
  sel = -1;
  auto set = [](int p, int n) { pos.pts[p] = n; };
  if (i == 0) {
    set(24, 2); set(13, 5); set(8, 3); set(6, 5);
    set(1, -2); set(12, -5); set(17, -3); set(19, -5);
    pos.eval = "Your roll";
  } else if (i == 1) {
    set(6, 7); set(8, 2); set(13, 2); set(5, 2); set(4, 1); set(21, 1);
    set(1, -2); set(12, -3); set(17, -2); set(18, -2); set(19, -3); set(20, -2);
    pos.bar[1] = 1; pos.cube = 2; pos.cubeOwner = 1;
    dice[0] = 6; dice[1] = 3; ndice = 2;
    pos.eval = "Win 61.4%  +0.27";
  } else {
    set(1, 3); set(2, 3); set(3, 2); set(4, 2); set(5, 1);
    set(24, -3); set(23, -2); set(21, -2); set(19, -1);
    pos.off[0] = 4; pos.off[1] = 7;
    dice[0] = 5; dice[1] = 2; ndice = 2;
    pos.eval = "Win 38.9%  -0.22";
  }
}

static bool allHome() {
  if (pos.bar[0]) return false;
  for (int p = 7; p <= 24; p++) if (pos.pts[p] > 0) return false;
  return true;
}
static int dest(int p, int d) {
  int t = p - d;
  if (t >= 1) return pos.pts[t] >= -1 ? t : -1;
  if (!allHome()) return -1;
  if (t == 0) return 0;
  for (int q = p + 1; q <= 6; q++) if (pos.pts[q] > 0) return -1;
  return 0;
}
static void useDie(int d) {
  for (int i = 0; i < ndice; i++) if (dice[i] == d) {
    for (int j = i; j < ndice - 1; j++) dice[j] = dice[j + 1];
    ndice--;
    return;
  }
}
static void roll() {
  int a = random(1, 7), b = random(1, 7);
  ndice = 0;
  dice[ndice++] = a; dice[ndice++] = b;
  if (a == b) { dice[ndice++] = a; dice[ndice++] = a; }
  sel = -1;
  pos.eval = "0-ply: n/a";
}
// The turn is over when no remaining die can move any checker: clear the dice
// so the next tap rolls. (Bar entry and must-use-both-dice come with the
// movegen port; this is still a display/touch test.)
static void endIfStuck() {
  for (int i = 0; i < ndice; i++)
    for (int p = 1; p <= 24; p++)
      if (pos.pts[p] > 0 && dest(p, dice[i]) >= 0) return;
  if (ndice) { ndice = 0; sel = -1; pos.eval = "Tap dice to roll"; }
}

static int pips(bool me) {
  int s = 0;
  for (int p = 1; p <= 24; p++) {
    if (me && pos.pts[p] > 0) s += pos.pts[p] * p;
    if (!me && pos.pts[p] < 0) s += -pos.pts[p] * (25 - p);
  }
  return s + pos.bar[me ? 0 : 1] * 25;
}

// ---- geometry (480x320) ----
static const int PW = 32;                  // point width
static const int LX = 6, RFX = 228;         // left / right field x
static const int BARX = 198, BARW = 30;
static const int TRX = 424, TRW = 50;        // tray
static const int FT = 25, FB = 316;        // field top / bottom
static const int PH = 128;                 // point (triangle) height
static const int CR = 13, STEP = 27;       // checker radius, stack step
static const int MIDY = 171;               // dice / centred cube row

static int colX(int c) { return c < 6 ? LX + c * PW : RFX + (c - 6) * PW; }
static void pointGeom(int p, int& c, bool& top) {
  if (p >= 13) { c = p - 13; top = true; } else { c = 12 - p; top = false; }
}
static int stackY(bool top, int i) { return top ? FT + 14 + i * STEP : FB - 14 - i * STEP; }

static uint16_t C_FRAME, C_FELT, C_PTA, C_PTB, C_ME, C_MERIM, C_MEIN, C_OP, C_OPRIM, C_OPIN,
    C_BAR, C_TRAY, C_SEL, C_TEXT, C_DIM, C_GOOD, C_BAD, C_STATUS;

// ---- drawing (all y coordinates are screen space; shifted by the band offset) ----
static void checker(int cx, int cy, bool mine, int r = CR) {
  cy -= oy;
  cv.fillSmoothCircle(cx, cy, r, mine ? C_MERIM : C_OPRIM);
  cv.fillSmoothCircle(cx, cy, r - 1, mine ? C_ME : C_OP);
  cv.drawCircle(cx, cy, r - 5, mine ? C_MEIN : C_OPIN);
}
static void rect(int x, int y, int w, int h, uint16_t c) { cv.fillRect(x, y - oy, w, h, c); }
static void tri(int x0, bool top, uint16_t col) {
  if (top) cv.fillTriangle(x0, FT - oy, x0 + PW - 1, FT - oy, x0 + PW / 2, FT + PH - oy, col);
  else     cv.fillTriangle(x0, FB - 1 - oy, x0 + PW - 1, FB - 1 - oy, x0 + PW / 2, FB - PH - oy, col);
}
static void die(int x, int y, int v) {
  y -= oy;
  cv.fillRoundRect(x, y, 22, 22, 4, 0xF79D);
  static const uint8_t P[7][6][2] = {{}, {{11,11}}, {{6,6},{16,16}}, {{6,6},{11,11},{16,16}},
    {{6,6},{16,6},{6,16},{16,16}}, {{6,6},{16,6},{11,11},{6,16},{16,16}},
    {{6,6},{16,6},{6,11},{16,11},{6,16},{16,16}}};
  for (int i = 0; i < v; i++) cv.fillSmoothCircle(x + P[v][i][0], y + P[v][i][1], 2, TFT_BLACK);
}
static void text(const char* s, int x, int y) { cv.drawString(s, x, y - oy); }
static void number(int n, int x, int y) { cv.drawNumber(n, x, y - oy); }

static void drawBand() {
  rect(0, 0, W, 22, C_STATUS);
  rect(0, 22, W, H - 22, C_FRAME);
  rect(LX, FT, 6 * PW, FB - FT, C_FELT);
  rect(RFX, FT, 6 * PW, FB - FT, C_FELT);
  rect(BARX, 22, BARW, H - 22, C_BAR);
  rect(TRX, FT, TRW, FB - FT, C_TRAY);
  for (int c = 0; c < 12; c++) {
    tri(colX(c), true, c % 2 ? C_PTB : C_PTA);
    tri(colX(c), false, c % 2 ? C_PTA : C_PTB);
  }
  if (sel > 0) {
    int c; bool top; pointGeom(sel, c, top);
    cv.drawRect(colX(c), (top ? FT : FB - 5 * STEP - 1) - oy, PW, 5 * STEP + 1, C_SEL);
  }
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextDatum(middle_center);
  for (int p = 1; p <= 24; p++) {
    int n = pos.pts[p]; if (!n) continue;
    bool mine = n > 0; int a = abs(n), c; bool top; pointGeom(p, c, top);
    int cx = colX(c) + PW / 2;
    for (int i = 0; i < min(a, 5); i++) checker(cx, stackY(top, i), mine);
    if (a > 5) { cv.setTextColor(mine ? TFT_BLACK : C_ME); number(a, cx, stackY(top, 4) + 1); }
  }
  int bx = BARX + BARW / 2;
  for (int i = 0; i < pos.bar[1]; i++) checker(bx, MIDY - 50 - i * STEP, false);
  for (int i = 0; i < pos.bar[0]; i++) checker(bx, MIDY + 50 + i * STEP, true);
  // cube
  int cy = pos.cubeOwner == 1 ? FB - 30 : pos.cubeOwner == -1 ? FT + 6 : MIDY - 12;
  rect(BARX + 3, cy, 24, 24, 0xEF3A);
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextColor(TFT_BLACK);
  number(pos.cube, bx, cy + 12);
  // borne off
  for (int i = 0; i < pos.off[1]; i++) { rect(TRX + 3, FT + 2 + i * 8, TRW - 6, 7, C_OPRIM); rect(TRX + 4, FT + 3 + i * 8, TRW - 8, 5, C_OP); }
  for (int i = 0; i < pos.off[0]; i++) { rect(TRX + 3, FB - 9 - i * 8, TRW - 6, 7, C_MERIM); rect(TRX + 4, FB - 8 - i * 8, TRW - 8, 5, C_ME); }
  // dice
  if (ndice >= 2) { die(TRX + 2, MIDY - 11, dice[0]); die(TRX + 26, MIDY - 11, dice[1]); }
  else if (ndice == 1) die(TRX + 14, MIDY - 11, dice[0]);
  else { cv.setTextColor(C_TEXT); cv.setFont(&fonts::FreeSansBold9pt7b); text("ROLL", TRX + TRW / 2, MIDY); }
  if (ndice > 2) {
    cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(C_TEXT);
    char b[4]; snprintf(b, sizeof b, "x%d", ndice);
    text(b, TRX + TRW / 2, MIDY + 22);
  }
  // targets
  if (sel > 0) {
    int seen = 0;
    for (int i = 0; i < ndice; i++) {
      int d = dice[i]; if (seen & (1 << d)) continue; seen |= 1 << d;
      int t = dest(sel, d);
      if (t > 0) {
        int c; bool top; pointGeom(t, c, top);
        int n = pos.pts[t] > 0 ? min((int)pos.pts[t], 4) : 0;
        cv.fillSmoothCircle(colX(c) + PW / 2, stackY(top, n) - oy, 6, C_SEL);
      } else if (t == 0) cv.fillSmoothCircle(TRX + TRW / 2, FB - 8 - pos.off[0] * 8 - 10 - oy, 6, C_SEL);
    }
  }
  // status bar
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextDatum(middle_left);
  checker(12, 11, true, 7);
  cv.setTextColor(C_TEXT); number(pips(true), 24, 11);
  checker(80, 11, false, 7);
  number(pips(false), 92, 11);
  cv.setTextDatum(middle_center);
  cv.setTextColor(C_DIM); text("0-ply", W / 2, 11);
  cv.setTextDatum(middle_right);
  cv.setTextColor(!strncmp(pos.eval, "Win 6", 5) ? C_GOOD : !strncmp(pos.eval, "Win 3", 5) ? C_BAD : C_TEXT);
  text(pos.eval, W - 6, 11);
}

static void draw() {
  for (oy = 0; oy < H; oy += BAND) {
    drawBand();
    cv.pushSprite(0, oy);
  }
}

static void dumpFrame() {
  Serial.print("FRAME\n");
  static uint16_t line[W];
  for (oy = 0; oy < H; oy += BAND) {
    drawBand();
    for (int y = 0; y < BAND; y++) {
      for (int x = 0; x < W; x++) line[x] = cv.readPixel(x, y);
      Serial.write((uint8_t*)line, sizeof line);
    }
  }
  Serial.flush();
}

// ---- touch ----
static void calibrate() {
  uint16_t cal[8];
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&fonts::FreeSans12pt7b);
  lcd.setTextDatum(middle_center);
  lcd.drawString("Touch calibration", W / 2, H / 2 - 16);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.drawString("Tap each corner arrow as it appears", W / 2, H / 2 + 14);
  lcd.calibrateTouch(cal, TFT_YELLOW, TFT_BLACK, 24);
  prefs.putBytes("cal", cal, sizeof cal);
  Serial.println("calibrated");
}

static int hitPoint(int x, int y) {
  if (y < FT || y >= FB) return -1;
  int c = -1;
  if (x >= LX && x < LX + 6 * PW) c = (x - LX) / PW;
  else if (x >= RFX && x < RFX + 6 * PW) c = 6 + (x - RFX) / PW;
  if (c < 0) return -1;
  return y < MIDY ? 13 + c : 12 - c;
}
static void tap(int x, int y) {
  if (y < 22) { preset = (preset + 1) % 3; loadPreset(preset); draw(); return; }
  if (x >= TRX && y >= MIDY - 20 && y <= MIDY + 20) {
    if (!ndice) { roll(); endIfStuck(); draw(); }  // dice only roll once the turn is over
    return;
  }
  if (sel > 0) {
    int tp = (x >= TRX && y > MIDY + 20) ? 0 : hitPoint(x, y);
    int seen = 0;
    for (int i = 0; i < ndice; i++) {
      int d = dice[i]; if (seen & (1 << d)) continue; seen |= 1 << d;
      if (tp >= 0 && dest(sel, d) == tp) {
        pos.pts[sel]--;
        if (tp == 0) pos.off[0]++;
        else {
          if (pos.pts[tp] == -1) { pos.pts[tp] = 0; pos.bar[1]++; }
          pos.pts[tp]++;
        }
        useDie(d);
        sel = (pos.pts[sel] > 0 && ndice) ? sel : -1;
        endIfStuck();
        draw();
        return;
      }
    }
  }
  int p = hitPoint(x, y);
  sel = (p > 0 && pos.pts[p] > 0 && ndice && p != sel) ? p : -1;
  draw();
}

void setup() {
  Serial.begin(921600);
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);
  cv.setColorDepth(16);
  if (!cv.createSprite(W, BAND)) Serial.println("sprite alloc failed");
  auto c = [](uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); };
  C_FRAME = c(0x5b, 0x3a, 0x1f); C_FELT = c(0x1e, 0x5a, 0x38);
  C_PTA = c(0xdc, 0xc9, 0xa0);  C_PTB = c(0xa3, 0x39, 0x2b);
  C_ME = c(0xf3, 0xee, 0xe0);   C_MERIM = c(0x8d, 0x86, 0x76); C_MEIN = c(0xd6, 0xcf, 0xbd);
  C_OP = c(0x26, 0x26, 0x26);   C_OPRIM = c(0xa0, 0xa0, 0xa0); C_OPIN = c(0x3c, 0x3c, 0x3c);
  C_BAR = c(0x4a, 0x2f, 0x18);  C_TRAY = c(0x17, 0x3f, 0x28);  C_SEL = c(0xff, 0xd2, 0x3c);
  C_TEXT = c(0xe8, 0xe2, 0xd2); C_DIM = c(0x8a, 0x8a, 0x8a);
  C_GOOD = c(0x7f, 0xe0, 0x8a); C_BAD = c(0xff, 0x8a, 0x7a); C_STATUS = c(0x10, 0x10, 0x10);
  randomSeed(esp_random());
  prefs.begin("bg", false);
  uint16_t cal[8];
  if (prefs.getBytes("cal", cal, sizeof cal) == sizeof cal) lcd.setTouchCalibrate(cal);
  else calibrate();
  loadPreset(preset);
  draw();
}

void loop() {
  int32_t x, y;
  static bool down = false;
  bool now = lcd.getTouch(&x, &y);
  if (now && !down) tap(x, y);
  down = now;
  if (Serial.available()) {
    int ch = Serial.read();
    if (ch == 'n') { preset = (preset + 1) % 3; loadPreset(preset); draw(); }
    else if (ch == 'r') { roll(); draw(); }
    else if (ch == 'd') dumpFrame();
    else if (ch == 'k') { calibrate(); draw(); }
    else if (ch == 't') { int tx = Serial.parseInt(), ty = Serial.parseInt(); tap(tx, ty); Serial.printf("tap %d %d sel=%d\n", tx, ty, sel); }
  }
  delay(10);
}
