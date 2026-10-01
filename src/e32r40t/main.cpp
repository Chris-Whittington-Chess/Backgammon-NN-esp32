// Backgammon vs the Backgammon-NN net (0-ply) on the 4.0" ESP32-32E display
// (lcdwiki E32R40T, sold as Hosyond): ESP32-D0WD-V3, no PSRAM, ST7796S 480x320
// SPI, XPT2046 resistive touch on the same bus. Pins from lcdwiki.
//
// No PSRAM: the frame is rendered in 480x20 bands through one small sprite,
// and the net is read from flash except its dense layer 2 (four 32 KB chunks
// in SRAM). Rules/movegen: common/bg.cpp (port of bgcore moves.rs).
//
// Play: you are white, moving 24 -> 1 (home bottom right). Tap the dice to
// roll, a highlighted point (or the bar) to pick a checker, then a yellow dot
// (or the tray to bear off). Tap the status bar to undo your turn so far.
// Touch calibration runs on first boot (or serial 'k'), saved in NVS.
// Serial (921600): 'v' verify movegen + move choice against bgcore and time
// it, 'n' new game, 't X Y' simulated tap, 'k' recalibrate, 'd' dump frame
// ("FRAME\n" + 480*320 LE RGB565).
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <algorithm>
#include "../common/bg.h"

extern const uint8_t net_bin[] asm("_binary_data_net_bin_start");
extern const uint8_t movetests_bin[] asm("_binary_data_movetests_bin_start");

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
static BgNet net;
static bool netOk;
static const int W = 480, H = 320, BAND = 20;  // 19 KB sprite: the heap is fragmented
static int oy = 0;  // y offset of the band being rendered

// ---- game state (board always from the human's side: + = you) ----
enum Phase { ROLL, MOVE, PASS, OVER };
static BgBoard g, turnStart, cpuBefore;
static Phase phase;
static int dice[2];             // shown dice
static bool cpuDice;            // shown dice are the CPU's
static int rem[4], nrem;        // your dice still to play
static BgSub subs[64];
static int nsubs, sel = -1;     // sel: 1..24, 25 = bar
static bool cpuMarks;           // show the CPU's last move
static int scoreYou, scoreCpu;
static char msg[48], evalTxt[32];
static uint16_t evalCol;
static BgBoard kids[1024];

static uint16_t C_FRAME, C_FELT, C_PTA, C_PTB, C_ME, C_MERIM, C_MEIN, C_OP, C_OPRIM, C_OPIN,
    C_BAR, C_TRAY, C_SEL, C_TEXT, C_DIM, C_GOOD, C_BAD, C_STATUS, C_CPU;

static int pips(const BgBoard& b, int side) { return bg_pip_count(b, side); }

// Destinations for the selected checker, including ones that take several
// dice (e.g. 11 pips with a 6-5): each is a chain of legal sub-moves by that
// same checker. Fewest steps wins; on a tie the first chain found is kept.
struct Path { int8_t to, n; BgSub step[4]; };
static Path paths[24];
static int npaths;

static void reachFrom(const BgBoard& b, const int* dr, int dn, int at, Path& cur) {
  static BgSub lvl[4][64];
  BgSub* s = lvl[cur.n];
  int k = bg_next_submoves(b, dr, dn, s, 64);
  for (int i = 0; i < k; i++) {
    if (s[i].from != at) continue;
    Path p = cur;
    p.step[p.n++] = s[i];
    p.to = s[i].to;
    int j = 0;
    while (j < npaths && paths[j].to != p.to) j++;
    if (j == npaths && npaths < 24) paths[npaths++] = p;
    else if (j < npaths && p.n < paths[j].n) paths[j] = p;
    if (p.to == 0) continue;  // borne off: this checker is done
    int r2[4], n2 = 0;
    bool dropped = false;
    for (int d = 0; d < dn; d++) {
      if (!dropped && dr[d] == s[i].die) { dropped = true; continue; }
      r2[n2++] = dr[d];
    }
    if (n2) reachFrom(s[i].result, r2, n2, p.to, p);
  }
}

static void computePaths() {
  npaths = 0;
  if (sel < 0) return;
  Path start; start.n = 0; start.to = sel;
  reachFrom(g, rem, nrem, sel, start);
}

// ---- geometry (480x320) ----
static const int PW = 32;                  // point width
static const int LX = 6, RFX = 228;        // left / right field x
static const int BARX = 198, BARW = 30;
static const int TRX = 424, TRW = 50;      // tray
static const int FT = 25, FB = 316;        // field top / bottom
static const int PH = 128;                 // point (triangle) height
static const int CR = 13, STEP = 27;       // checker radius, stack step
static const int MIDY = 171;               // dice row

static int colX(int c) { return c < 6 ? LX + c * PW : RFX + (c - 6) * PW; }
static void pointGeom(int p, int& c, bool& top) {
  if (p >= 13) { c = p - 13; top = true; } else { c = 12 - p; top = false; }
}
static int stackY(bool top, int i) { return top ? FT + 14 + i * STEP : FB - 14 - i * STEP; }
static int pointCX(int p) { int c; bool t; pointGeom(p, c, t); return colX(c) + PW / 2; }

// ---- drawing (screen coordinates; shifted by the band offset) ----
static void checker(int cx, int cy, bool mine, int r = CR) {
  cy -= oy;
  if (cy < -r || cy > BAND + r) return;
  cv.fillSmoothCircle(cx, cy, r, mine ? C_MERIM : C_OPRIM);
  cv.fillSmoothCircle(cx, cy, r - 1, mine ? C_ME : C_OP);
  cv.drawCircle(cx, cy, r - 5, mine ? C_MEIN : C_OPIN);
}
static void rect(int x, int y, int w, int h, uint16_t c) { cv.fillRect(x, y - oy, w, h, c); }
static void dot(int x, int y, int r, uint16_t c) { cv.fillSmoothCircle(x, y - oy, r, c); }
static void tri(int x0, bool top, uint16_t col) {
  if (top) cv.fillTriangle(x0, FT - oy, x0 + PW - 1, FT - oy, x0 + PW / 2, FT + PH - oy, col);
  else     cv.fillTriangle(x0, FB - 1 - oy, x0 + PW - 1, FB - 1 - oy, x0 + PW / 2, FB - PH - oy, col);
}
static void die(int x, int y, int v, bool cpu, bool used) {
  y -= oy;
  uint16_t body = cpu ? C_OP : 0xF79D, pip = cpu ? C_ME : TFT_BLACK;
  if (used) { body = C_DIM; pip = 0x4208; }
  cv.fillRoundRect(x, y, 22, 22, 4, body);
  if (cpu) cv.drawRoundRect(x, y, 22, 22, 4, C_OPRIM);
  static const uint8_t P[7][6][2] = {{}, {{11,11}}, {{6,6},{16,16}}, {{6,6},{11,11},{16,16}},
    {{6,6},{16,6},{6,16},{16,16}}, {{6,6},{16,6},{11,11},{6,16},{16,16}},
    {{6,6},{16,6},{6,11},{16,11},{6,16},{16,16}}};
  for (int i = 0; i < v; i++) cv.fillSmoothCircle(x + P[v][i][0], y + P[v][i][1], 2, pip);
}
static void text(const char* s, int x, int y) { cv.drawString(s, x, y - oy); }
static void number(int n, int x, int y) { cv.drawNumber(n, x, y - oy); }

static bool canMoveFrom(int p) {
  for (int i = 0; i < nsubs; i++) if (subs[i].from == p) return true;
  return false;
}

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
  // Movable checkers: a yellow bar at the point's base (or the bar).
  if (phase == MOVE && sel < 0) {
    for (int p = 1; p <= 24; p++) if (canMoveFrom(p)) {
      int c; bool top; pointGeom(p, c, top);
      rect(colX(c) + 4, top ? FT : FB - 4, PW - 8, 4, C_SEL);
    }
  }
  if (sel > 0 && sel <= 24) {
    int c; bool top; pointGeom(sel, c, top);
    cv.drawRect(colX(c), (top ? FT : FB - 5 * STEP - 1) - oy, PW, 5 * STEP + 1, C_SEL);
  }
  // checkers
  cv.setFont(&fonts::FreeSansBold12pt7b);
  cv.setTextDatum(middle_center);
  for (int p = 1; p <= 24; p++) {
    int n = g.pts[p]; if (!n) continue;
    bool mine = n > 0; int a = abs(n), c; bool top; pointGeom(p, c, top);
    int cx = colX(c) + PW / 2;
    for (int i = 0; i < min(a, 5); i++) checker(cx, stackY(top, i), mine);
    if (a > 5) { cv.setTextColor(mine ? TFT_BLACK : C_ME); number(a, cx, stackY(top, 4) + 1); }
  }
  int bx = BARX + BARW / 2;
  for (int i = 0; i < g.bar[1]; i++) checker(bx, MIDY - 40 - i * STEP, false);
  for (int i = 0; i < g.bar[0]; i++) checker(bx, MIDY + 40 + i * STEP, true);
  if (sel == 25) cv.drawRect(BARX, MIDY + 26 - oy, BARW, g.bar[0] * STEP + 2, C_SEL);
  else if (phase == MOVE && sel < 0 && canMoveFrom(25)) rect(BARX + 4, MIDY + 22, BARW - 8, 4, C_SEL);
  // The CPU's last move: orange marks where its checkers left and landed.
  if (cpuMarks) {
    for (int p = 1; p <= 24; p++) {
      int was = cpuBefore.pts[p] < 0 ? -cpuBefore.pts[p] : 0, now = g.pts[p] < 0 ? -g.pts[p] : 0;
      if (was == now) continue;
      int c; bool top; pointGeom(p, c, top);
      int y = top ? FT + 5 * STEP + 6 : FB - 5 * STEP - 6;
      if (now > was) dot(colX(c) + PW / 2, y, 4, C_CPU);
      else cv.drawCircle(colX(c) + PW / 2, y - oy, 4, C_CPU);
    }
  }
  // borne off
  for (int i = 0; i < g.off[1]; i++) { rect(TRX + 3, FT + 2 + i * 8, TRW - 6, 7, C_OPRIM); rect(TRX + 4, FT + 3 + i * 8, TRW - 8, 5, C_OP); }
  for (int i = 0; i < g.off[0]; i++) { rect(TRX + 3, FB - 9 - i * 8, TRW - 6, 7, C_MERIM); rect(TRX + 4, FB - 8 - i * 8, TRW - 8, 5, C_ME); }
  // dice
  if (dice[0]) {
    bool dbl = dice[0] == dice[1];
    bool used0 = false, used1 = false;
    if (phase == MOVE && !cpuDice && !dbl) {
      used0 = true; used1 = true;
      for (int i = 0; i < nrem; i++) { if (rem[i] == dice[0]) used0 = false; if (rem[i] == dice[1]) used1 = false; }
    }
    die(TRX + 2, MIDY - 11, dice[0], cpuDice, used0);
    die(TRX + 26, MIDY - 11, dice[1], cpuDice, used1);
    if (dbl && phase == MOVE && !cpuDice) {
      cv.setFont(&fonts::FreeSans9pt7b); cv.setTextColor(C_TEXT); cv.setTextDatum(middle_center);
      char b[4]; snprintf(b, sizeof b, "x%d", nrem);
      text(b, TRX + TRW / 2, MIDY + 22);
    }
  }
  if (phase == ROLL) {
    cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextColor(C_SEL); cv.setTextDatum(middle_center);
    text("ROLL", TRX + TRW / 2, dice[0] ? MIDY + 24 : MIDY);
  }
  // targets for the selected checker
  // Filled dot: one die. Ring: several dice with the same checker.
  if (sel > 0) {
    for (int i = 0; i < npaths; i++) {
      int t = paths[i].to, x, y;
      if (t > 0) {
        int c; bool top; pointGeom(t, c, top);
        int n = g.pts[t] > 0 ? min((int)g.pts[t], 4) : 0;
        x = colX(c) + PW / 2; y = stackY(top, n);
      } else { x = TRX + TRW / 2; y = FB - 8 - g.off[0] * 8 - 10; }
      if (paths[i].n == 1) dot(x, y, 6, C_SEL);
      else { dot(x, y, 7, C_SEL); dot(x, y, 4, C_FELT); }
    }
  }
  // status bar: pips | message | eval
  cv.setFont(&fonts::FreeSans9pt7b);
  cv.setTextDatum(middle_left);
  checker(12, 11, true, 7);
  cv.setTextColor(C_TEXT); number(pips(g, 0), 24, 11);
  checker(72, 11, false, 7);
  number(pips(g, 1), 84, 11);
  cv.setTextDatum(middle_center);
  cv.setTextColor(C_TEXT); text(msg, 236, 11);
  cv.setTextDatum(middle_right);
  cv.setTextColor(evalCol); text(evalTxt, W - 6, 11);
}

static void draw() {
  for (oy = 0; oy < H; oy += BAND) {
    drawBand();
    cv.pushSprite(0, oy);
  }
}

// ---- game flow ----
static void setEval() {
  // Your chances with you on roll, from the net.
  float p[6];
  net.eval(g, p);
  float win = p[0] + p[1] + p[2], eq = bg_equity(p);
  snprintf(evalTxt, sizeof evalTxt, "%.1f%%  %+.2f", win * 100, eq);
  evalCol = eq > 0.1f ? C_GOOD : eq < -0.1f ? C_BAD : C_TEXT;
}

static void gameOver(int pts, bool youWon) {
  (youWon ? scoreYou : scoreCpu) += pts;
  const char* kind = pts == 3 ? "backgammon" : pts == 2 ? "gammon" : "single";
  snprintf(msg, sizeof msg, "%s %d (%s)", youWon ? "You win" : "CPU wins", pts, kind);
  snprintf(evalTxt, sizeof evalTxt, "You %d - CPU %d", scoreYou, scoreCpu);
  evalCol = C_TEXT;
  phase = OVER;
  sel = -1; nsubs = 0;
}

static void cpuTurn(int d1, int d2) {
  dice[0] = d1; dice[1] = d2; cpuDice = true;
  snprintf(msg, sizeof msg, "CPU rolls %d-%d...", d1, d2);
  nsubs = 0; sel = -1; cpuMarks = false;
  draw();
  uint32_t t0 = millis();
  BgBoard me = bg_swap(g);
  int n = bg_genmoves(me, d1, d2, kids, 1024);
  int best = bg_best(net, kids, n);
  cpuBefore = g;
  g = bg_swap(kids[best]);
  cpuMarks = true;
  uint32_t ms = millis() - t0;
  Serial.printf("cpu %d-%d: %d moves, %u ms\n", d1, d2, n, ms);
  if (int r = bg_result(kids[best])) { gameOver(r, false); return; }
  snprintf(msg, sizeof msg, n == 1 && !memcmp(&kids[0], &me, sizeof me) ? "CPU can't move" : "CPU played %d-%d", d1, d2);
  phase = ROLL;
  setEval();
}

static void startMove(int d1, int d2) {
  dice[0] = d1; dice[1] = d2; cpuDice = false;
  nrem = 0;
  rem[nrem++] = d1; rem[nrem++] = d2;
  if (d1 == d2) { rem[nrem++] = d1; rem[nrem++] = d1; }
  turnStart = g;
  sel = -1; npaths = 0;
  nsubs = bg_next_submoves(g, rem, nrem, subs, 64);
  if (!nsubs) { phase = PASS; snprintf(msg, sizeof msg, "No legal move - tap to pass"); }
  else { phase = MOVE; snprintf(msg, sizeof msg, "Your move: %d-%d", d1, d2); }
}

static void newGame() {
  g = bg_start();
  cpuMarks = false;
  int a, b;
  do { a = random(1, 7); b = random(1, 7); } while (a == b);
  dice[0] = dice[1] = 0;
  if (a > b) {
    snprintf(msg, sizeof msg, "You %d, CPU %d: you start", a, b);
    setEval();
    startMove(a, b);
    snprintf(msg, sizeof msg, "You start: play %d-%d", a, b);
  } else {
    setEval();
    cpuTurn(b, a);
  }
}

static void endHumanTurn() {
  nsubs = 0; sel = -1;
  draw();
  delay(250);
  cpuTurn(random(1, 7), random(1, 7));
}

static void applyPath(const Path& p) {
  for (int k = 0; k < p.n; k++) {
    g = p.step[k].result;
    for (int i = 0; i < nrem; i++) if (rem[i] == p.step[k].die) { rem[i] = rem[--nrem]; break; }
  }
  if (int r = bg_result(g)) { gameOver(r, true); return; }
  sel = -1; npaths = 0;
  nsubs = bg_next_submoves(g, rem, nrem, subs, 64);
  if (!nsubs) endHumanTurn();
  else snprintf(msg, sizeof msg, "Tap this bar to undo");
}

// Why a checker on point p (or the bar, 25) can't move right now.
static const char* whyNot(int p) {
  if (p != 25 && g.bar[0]) return "Enter from the bar first";
  bool home = true;
  for (int q = 7; q <= 24; q++) if (g.pts[q] > 0) home = false;
  for (int i = 0; i < nrem; i++) {
    int t = p - rem[i];
    if (p != 25 && t < 1 && !home) return "Bring all checkers home first";
    if (t >= 1 && g.pts[t] > -2) return "Must play both dice";  // legal alone, but not maximal
    if (p == 25 && g.pts[25 - rem[i]] > -2) return "Must play the higher die";
  }
  return "That checker is blocked";
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

// Board location under a tap: 1..24 point, 25 bar, 0 tray (bear off), -1 none.
static int hitSpot(int x, int y) {
  if (y < FT || y >= FB) return -1;
  if (x >= BARX && x < BARX + BARW) return 25;
  if (x >= TRX) return 0;
  int c = -1;
  if (x >= LX && x < LX + 6 * PW) c = (x - LX) / PW;
  else if (x >= RFX && x < RFX + 6 * PW) c = 6 + (x - RFX) / PW;
  if (c < 0) return -1;
  return y < MIDY ? 13 + c : 12 - c;
}

static void tap(int x, int y) {
  if (phase == OVER) { newGame(); draw(); return; }
  if (phase == PASS) { endHumanTurn(); draw(); return; }
  if (phase == ROLL) {
    if (x >= TRX && y >= MIDY - 30 && y <= MIDY + 40) {
      cpuMarks = false;
      startMove(random(1, 7), random(1, 7));
      if (phase == MOVE) snprintf(msg, sizeof msg, "Your move: %d-%d", dice[0], dice[1]);
      draw();
    }
    return;
  }
  // MOVE
  if (y < 22) {  // undo the turn so far
    if (memcmp(&g, &turnStart, sizeof g)) { startMove(dice[0], dice[1]); draw(); }
    return;
  }
  int s = hitSpot(x, y);
  Serial.printf("tap %d,%d -> spot %d (sel %d)\n", x, y, s, sel);
  if (sel > 0) {
    for (int i = 0; i < npaths; i++)
      if (paths[i].to == s) { applyPath(paths[i]); draw(); return; }
  }
  if (s > 0 && s != sel && canMoveFrom(s)) {
    sel = s;
    computePaths();
    if (npaths == 1) { applyPath(paths[0]); draw(); return; }  // only one place to go
    snprintf(msg, sizeof msg, "Your move: %d-%d", dice[0], dice[1]);
  } else {
    bool mine = s == 25 ? g.bar[0] > 0 : s > 0 && g.pts[s] > 0;
    if (mine && s != sel) snprintf(msg, sizeof msg, "%s", whyNot(s));
    else if (sel > 0 && s >= 0) snprintf(msg, sizeof msg, "Can't move there");
    sel = -1; npaths = 0;
  }
  draw();
}

// ---- verification against bgcore (serial 'v') ----
static uint32_t fmix(uint32_t h) {
  h ^= h >> 16; h *= 0x85EBCA6B; h ^= h >> 13; h *= 0xC2B2AE35; return h ^ (h >> 16);
}

static void verify() {
  uint32_t n;
  memcpy(&n, movetests_bin, 4);
  int badCount = 0, badSet = 0, badBest = 0, ties = 0;
  uint32_t genUs = 0, genMax = 0, bestUs = 0, bestMax = 0, evals = 0;
  for (uint32_t t = 0; t < n; t++) {
    const uint8_t* r = movetests_bin + 4 + t * 44;
    BgBoard b; memset(&b, 0, sizeof b);
    for (int p = 0; p < 24; p++) b.pts[p + 1] = (int8_t)r[p];
    b.bar[0] = r[24]; b.bar[1] = r[25]; b.off[0] = r[26]; b.off[1] = r[27];
    int d1 = r[28], d2 = r[29];
    uint16_t cnt; uint32_t setH, bestH; float bestS;
    memcpy(&cnt, r + 30, 2); memcpy(&setH, r + 32, 4); memcpy(&bestH, r + 36, 4); memcpy(&bestS, r + 40, 4);
    uint32_t t0 = micros();
    int k = bg_genmoves(b, d1, d2, kids, 1024);
    uint32_t t1 = micros();
    float s;
    int best = bg_best(net, kids, k, &s);
    uint32_t t2 = micros();
    genUs += t1 - t0; genMax = max(genMax, t1 - t0);
    bestUs += t2 - t1; bestMax = max(bestMax, t2 - t1); evals += k;
    uint32_t h = 0;
    for (int i = 0; i < k; i++) h += fmix(bg_hash(kids[i]));
    if (k != cnt) badCount++;
    if (h != setH) badSet++;
    if (bg_hash(kids[best]) != bestH) {
      if (fabsf(s - bestS) < 1e-4f) ties++;
      else { badBest++; Serial.printf("  test %u: best differs, dev %.5f pc %.5f\n", t, s, bestS); }
    }
  }
  Serial.printf("verify %u tests: count miss %d, set miss %d, choice miss %d (ties %d)\n",
                n, badCount, badSet, badBest, ties);
  Serial.printf("movegen avg %.2f ms max %.1f ms; 0-ply choice avg %.1f ms max %.0f ms; %.0f us/eval\n",
                genUs / 1000.0 / n, genMax / 1000.0, bestUs / 1000.0 / n, bestMax / 1000.0,
                double(bestUs) / evals);
  Serial.println("DONE");
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

void setup() {
  Serial.begin(921600);
  netOk = net.load(net_bin, NET_FLASH_SRAM);  // grab the 32 KB chunks first
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);
  cv.setColorDepth(16);
  if (!cv.createSprite(W, BAND)) Serial.println("sprite alloc failed");
  Serial.printf("net %s, free heap %u, largest %u\n", netOk ? "ok" : "LOAD FAILED",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  auto c = [](uint8_t r, uint8_t g, uint8_t b) { return lcd.color565(r, g, b); };
  C_FRAME = c(0x5b, 0x3a, 0x1f); C_FELT = c(0x1e, 0x5a, 0x38);
  C_PTA = c(0xdc, 0xc9, 0xa0);  C_PTB = c(0xa3, 0x39, 0x2b);
  C_ME = c(0xf3, 0xee, 0xe0);   C_MERIM = c(0x8d, 0x86, 0x76); C_MEIN = c(0xd6, 0xcf, 0xbd);
  C_OP = c(0x26, 0x26, 0x26);   C_OPRIM = c(0xa0, 0xa0, 0xa0); C_OPIN = c(0x3c, 0x3c, 0x3c);
  C_BAR = c(0x4a, 0x2f, 0x18);  C_TRAY = c(0x17, 0x3f, 0x28);  C_SEL = c(0xff, 0xd2, 0x3c);
  C_TEXT = c(0xe8, 0xe2, 0xd2); C_DIM = c(0x8a, 0x8a, 0x8a);
  C_GOOD = c(0x7f, 0xe0, 0x8a); C_BAD = c(0xff, 0x8a, 0x7a); C_STATUS = c(0x10, 0x10, 0x10);
  C_CPU = c(0xff, 0x9a, 0x3c);
  randomSeed(esp_random());
  prefs.begin("bg", false);
  uint16_t cal[8];
  if (prefs.getBytes("cal", cal, sizeof cal) == sizeof cal) lcd.setTouchCalibrate(cal);
  else calibrate();
  if (!netOk) {
    lcd.fillScreen(TFT_RED);
    lcd.drawString("Net failed to load", W / 2, H / 2);
    return;
  }
  newGame();
  draw();
}

void loop() {
  // Resistive touch: the first samples of a press are unreliable, so collect
  // the whole press and act on release at the median position.
  static int16_t sx[64], sy[64];
  static int ns = 0, idle = 0;
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    if (ns < 64) { sx[ns] = x; sy[ns] = y; ns++; }
    idle = 0;
  } else if (ns && ++idle >= 3) {  // ~30 ms without contact = released
    int skip = ns > 4 ? 2 : 0, m = ns - skip;
    std::sort(sx + skip, sx + ns);
    std::sort(sy + skip, sy + ns);
    if (netOk) tap(sx[skip + m / 2], sy[skip + m / 2]);
    ns = 0;
  }
  if (Serial.available()) {
    int ch = Serial.read();
    if (!netOk && ch != 'd' && ch != 'k') ch = 0;  // nothing that evaluates without a net
    if (ch == 'v') verify();
    else if (ch == 'n') { newGame(); draw(); }
    else if (ch == 'd') dumpFrame();
    else if (ch == 'k') { calibrate(); draw(); }
    else if (ch == 't') { int tx = Serial.parseInt(), ty = Serial.parseInt(); tap(tx, ty); Serial.printf("tap %d %d phase=%d sel=%d\n", tx, ty, phase, sel); }
  }
  delay(10);
}
