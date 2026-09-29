// Backgammon board on the CoreS3's 320x240 display.
// Touch: tap a point with your checkers to select it (targets shown as dots),
// tap a dot (or the tray for bear-off) to move, tap the dice to roll,
// tap the status bar to cycle preset positions.
// Serial: 'n' next preset, 'r' roll, 't X Y' simulated tap, 'd' dump frame
// (prints "FRAME\n" then 320*240 little-endian RGB565 pixels).
#include <M5Unified.h>

static M5Canvas cv(&M5.Display);

// Points 1..24 from the bottom player's view; >0 mine (light), <0 CPU (dark).
struct Pos {
  int pts[25];
  int bar[2];   // [me, cpu]
  int off[2];
  int cube, cubeOwner;  // owner: 0 centre, 1 me, -1 cpu
  const char* eval;
};
static Pos pos;
static int dice[4], ndice = 0;
static int sel = -1;  // selected point or -1
static int preset = 1;

static uint16_t C_FRAME, C_FELT, C_PTA, C_PTB, C_ME, C_MERIM, C_MEIN, C_OP, C_OPRIM, C_OPIN,
    C_BAR, C_TRAY, C_SEL, C_TEXT, C_DIM, C_GOOD, C_BAD;

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
    pos.eval = "Win 61.4% +0.27";
  } else {
    set(1, 3); set(2, 3); set(3, 2); set(4, 2); set(5, 1);
    set(24, -3); set(23, -2); set(21, -2); set(19, -1);
    pos.off[0] = 4; pos.off[1] = 7;
    dice[0] = 5; dice[1] = 2; ndice = 2;
    pos.eval = "Win 38.9% -0.22";
  }
}

// ---- geometry ----
static int colX(int c) { return c < 6 ? 4 + c * 21 : 150 + (c - 6) * 21; }
static void pointGeom(int p, int& c, bool& top) {
  if (p >= 13) { c = p - 13; top = true; } else { c = 12 - p; top = false; }
}
static int stackY(bool top, int i) { return top ? 29 + i * 19 : 227 - i * 19; }

// ---- rules (just enough for a touch test) ----
static bool allHome() {
  if (pos.bar[0]) return false;
  for (int p = 7; p <= 24; p++) if (pos.pts[p] > 0) return false;
  return true;
}
// Destination for moving from p with die d: 1..24, 0 = bear off, -1 = illegal.
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
static int pips(bool me) {
  int s = 0;
  for (int p = 1; p <= 24; p++) {
    if (me && pos.pts[p] > 0) s += pos.pts[p] * p;
    if (!me && pos.pts[p] < 0) s += -pos.pts[p] * (25 - p);
  }
  return s + pos.bar[me ? 0 : 1] * 25;
}

// ---- drawing ----
static void checker(int cx, int cy, bool mine, int r = 9) {
  cv.fillSmoothCircle(cx, cy, r, mine ? C_MERIM : C_OPRIM);
  cv.fillSmoothCircle(cx, cy, r - 1, mine ? C_ME : C_OP);
  cv.drawCircle(cx, cy, r - 4, mine ? C_MEIN : C_OPIN);
}
static void tri(int x0, bool top, uint16_t col) {
  if (top) cv.fillTriangle(x0, 19, x0 + 20, 19, x0 + 10, 113, col);
  else     cv.fillTriangle(x0, 236, x0 + 20, 236, x0 + 10, 143, col);
}
static void die(int x, int y, int v) {
  cv.fillRoundRect(x, y, 17, 17, 3, 0xF79D);
  static const uint8_t P[7][6][2] = {{}, {{8,8}}, {{4,4},{12,12}}, {{4,4},{8,8},{12,12}},
    {{4,4},{12,4},{4,12},{12,12}}, {{4,4},{12,4},{8,8},{4,12},{12,12}},
    {{4,4},{12,4},{4,8},{12,8},{4,12},{12,12}}};
  for (int i = 0; i < v; i++) cv.fillRect(x + P[v][i][0] - 1, y + P[v][i][1] - 1, 3, 3, TFT_BLACK);
}
static void dot(int cx, int cy) { cv.fillSmoothCircle(cx, cy, 4, C_SEL); }

static void draw() {
  cv.fillRect(0, 0, 320, 16, 0x0841);
  cv.fillRect(0, 16, 320, 224, C_FRAME);
  cv.fillRect(4, 19, 126, 218, C_FELT);
  cv.fillRect(150, 19, 126, 218, C_FELT);
  cv.fillRect(130, 16, 20, 224, C_BAR);
  cv.fillRect(279, 19, 37, 218, C_TRAY);
  for (int c = 0; c < 12; c++) {
    tri(colX(c), true, c % 2 ? C_PTB : C_PTA);
    tri(colX(c), false, c % 2 ? C_PTA : C_PTB);
  }
  if (sel > 0) {
    int c; bool top; pointGeom(sel, c, top);
    cv.drawRect(colX(c), top ? 19 : 137, 21, 100, C_SEL);
  }
  cv.setFont(&fonts::FreeSansBold9pt7b);
  cv.setTextDatum(middle_center);
  for (int p = 1; p <= 24; p++) {
    int n = pos.pts[p]; if (!n) continue;
    bool mine = n > 0; int a = abs(n), c; bool top; pointGeom(p, c, top);
    int cx = colX(c) + 10;
    for (int i = 0; i < min(a, 5); i++) checker(cx, stackY(top, i), mine);
    if (a > 5) {
      cv.setTextColor(mine ? TFT_BLACK : C_ME);
      cv.drawNumber(a, cx, stackY(top, 4) + 1);
    }
  }
  for (int i = 0; i < pos.bar[1]; i++) checker(140, 58 + i * 19, false);
  for (int i = 0; i < pos.bar[0]; i++) checker(140, 198 - i * 19, true);
  // cube
  int cy = pos.cubeOwner == 1 ? 200 : pos.cubeOwner == -1 ? 24 : 120;
  cv.fillRect(132, cy, 16, 16, 0xEF3A);
  cv.setFont(&fonts::Font0);
  cv.setTextColor(TFT_BLACK);
  cv.drawNumber(pos.cube, 140, cy + 8);
  // borne off
  for (int i = 0; i < pos.off[1]; i++) { cv.fillRect(281, 21 + i * 5, 33, 4, C_OPRIM); cv.fillRect(282, 22 + i * 5, 31, 2, C_OP); }
  for (int i = 0; i < pos.off[0]; i++) { cv.fillRect(281, 231 - i * 5, 33, 4, C_MERIM); cv.fillRect(282, 232 - i * 5, 31, 2, C_ME); }
  // dice
  if (ndice >= 2) { die(279, 119, dice[0]); die(298, 119, dice[1]); }
  else if (ndice == 1) die(288, 119, dice[0]);
  else { cv.setTextColor(C_TEXT); cv.setFont(&fonts::Font0); cv.drawString("ROLL", 297, 128); }
  if (ndice > 2) {  // doubles: remaining-uses count
    cv.setFont(&fonts::Font0); cv.setTextColor(C_TEXT);
    cv.drawString(String("x") + ndice, 297, 144);
  }
  // targets
  if (sel > 0) {
    int seen = 0;
    for (int i = 0; i < ndice; i++) {
      int d = dice[i]; if (seen & (1 << d)) continue; seen |= 1 << d;
      int t = dest(sel, d);
      if (t > 0) {
        int c; bool top; pointGeom(t, c, top);
        int n = pos.pts[t] > 0 ? min(pos.pts[t], 4) : 0;
        dot(colX(c) + 10, stackY(top, n));
      } else if (t == 0) dot(297, 231 - pos.off[0] * 5 - 6);
    }
  }
  // status bar
  cv.setFont(&fonts::DejaVu12);
  cv.setTextDatum(middle_left);
  checker(8, 8, true, 5);
  cv.setTextColor(C_TEXT); cv.drawNumber(pips(true), 17, 8);
  checker(56, 8, false, 5);
  cv.drawNumber(pips(false), 65, 8);
  cv.setTextDatum(middle_center);
  cv.setTextColor(C_DIM); cv.drawString("0-ply", 160, 8);
  cv.setTextDatum(middle_right);
  cv.setTextColor(!strncmp(pos.eval, "Win 6", 5) ? C_GOOD : !strncmp(pos.eval, "Win 3", 5) ? C_BAD : C_TEXT);
  cv.drawString(pos.eval, 316, 8);
  cv.pushSprite(0, 0);
}

// ---- input ----
static int hitPoint(int x, int y) {
  if (y < 19 || y > 236) return -1;
  int c = -1;
  if (x >= 4 && x < 130) c = (x - 4) / 21;
  else if (x >= 150 && x < 276) c = 6 + (x - 150) / 21;
  if (c < 0) return -1;
  return y < 128 ? 13 + c : 12 - c;
}
static void tap(int x, int y) {
  if (y < 16) { preset = (preset + 1) % 3; loadPreset(preset); draw(); return; }
  if (x >= 279 && y >= 112 && y <= 144) { roll(); draw(); return; }
  if (sel > 0) {
    int tp = (x >= 279 && y > 144) ? 0 : hitPoint(x, y);
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
        draw();
        return;
      }
    }
  }
  int p = hitPoint(x, y);
  sel = (p > 0 && pos.pts[p] > 0 && ndice && p != sel) ? p : -1;
  draw();
}

static void dumpFrame() {
  Serial.print("FRAME\n");
  static uint16_t line[320];
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      uint16_t v = cv.readPixel(x, y);  // RGB565
      line[x] = v;
    }
    Serial.write((uint8_t*)line, sizeof line);
  }
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  cv.setPsram(true);
  cv.setColorDepth(16);
  cv.createSprite(320, 240);
  auto c = [](uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); };
  C_FRAME = c(0x5b, 0x3a, 0x1f); C_FELT = c(0x1e, 0x5a, 0x38);
  C_PTA = c(0xdc, 0xc9, 0xa0);  C_PTB = c(0xa3, 0x39, 0x2b);
  C_ME = c(0xf3, 0xee, 0xe0);   C_MERIM = c(0x8d, 0x86, 0x76); C_MEIN = c(0xd6, 0xcf, 0xbd);
  C_OP = c(0x26, 0x26, 0x26);   C_OPRIM = c(0xa0, 0xa0, 0xa0); C_OPIN = c(0x3c, 0x3c, 0x3c);
  C_BAR = c(0x4a, 0x2f, 0x18);  C_TRAY = c(0x17, 0x3f, 0x28);  C_SEL = c(0xff, 0xd2, 0x3c);
  C_TEXT = c(0xe8, 0xe2, 0xd2); C_DIM = c(0x8a, 0x8a, 0x8a);
  C_GOOD = c(0x7f, 0xe0, 0x8a); C_BAD = c(0xff, 0x8a, 0x7a);
  randomSeed(esp_random());
  loadPreset(preset);
  draw();
}

void loop() {
  M5.update();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) tap(t.x, t.y);
  if (Serial.available()) {
    int ch = Serial.read();
    if (ch == 'n') { preset = (preset + 1) % 3; loadPreset(preset); draw(); }
    else if (ch == 'r') { roll(); draw(); }
    else if (ch == 'd') dumpFrame();
    else if (ch == 't') { int x = Serial.parseInt(), y = Serial.parseInt(); tap(x, y); Serial.printf("tap %d %d sel=%d\n", x, y, sel); }
  }
  delay(5);
}
