// Backgammon vs the Backgammon-NN net (0-ply) on an ESP32 touch-screen board.
// The board (panel, touch, pins, layout size) comes from include/board.h; the
// layout below is computed from its W x H. Reference board: the 4.0" ESP32-32E
// display (480x320, no PSRAM).
//
// No PSRAM: the frame is rendered in W x 20 bands through one small sprite,
// and the net is read from flash except its dense layer 2 (four 32 KB chunks
// in SRAM). Rules/movegen: common/bg.cpp (port of bgcore moves.rs).
//
// Play: you are white, moving 24 -> 1 (home bottom right). Tap the dice to
// roll, a highlighted point (or the bar) to pick a checker, then a yellow dot
// or ring (or the tray to bear off). The menu (top left) has new game, take
// back, hint, reset score and touch calibration.
// Touch calibration runs on first boot (or serial 'k'), saved in NVS.
// Serial (921600): 'v' verify movegen + move choice against bgcore and time
// it, 'n' new game, 't X Y' simulated tap, 'k' recalibrate, 'd' dump frame
// ("FRAME\n" + W*H LE RGB565).
#include "board.h"
#include <Preferences.h>
#include <algorithm>
#include "../common/bg.h"

extern const uint8_t net_bin[] asm("_binary_data_net_bin_start");
extern const uint8_t movetests_bin[] asm("_binary_data_movetests_bin_start");

// Anti-aliased DejaVu Sans (tools/make_fonts.py), read from flash, at the
// board's two sizes: F_S / F_B small regular / bold, F_L / F_LB large.
#define STR2(x) #x
#define STR(x) STR2(x)
#define CAT3(a, b, c) a##b##c
#define FONT(var, kind, px)                                                                   \
  extern const uint8_t var##_s[] asm(STR(CAT3(_binary_data_fonts_, kind, px)) "_vlw_start"); \
  extern const uint8_t var##_e[] asm(STR(CAT3(_binary_data_fonts_, kind, px)) "_vlw_end");
#define FONT_X(var, kind, px) FONT(var, kind, px)
FONT_X(fs, sans, FONT_PX_S) FONT_X(fb, bold, FONT_PX_S) FONT_X(fl, sans, FONT_PX_L) FONT_X(flb, bold, FONT_PX_L)
static lgfx::VLWfont F_S16, F_B16, F_S21, F_B21;  // small / small bold / large / large bold
static lgfx::PointerWrapper fontData[4];
static void loadFonts() {
  const uint8_t* s[4] = {fs_s, fb_s, fl_s, flb_s};
  const uint8_t* e[4] = {fs_e, fb_e, fl_e, flb_e};
  lgfx::VLWfont* f[4] = {&F_S16, &F_B16, &F_S21, &F_B21};
  for (int i = 0; i < 4; i++) {
    fontData[i].set(s[i], e[i] - s[i]);
    if (!f[i]->loadFont(&fontData[i])) Serial.printf("font %d failed\n", i);
  }
}

static LGFX_Sprite cv;  // one band of the frame; the board's display_push() puts it on screen
static Preferences prefs;
static BgNet net;
static bool netOk;
static const int BAND = 20;  // W x 20 x 2 bytes of sprite (19 KB at 480): the heap is fragmented
static int oy = 0;  // y offset of the band being rendered
static int clipX0 = 0, clipX1 = W;  // columns being redrawn: skip shapes outside them

// ---- game state (board always from the human's side: + = you) ----
// DONE: all your dice played, waiting for you to tap the dice to hand over.
// OFFER: the CPU has doubled you. RESIGN: you're choosing how much to resign.
// RESOFFER: the CPU has offered to resign.
// COUNTER: the CPU refused your resignation / claim and offered another deal.
enum Phase { ROLL, MOVE, PASS, OVER, OFFER, DONE, RESIGN, RESOFFER, COUNTER };
static BgBoard g, turnStart, cpuBefore;
static Phase phase;
static int dice[2];             // shown dice
static bool cpuDice;            // shown dice are the CPU's
static int rem[4], nrem;        // your dice still to play
// Each checker move this turn (one tap = one step, even if it used two dice),
// so Undo step can go back one at a time.
struct Step { BgBoard b; int rem[4], nrem; };
static Step steps[8];
static int nsteps;
static BgSub subs[64];
static int nsubs, sel = -1;     // sel: 1..24, 25 = bar
static bool cpuMarks;           // show the CPU's last move
static int resumePhase;         // where a refused resignation returns to
static int cpuResignLevel;      // what the CPU offered (1 single, 2 gammon, 3 backgammon)
static bool cpuResignRefused;   // you refused it: the CPU won't offer again this game
static bool hintMarks;          // show the engine's suggested move
static BgBoard hintBefore, hintAfter;
static bool menuOpen;
static bool flying, flyMine, tumbling;  // animation: a checker in flight / dice rolling
static bool blinkOn = true;             // ROLL / waiting prompts wink on and off
static int flyX, flyY;
static bool touchLog;  // serial 'L': log every touch, its phase change and handling time
// Start of one of your turns, for take-back (cube state included).
struct Snap { BgBoard b; int8_t d1, d2; int16_t cubeVal; int8_t cubeOwn; };
static Snap hist[32];
static int nhist;
static int scoreYou, scoreCpu;
static int cubeVal = 1, cubeOwn = 0;  // owner from your side: +1 you, -1 CPU, 0 centred
static const float CUBE_X = 0.68f;     // Janowski cube efficiency
static char offerTxt[40];
static char msg[48], evalTxt[32];
static uint16_t evalCol;
static BgBoard kids[1024];

static uint16_t C_FRAME, C_FELT, C_PTA, C_PTB, C_ME, C_MERIM, C_MEIN, C_OP, C_OPRIM, C_OPIN,
    C_BAR, C_TRAY, C_SEL, C_TEXT, C_DIM, C_GOOD, C_BAD, C_STATUS, C_CPU, C_HINT, C_OPHI, C_MEHI;

static void calibrate();

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

// ---- geometry: designed at 480x320 and scaled to the board's W x H ----
// Horizontal sizes scale with W, vertical with H, and round things (checkers,
// dice, text, menus) with the smaller of the two. At 480x320 every value below
// is the original hand-tuned one.
static constexpr float LSX = W / 480.0f, LSY = H / 320.0f, LSS = LSX < LSY ? LSX : LSY;
static constexpr int sx(float v) { return int(v * LSX + 0.5f); }
static constexpr int sy(float v) { return int(v * LSY + 0.5f); }
static constexpr int ss(float v) { return int(v * LSS + 0.5f) > 0 ? int(v * LSS + 0.5f) : 1; }
static const int SB = sy(22);                      // status bar height
static const int PW = sx(32);                      // point width
static const int LX = sx(6);                       // left field x
static const int BARX = LX + 6 * PW, BARW = sx(30);
static const int RFX = BARX + BARW;                // right field x
static const int TRX = RFX + 6 * PW + sx(4);       // tray
static const int TRW = W - TRX - sx(6);
static const int FT = SB + sy(3), FB = H - sy(4);  // field top / bottom
static const int CR = ss(13), STEP = 2 * CR + 1;   // checker radius, stack step
static const int PH = 5 * STEP - ss(7);            // point (triangle) height
static const int MIDY = (FT + FB + 1) / 2;         // dice row / board middle
static const int DS = ss(22);                      // die size
static const int OFFS = ss(8);                     // borne-off slab pitch
static const int BAROFF = ss(40);                  // bar checkers' distance from MIDY
static const int TAP_H = ss(30), TAP_D = ss(40);   // dice / tray tap zone: MIDY - TAP_H .. MIDY + TAP_D
static_assert(PW >= 2 * CR + 1, "checkers must fit on a point");
static_assert(2 * (5 * STEP) <= FB - FT + STEP, "five checkers per half must fit");

static int colX(int c) { return c < 6 ? LX + c * PW : RFX + (c - 6) * PW; }
static void pointGeom(int p, int& c, bool& top) {
  if (p >= 13) { c = p - 13; top = true; } else { c = 12 - p; top = false; }
}
static int stackY(bool top, int i) { return top ? FT + CR + 1 + i * STEP : FB - CR - 1 - i * STEP; }
static int pointCX(int p) { int c; bool t; pointGeom(p, c, t); return colX(c) + PW / 2; }

// ---- drawing (screen coordinates; shifted by the band offset) ----
// A board checker's face (everything inside the rim), anti-aliased once at
// start-up: drawing a checker is then one smooth rim circle plus a keyed blit,
// instead of five smooth circles every time.
static LGFX_Sprite faceMe(&cv), faceOp(&cv);
static const int FR = CR - 1;                  // face radius; sprites are 2*FR+1 square
static const uint16_t FACE_KEY = TFT_MAGENTA;  // transparent outside the face
static void makeFace(LGFX_Sprite& s, bool mine) {
  s.setColorDepth(16);
  s.createSprite(2 * FR + 1, 2 * FR + 1);
  s.fillSprite(mine ? C_MERIM : C_OPRIM);  // the rim the face's AA edge blends into
  s.fillSmoothCircle(FR, FR, FR, mine ? C_ME : C_OP);
  s.fillSmoothCircle(FR, FR, CR - 4, mine ? C_MEIN : C_OPIN);
  s.fillSmoothCircle(FR, FR, CR - 5, mine ? C_ME : C_OP);
  s.fillSmoothCircle(FR - CR / 3, FR - CR / 3, 2, mine ? C_MEHI : C_OPHI);
  for (int y = 0; y <= 2 * FR; y++)
    for (int x = 0; x <= 2 * FR; x++)
      if ((x - FR) * (x - FR) + (y - FR) * (y - FR) > (FR + 0.5f) * (FR + 0.5f)) s.drawPixel(x, y, FACE_KEY);
}

static void checker(int cx, int cy, bool mine, int r = CR) {
  cy -= oy;
  if (cy < -r || cy > BAND + r || cx + r < clipX0 || cx - r >= clipX1) return;
  if (r == CR && faceMe.getBuffer()) {
    cv.fillSmoothCircle(cx, cy, r, mine ? C_MERIM : C_OPRIM);
    (mine ? faceMe : faceOp).pushSprite(cx - FR, cy - FR, FACE_KEY);
    return;
  }
  // Rim, face, a smooth inner ring, and a small highlight up-left.
  cv.fillSmoothCircle(cx, cy, r, mine ? C_MERIM : C_OPRIM);
  cv.fillSmoothCircle(cx, cy, r - 1, mine ? C_ME : C_OP);
  if (r >= ss(9)) {
    cv.fillSmoothCircle(cx, cy, r - 4, mine ? C_MEIN : C_OPIN);
    cv.fillSmoothCircle(cx, cy, r - 5, mine ? C_ME : C_OP);
    cv.fillSmoothCircle(cx - r / 3, cy - r / 3, 2, mine ? C_MEHI : C_OPHI);
  }
}
static void rect(int x, int y, int w, int h, uint16_t c) { cv.fillRect(x, y - oy, w, h, c); }
static void dot(int x, int y, int r, uint16_t c) { cv.fillSmoothCircle(x, y - oy, r, c); }
// The point triangle's shape, worked out once: for each row (0 = the base),
// the fully covered span and the coverage of the edge pixel either side. A
// point is then one horizontal line per row plus two blended edge pixels -
// anti-aliased, and far cheaper than a filled triangle and two smooth lines.
static int16_t triL[512], triR[512];  // first / last fully covered x in the row
static uint8_t triAL[512], triAR[512];  // coverage (0..255) of the pixels at triL-1 / triR+1
static void makeTriangle() {
  const float apex = PW / 2.0f, right = PW - 1 + 1.0f;  // edges run from x = 0 and x = PW to the apex
  for (int r = 0; r < PH && r < 512; r++) {
    float t = (r + 0.5f) / PH;
    float xl = apex * t, xr = right - (right - apex) * t;  // continuous edges at the row's centre
    int l = (int)ceilf(xl), rr = (int)floorf(xr) - 1;
    triL[r] = l; triR[r] = rr;
    triAL[r] = (uint8_t)((l - xl) * 255);
    triAR[r] = (uint8_t)((xr - (rr + 1)) * 255);
  }
}
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {  // a/255 of fg over bg
  int r = (((fg >> 11) * a) + ((bg >> 11) * (255 - a))) / 255;
  int g = ((((fg >> 5) & 63) * a) + (((bg >> 5) & 63) * (255 - a))) / 255;
  int b = (((fg & 31) * a) + ((bg & 31) * (255 - a))) / 255;
  return r << 11 | g << 5 | b;
}
static void tri(int x0, bool top, uint16_t col) {
  if (x0 + PW <= clipX0 || x0 >= clipX1) return;
  // Rows of this triangle inside the band: screen y = FT + r (top) or FB - 1 - r.
  for (int y = max(oy, top ? FT : FB - PH); y < min(oy + BAND, top ? FT + PH : FB); y++) {
    int r = top ? y - FT : FB - 1 - y;
    int yy = y - oy;
    if (triR[r] >= triL[r]) cv.drawFastHLine(x0 + triL[r], yy, triR[r] - triL[r] + 1, col);
    if (triAL[r]) cv.drawPixel(x0 + triL[r] - 1, yy, blend565(col, C_FELT, triAL[r]));
    if (triAR[r]) cv.drawPixel(x0 + triR[r] + 1, yy, blend565(col, C_FELT, triAR[r]));
  }
}
static void die(int x, int y, int v, bool cpu, bool used) {
  y -= oy;
  uint16_t body = cpu ? C_OP : 0xF79D, pip = cpu ? C_ME : TFT_BLACK;
  if (used) { body = C_DIM; pip = 0x4208; }
  if (cpu) { cv.fillSmoothRoundRect(x, y, DS, DS, ss(4), C_OPRIM); cv.fillSmoothRoundRect(x + 1, y + 1, DS - 2, DS - 2, ss(3), body); }
  else cv.fillSmoothRoundRect(x, y, DS, DS, ss(4), body);
  // Pip centres on a 22-unit die, scaled to DS.
  static const uint8_t P[7][6][2] = {{}, {{11,11}}, {{6,6},{16,16}}, {{6,6},{11,11},{16,16}},
    {{6,6},{16,6},{6,16},{16,16}}, {{6,6},{16,6},{11,11},{6,16},{16,16}},
    {{6,6},{16,6},{6,11},{16,11},{6,16},{16,16}}};
  for (int i = 0; i < v; i++)
    cv.fillSmoothCircle(x + P[v][i][0] * DS / 22, y + P[v][i][1] * DS / 22, ss(2), pip);
}
static void text(const char* s, int x, int y) { cv.drawString(s, x, y - oy); }
static void number(int n, int x, int y) { cv.drawNumber(n, x, y - oy); }

static bool canMoveFrom(int p) {
  for (int i = 0; i < nsubs; i++) if (subs[i].from == p) return true;
  return false;
}

// Screen position of the target marker for destination t (0 = bear off).
static void targetXY(int t, int& x, int& y) {
  if (t > 0) {
    int c; bool top; pointGeom(t, c, top);
    int n = g.pts[t] > 0 ? min((int)g.pts[t], 4) : 0;
    x = colX(c) + PW / 2; y = stackY(top, n);
  } else { x = TRX + TRW / 2; y = FB - OFFS - g.off[0] * OFFS - ss(10); }
}

static void drawMarks(const BgBoard& a, const BgBoard& b, bool mine, uint16_t col) {
  auto cnt = [mine](const BgBoard& x, int p) { int v = mine ? x.pts[p] : -x.pts[p]; return v > 0 ? v : 0; };
  for (int p = 1; p <= 24; p++) {
    int was = cnt(a, p), now = cnt(b, p);
    if (was == now) continue;
    // On the stack itself: a dot on the arriving checker, a ring in the slot
    // the leaving checker vacated.
    int c; bool top; pointGeom(p, c, top);
    int x = colX(c) + PW / 2;
    if (now > was) dot(x, stackY(top, min(now, 5) - 1), ss(5), col);
    else {
      int y = stackY(top, min(now, 4)) - oy;
      cv.drawCircle(x, y, ss(7), col); cv.drawCircle(x, y, ss(6), col);
    }
  }
  int s = mine ? 0 : 1;
  if (b.off[s] > a.off[s])
    dot(TRX + TRW / 2, mine ? FB - OFFS - b.off[s] * OFFS - ss(10) : FT + OFFS + b.off[s] * OFFS + ss(10), ss(5), col);
  if (b.bar[s] < a.bar[s]) { int y = mine ? MIDY + BAROFF : MIDY - BAROFF; cv.drawCircle(BARX + BARW / 2, y - oy, ss(5), col); }
}

// ---- menu (2 x 4 buttons over the board) ----
static const int MW = ss(300), MX = (W - MW) / 2, MY = SB, MH = ss(258), BW = ss(136), BH = ss(46);
static const int BGAP = ss(8);  // between buttons
// Tapping outside the panel (or the menu icon) also closes it.
static const int NMENU = 8;
static const char* const MENU[NMENU] = {"Undo step", "New game", "Undo move", "Hint",
                                        "Resign", "Reset score", "Calibrate touch", "Claim win"};
static bool canTakeBack() {
  if (phase == DONE) return true;
  if (phase == MOVE) return memcmp(&g, &turnStart, sizeof g) || nhist >= 2;
  if (phase == PASS) return nhist >= 2;
  return phase == ROLL && nhist >= 1;
}
static bool menuEnabled(int i) {
  if (i == 0) return (phase == MOVE || phase == DONE) && nsteps > 0;
  if (i == 2) return canTakeBack();
  if (i == 3) return phase == MOVE || phase == ROLL;  // move hint / cube advice
  if (i == 6) return BOARD_TOUCH_CALIBRATION;         // capacitive boards don't calibrate
  if (i == 4 || i == 7) return phase == ROLL || phase == MOVE || phase == PASS || phase == DONE;
  return true;
}
static int buttonX(int i) { return MX + ss(10) + (i % 2) * (BW + BGAP); }

// A question over the board (the CPU's double, choosing how much to resign,
// the CPU's resignation): a title, a line of detail and 2 or 4 buttons.
static char dlgTitle[40];
static char dlgLbl[4][20];
static int dlgN;
static const int DLG_Y = sy(70);
static int dlgBY(int i) { return DLG_Y + ss(74) + (i / 2) * (BH + BGAP); }
static int dlgH() { return ss(74) + (dlgN / 2) * (BH + BGAP) + ss(6); }
static bool dialogUp() { return phase == OFFER || phase == RESIGN || phase == RESOFFER || phase == COUNTER; }
static void setDialog(const char* title, const char* const* lbl, int n) {
  snprintf(dlgTitle, sizeof dlgTitle, "%s", title);
  for (int i = 0; i < n; i++) snprintf(dlgLbl[i], sizeof dlgLbl[i], "%s", lbl[i]);
  dlgN = n;
}
static int dlgHit(int x, int y) {
  for (int i = 0; i < dlgN; i++)
    if (x >= buttonX(i) && x < buttonX(i) + BW && y >= dlgBY(i) && y < dlgBY(i) + BH) return i;
  return -1;
}
static void drawDialog() {
  if (DLG_Y + dlgH() < oy || DLG_Y > oy + BAND) return;
  cv.fillRoundRect(MX, DLG_Y - oy, MW, dlgH(), ss(10), C_STATUS);
  cv.drawRoundRect(MX, DLG_Y - oy, MW, dlgH(), ss(10), C_DIM);
  cv.setTextDatum(middle_center);
  cv.setFont(strlen(dlgTitle) > 22 ? &F_B16 : &F_B21);  // long titles must fit the panel
  cv.setTextColor(C_TEXT);
  text(dlgTitle, MX + MW / 2, DLG_Y + ss(22));
  cv.setFont(&F_S16);
  cv.setTextColor(C_DIM);
  text(offerTxt, MX + MW / 2, DLG_Y + ss(50));
  for (int i = 0; i < dlgN; i++) {
    cv.fillRoundRect(buttonX(i), dlgBY(i) - oy, BW, BH, ss(8), C_FELT);
    cv.drawRoundRect(buttonX(i), dlgBY(i) - oy, BW, BH, ss(8), C_SEL);
    cv.setTextColor(C_TEXT);
    char* nl = strchr(dlgLbl[i], '\n');  // two-line label: name, then detail
    if (nl) {
      *nl = 0;
      cv.setFont(&F_S16);
      text(dlgLbl[i], buttonX(i) + BW / 2, dlgBY(i) + BH / 2 - ss(9));
      cv.setTextColor(C_DIM);
      text(nl + 1, buttonX(i) + BW / 2, dlgBY(i) + BH / 2 + ss(10));
      *nl = '\n';
    } else {
      cv.setFont(strlen(dlgLbl[i]) > 9 ? &F_S16 : &F_S21);
      text(dlgLbl[i], buttonX(i) + BW / 2, dlgBY(i) + BH / 2);
    }
  }
}

// The cube in the bar: centred, or at the owner's end. Yellow rim = you may double.
static const int CUBE_S = ss(26);
static int cubeY() { return cubeOwn > 0 ? FB - CUBE_S - ss(4) : cubeOwn < 0 ? FT + ss(4) : MIDY - CUBE_S / 2; }
static void drawCube() {
  int x = BARX + (BARW - CUBE_S) / 2, y = cubeY();
  const int RIM = ss(3);
  if (y + CUBE_S + RIM < oy || y - RIM > oy + BAND || x + CUBE_S + RIM < clipX0 || x - RIM >= clipX1) return;
  bool may = phase == ROLL && cubeOwn >= 0 && cubeVal < 64;
  if (may) cv.fillSmoothRoundRect(x - RIM, y - RIM - oy, CUBE_S + 2 * RIM, CUBE_S + 2 * RIM, ss(7), C_SEL);
  cv.fillSmoothRoundRect(x, y - oy, CUBE_S, CUBE_S, ss(5), 0xEF3A);
  cv.setFont(&F_B16);
  cv.setTextDatum(middle_center);
  cv.setTextColor(TFT_BLACK);
  number(cubeVal == 1 ? 64 : cubeVal, x + CUBE_S / 2, y + CUBE_S / 2 + 1);
}
static int buttonY(int i) { return MY + ss(36) + (i / 2) * (BH + BGAP); }

static void drawMenu() {
  if (MY + MH < oy || MY > oy + BAND) return;
  cv.fillRoundRect(MX, MY - oy, MW, MH, ss(10), C_STATUS);
  cv.drawRoundRect(MX, MY - oy, MW, MH, ss(10), C_DIM);
  cv.setFont(&F_B21);
  cv.setTextDatum(middle_center);
  cv.setTextColor(C_TEXT);
  text("Menu", MX + MW / 2, MY + ss(18));
  cv.setFont(&F_S21);
  for (int i = 0; i < NMENU; i++) {
    bool on = menuEnabled(i);
    cv.fillRoundRect(buttonX(i), buttonY(i) - oy, BW, BH, ss(8), on ? C_FELT : C_BAR);
    cv.drawRoundRect(buttonX(i), buttonY(i) - oy, BW, BH, ss(8), on ? C_SEL : C_DIM);
    cv.setTextColor(on ? C_TEXT : C_DIM);
    if (i == 6) {  // two lines
      cv.setFont(&F_S16);
      text("Calibrate", buttonX(i) + BW / 2, buttonY(i) + BH / 2 - ss(9));
      text("touch", buttonX(i) + BW / 2, buttonY(i) + BH / 2 + ss(10));
      cv.setFont(&F_S21);
    } else text(MENU[i], buttonX(i) + BW / 2, buttonY(i) + BH / 2);
  }
}

static void drawBand() {
  rect(0, 0, W, SB, C_STATUS);
  rect(0, SB, W, H - SB, C_FRAME);
  rect(LX, FT, 6 * PW, FB - FT, C_FELT);
  rect(RFX, FT, 6 * PW, FB - FT, C_FELT);
  rect(BARX, SB, BARW, H - SB, C_BAR);
  rect(TRX, FT, TRW, FB - FT, C_TRAY);
  // Points, skipping a row whose triangles don't reach this band.
  bool topRow = oy < FT + PH + 1, botRow = oy + BAND > FB - PH - 1;
  for (int c = 0; c < 12; c++) {
    if (topRow) tri(colX(c), true, c % 2 ? C_PTB : C_PTA);
    if (botRow) tri(colX(c), false, c % 2 ? C_PTA : C_PTB);
  }
  // Movable checkers: a yellow bar at the point's base (or the bar).
  if (phase == MOVE && sel < 0) {
    for (int p = 1; p <= 24; p++) if (canMoveFrom(p)) {
      int c; bool top; pointGeom(p, c, top);
      rect(colX(c) + ss(4), top ? FT : FB - ss(4), PW - 2 * ss(4), ss(4), C_SEL);
    }
  }
  if (sel > 0 && sel <= 24) {
    int c; bool top; pointGeom(sel, c, top);
    cv.drawRect(colX(c), (top ? FT : FB - 5 * STEP - 1) - oy, PW, 5 * STEP + 1, C_SEL);
  }
  // checkers
  cv.setFont(&F_B21);
  cv.setTextDatum(middle_center);
  for (int p = 1; p <= 24; p++) {
    int n = g.pts[p]; if (!n) continue;
    bool mine = n > 0; int a = abs(n), c; bool top; pointGeom(p, c, top);
    int cx = colX(c) + PW / 2;
    for (int i = 0; i < min(a, 5); i++) checker(cx, stackY(top, i), mine);
    if (a > 5 && abs(stackY(top, 4) - oy - BAND / 2) < BAND && cx + CR >= clipX0 && cx - CR < clipX1) {
      cv.setFont(a >= 10 ? &F_B16 : &F_B21);  // two digits must fit on the checker
      cv.setTextColor(mine ? TFT_BLACK : C_ME); number(a, cx, stackY(top, 4) + 1);
    }
  }
  int bx = BARX + BARW / 2;
  for (int i = 0; i < g.bar[1]; i++) checker(bx, MIDY - BAROFF - i * STEP, false);
  for (int i = 0; i < g.bar[0]; i++) checker(bx, MIDY + BAROFF + i * STEP, true);
  drawCube();
  if (sel == 25) cv.drawRect(BARX, MIDY + BAROFF - CR - 1 - oy, BARW, g.bar[0] * STEP + 2, C_SEL);
  else if (phase == MOVE && sel < 0 && canMoveFrom(25))
    rect(BARX + ss(4), MIDY + BAROFF - CR - ss(5), BARW - 2 * ss(4), ss(4), C_SEL);
  // The CPU's last move (orange) or a hint (cyan): rings where checkers left,
  // dots where they landed.
  if (cpuMarks) drawMarks(cpuBefore, g, false, C_CPU);
  if (hintMarks) drawMarks(hintBefore, hintAfter, true, C_HINT);
  if (flying) checker(flyX, flyY, flyMine);
  if (clipX1 > TRX) {  // the tray: borne off, dice and prompts (skipped when not being redrawn)
  // borne off
  for (int i = 0; i < g.off[1]; i++) {
    rect(TRX + 3, FT + 2 + i * OFFS, TRW - 6, OFFS - 1, C_OPRIM); rect(TRX + 4, FT + 3 + i * OFFS, TRW - 8, OFFS - 3, C_OP);
  }
  for (int i = 0; i < g.off[0]; i++) {
    rect(TRX + 3, FB - OFFS - 1 - i * OFFS, TRW - 6, OFFS - 1, C_MERIM); rect(TRX + 4, FB - OFFS - i * OFFS, TRW - 8, OFFS - 3, C_ME);
  }
  // dice
  if (dice[0]) {  // the last dice stay on show until new ones are rolled
    bool dbl = dice[0] == dice[1];
    bool used0 = false, used1 = false;
    if ((phase == MOVE || phase == DONE || phase == PASS) && !cpuDice && (!dbl || phase == PASS)) {
      used0 = true; used1 = true;
      if (phase != PASS)  // a dance leaves both dice unusable
        for (int i = 0; i < nrem; i++) { if (rem[i] == dice[0]) used0 = false; if (rem[i] == dice[1]) used1 = false; }
    }
    int dx = TRX + (TRW - 2 * DS - ss(2)) / 2;  // two dice, centred in the tray
    die(dx, MIDY - DS / 2, dice[0], cpuDice, used0);
    die(dx + DS + ss(2), MIDY - DS / 2, dice[1], cpuDice, used1);
    if (dbl && phase == MOVE && !cpuDice) {
      cv.setFont(&F_S16); cv.setTextColor(C_TEXT); cv.setTextDatum(middle_center);
      char b[4]; snprintf(b, sizeof b, "x%d", nrem);
      text(b, TRX + TRW / 2, MIDY + DS / 2 + ss(11));
    }
  }
  if (phase == OVER && blinkOn) {  // the game is over: the tray starts the next
    cv.setFont(&F_B16); cv.setTextColor(C_SEL); cv.setTextDatum(middle_center);
    int ny = dice[0] ? MIDY + DS / 2 + ss(13) : MIDY - ss(9);  // under the dice, where ROLL goes
    text("NEW", TRX + TRW / 2, ny);
    text("GAME", TRX + TRW / 2, ny + ss(18));
  }
  if ((phase == DONE || phase == PASS) && blinkOn) {  // hand the dice over
    cv.setFont(&F_B16); cv.setTextColor(C_SEL); cv.setTextDatum(middle_center);
    text(phase == PASS ? "PASS" : "DONE", TRX + TRW / 2, MIDY + DS / 2 + ss(13));
  }
  if (phase == ROLL && !tumbling && blinkOn) {
    cv.setFont(&F_B16); cv.setTextColor(C_SEL); cv.setTextDatum(middle_center);
    text("ROLL", TRX + TRW / 2, dice[0] ? MIDY + DS / 2 + ss(13) : MIDY);
  }
  }  // tray
  // targets for the selected checker
  // Filled dot: one die. Ring: several dice with the same checker.
  if (sel > 0) {
    for (int i = 0; i < npaths; i++) {
      int x, y;
      targetXY(paths[i].to, x, y);
      if (paths[i].n == 1) dot(x, y, ss(6), C_SEL);
      else { dot(x, y, ss(7), C_SEL); dot(x, y, ss(4), C_FELT); }
    }
  }
  // status bar: menu | pips | message | eval (only in the bands it occupies -
  // smooth-font text is the costliest thing on the screen)
  if (oy < SB) {
    const int my = SB / 2;  // text middle
    for (int i = 0; i < 3; i++) rect(sx(6), my - sy(6) + i * sy(5), sx(20), sy(2), menuOpen ? C_SEL : C_TEXT);
    cv.setFont(&F_S16);
    cv.setTextDatum(middle_left);
    checker(sx(44), my, true, ss(7));
    cv.setTextColor(C_TEXT); number(pips(g, 0), sx(56), my);
    checker(sx(100), my, false, ss(7));
    number(pips(g, 1), sx(112), my);
    cv.setTextDatum(middle_center);
    // Waiting for a tap to pass / start a new game: the message pulses.
    cv.setTextColor(C_TEXT);
    text(msg, sx(262), my);
    cv.setTextDatum(middle_right);
    cv.setTextColor(evalCol); text(evalTxt, W - sx(6), my);
  }
  if (dialogUp() && !menuOpen) drawDialog();
  if (menuOpen) drawMenu();
}

static void draw() {
  uint32_t t0 = millis();
  for (oy = 0; oy < H; oy += BAND) {
    drawBand();
    display_push(cv, oy, 0, W);
  }
  Serial.printf("full draw %u ms\n", millis() - t0);
}

// ---- animation ----
// Redraw just the rectangle [x0,x1) x [y0,y1): only the bands it touches, with
// both the sprite and the panel clipped to its columns, so a frame costs a few
// ms instead of a full ~100 ms redraw.
static uint32_t profBands, profDrawUs, profPushUs;  // animation profiling (fly log)
static void redrawRegion(int x0, int y0, int x1, int y1) {
  x0 = max(x0, 0); x1 = min(x1, W); y0 = max(y0, 0); y1 = min(y1, H);
  if (x0 >= x1 || y0 >= y1) return;
  for (oy = y0 / BAND * BAND; oy < y1; oy += BAND) {
    cv.setClipRect(x0, 0, x1 - x0, BAND);
    clipX0 = x0; clipX1 = x1;
    uint32_t t0 = micros();
    drawBand();
    uint32_t t1 = micros();
    clipX0 = 0; clipX1 = W;
    cv.clearClipRect();
    display_push(cv, oy, x0, x1);
    profBands++; profDrawUs += t1 - t0; profPushUs += micros() - t1;
  }
}

// Screen centre of the checker in slot i (0 = bottom of the stack) of spot p
// for one side: p 1..24 point, 25 bar, 0 borne off.
static void slotXY(bool mine, int p, int i, int& x, int& y) {
  if (p == 25) { x = BARX + BARW / 2; y = mine ? MIDY + BAROFF + i * STEP : MIDY - BAROFF - i * STEP; return; }
  if (p == 0) { x = TRX + TRW / 2; y = mine ? FB - OFFS / 2 - 2 - i * OFFS : FT + OFFS / 2 + 1 + i * OFFS; return; }
  int c; bool top; pointGeom(p, c, top);
  x = colX(c) + PW / 2; y = stackY(top, min(i, 4));
}
static int countAt(const BgBoard& b, bool mine, int p) {
  if (p == 25) return b.bar[mine ? 0 : 1];
  if (p == 0) return b.off[mine ? 0 : 1];
  int v = mine ? b.pts[p] : -b.pts[p];
  return v > 0 ? v : 0;
}

// Slide a checker from (x0,y0) to (x1,y1), eased, ~120-400 ms by distance.
static void fly(int x0, int y0, int x1, int y1, bool mine) {
  float dist = sqrtf(float((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
  // ~0.4-1.2 s, timed by distance in design (480x320) pixels so every screen size moves alike
  uint32_t dur = 400 + (uint32_t)(dist / LSS * 1.8f), t0 = millis();
  flying = true; flyMine = mine;
  profBands = profDrawUs = profPushUs = 0;
  int px = x0, py = y0, frames = 0;
  for (;; frames++) {
    float t = min(1.0f, (millis() - t0) / float(dur));
    float e = t * t * (3 - 2 * t);  // smoothstep
    flyX = x0 + int((x1 - x0) * e); flyY = y0 + int((y1 - y0) * e);
    redrawRegion(min(px, flyX) - CR - 2, min(py, flyY) - CR - 2, max(px, flyX) + CR + 3, max(py, flyY) + CR + 3);
    px = flyX; py = flyY;
    if (t >= 1) break;
  }
  flying = false;
  Serial.printf("fly %.0f px: %d frames in %u ms (%u bands, draw %u us + push %u us per band)\n", dist, frames + 1,
                millis() - t0, profBands, profDrawUs / max(profBands, (uint32_t)1), profPushUs / max(profBands, (uint32_t)1));
}

// Animate one checker step from board a to board b (both from your side).
// from / to: 1..24, 25 = bar, 0 = off. A hit then sends the blot to the bar.
static void animateStep(const BgBoard& a, const BgBoard& b, int from, int to, bool mine) {
  int x0, y0, x1, y1;
  slotXY(mine, from, countAt(a, mine, from) - 1, x0, y0);
  BgBoard mid = a;  // the board without the moving checker
  if (from == 25) mid.bar[mine ? 0 : 1]--;
  else mid.pts[from] += mine ? -1 : 1;
  slotXY(mine, to, countAt(mid, mine, to), x1, y1);
  g = mid;
  fly(x0, y0, x1, y1, mine);
  int opp = mine ? 1 : 0;
  const int R = CR + 3;
  if (b.bar[opp] > a.bar[opp]) {  // hit: the blot flies to the bar
    BgBoard m2 = b;
    m2.bar[opp]--;
    g = m2;
    int bx, by;
    slotXY(!mine, 25, b.bar[opp] - 1, bx, by);
    fly(x1, y1, bx, by, !mine);
    g = b;
    redrawRegion(bx - R, by - R, bx + R, by + R);
  }
  g = b;
  redrawRegion(x1 - R, y1 - R, x1 + R, y1 + R);  // settle: stack count label, tray slab
}

// Tumble the dice in the tray for ~0.4 s before showing d1-d2.
static uint32_t tapAt;  // when the last tap was handled (handover latency log)
static void tumble(int d1, int d2, bool cpu) {
  if (cpu) Serial.printf("dice start %u ms after the tap\n", millis() - tapAt);
  tumbling = true; cpuDice = cpu;
  for (int i = 0; i < 8; i++) {  // slowing down, ~0.4 s in all: a roll shouldn't feel like a wait
    dice[0] = random(1, 7); dice[1] = random(1, 7);
    redrawRegion(TRX, MIDY - TAP_H, W, MIDY + TAP_D);
    delay(25 + i * 6);
  }
  dice[0] = d1; dice[1] = d2;
  tumbling = false;
  redrawRegion(TRX, MIDY - TAP_H, W, MIDY + TAP_D);
}

// The CPU's chosen result as single checker steps (CPU-relative), via the same
// sub-move generator a human turn uses. Returns the number of steps.
static BgSub cpuSteps[4];
static bool findSteps(const BgBoard& b, const int* d, int n, const BgBoard& target, int depth, int& len) {
  static BgSub lv[4][64];
  int k = n ? bg_next_submoves(b, d, n, lv[depth], 64) : 0;
  if (!k) { len = depth; return !memcmp(&b, &target, sizeof b); }
  for (int i = 0; i < k; i++) {
    int d2[4], n2 = 0; bool used = false;
    for (int j = 0; j < n; j++) { if (!used && d[j] == lv[depth][i].die) { used = true; continue; } d2[n2++] = d[j]; }
    cpuSteps[depth] = lv[depth][i];
    if (findSteps(lv[depth][i].result, d2, n2, target, depth + 1, len)) return true;
  }
  return false;
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

// pts: 1 single, 2 gammon, 3 backgammon - times the cube; 0 = a dropped double.
static void gameOver(int pts, bool youWon, const char* how = nullptr) {
  int won = pts ? pts * cubeVal : cubeVal;
  (youWon ? scoreYou : scoreCpu) += won;
  const char* kind = how ? how : !pts ? "dropped" : pts == 3 ? "backgammon" : pts == 2 ? "gammon" : "single";
  snprintf(msg, sizeof msg, "%s %d (%s)", youWon ? "You win" : "CPU wins", won, kind);
  snprintf(evalTxt, sizeof evalTxt, "Score %d-%d", scoreYou, scoreCpu);  // you-CPU
  evalCol = C_TEXT;
  phase = OVER;
  sel = -1; nsubs = 0;
}

// full: redraw everything first (after a dialog, or a fresh board); from your
// own turn only the status line changes, so the dice start tumbling at once.
static void cpuTurn(int d1, int d2, bool full = true) {
  snprintf(msg, sizeof msg, "CPU rolls %d-%d...", d1, d2);
  nsubs = 0; sel = -1; cpuMarks = false;
  if (full) draw();
  tumble(d1, d2, true);
  if (!full) redrawRegion(0, 0, W, SB);  // status line after: the dice go first
  uint32_t t0 = millis();
  BgBoard me = bg_swap(g);
  int n = bg_genmoves(me, d1, d2, kids, 1024);
  int best = bg_best(net, kids, n);
  uint32_t ms = millis() - t0;
  cpuBefore = g;
  // Play it out checker by checker (CPU point p is your 25-p; bar and off keep 25 / 0).
  int dl[4] = {d1, d2, d1, d1}, len = 0;
  if (findSteps(me, dl, d1 == d2 ? 4 : 2, kids[best], 0, len)) {
    BgBoard cur = me;
    for (int i = 0; i < len; i++) {
      const BgSub& s = cpuSteps[i];
      auto mapPt = [](int p) { return p == 25 || p == 0 ? p : 25 - p; };
      if (i) delay(200);  // a beat between the CPU's checkers
      animateStep(bg_swap(cur), bg_swap(s.result), mapPt(s.from), mapPt(s.to), false);
      cur = s.result;
    }
  }
  g = bg_swap(kids[best]);
  cpuMarks = true;
  Serial.printf("cpu %d-%d: %d moves, %u ms\n", d1, d2, n, ms);
  if (int r = bg_result(kids[best])) { gameOver(r, false); return; }
  snprintf(msg, sizeof msg, n == 1 && !memcmp(&kids[0], &me, sizeof me) ? "CPU can't move" : "CPU played %d-%d", d1, d2);
  phase = ROLL;
  setEval();
}

// Begin your turn with d1-d2. push: record it in the take-back history.
static void startMove(int d1, int d2, bool push = true) {
  if (push) {
    if (nhist == 32) { memmove(hist, hist + 1, sizeof hist - sizeof hist[0]); nhist--; }
    hist[nhist++] = Snap{g, (int8_t)d1, (int8_t)d2, (int16_t)cubeVal, (int8_t)cubeOwn};
  }
  hintMarks = false;
  dice[0] = d1; dice[1] = d2; cpuDice = false;
  nrem = 0;
  rem[nrem++] = d1; rem[nrem++] = d2;
  if (d1 == d2) { rem[nrem++] = d1; rem[nrem++] = d1; }
  turnStart = g;
  nsteps = 0;
  sel = -1; npaths = 0;
  nsubs = bg_next_submoves(g, rem, nrem, subs, 64);
  if (!nsubs) { phase = PASS; snprintf(msg, sizeof msg, "No legal move: tap the dice"); }
  else { phase = MOVE; snprintf(msg, sizeof msg, "Your move: %d-%d", d1, d2); }
}

static void newGame() {
  g = bg_start();
  cpuMarks = hintMarks = false;
  cubeVal = 1; cubeOwn = 0;
  cpuResignRefused = false;
  nhist = 0;
  int a, b;
  do { a = random(1, 7); b = random(1, 7); } while (a == b);
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

// The net's cube call for whoever is on roll in `onRoll` (mover-relative).
static BgCubeCall cubeCall(const BgBoard& onRoll, int own) {
  float p[6];
  net.eval(onRoll, p);
  return bg_cube_call(p, own, CUBE_X);
}

static void endHumanTurn() {
  nsubs = 0; sel = -1;
  // A hopeless CPU resigns rather than play on: the least you'd accept -
  // a single unless you have real gammon (or backgammon) chances.
  if (!cpuResignRefused) {
    float q[6];
    net.eval(bg_swap(g), q);  // CPU on roll
    if (q[0] + q[1] + q[2] < 0.002f) {
      cpuResignLevel = q[4] + q[5] < 0.01f ? 1 : q[5] < 0.01f ? 2 : 3;
      const char* nm[4] = {"", "a single", "a gammon", "a backgammon"};
      char t[40]; snprintf(t, sizeof t, "CPU resigns %s", nm[cpuResignLevel]);
      snprintf(offerTxt, sizeof offerTxt, "You would win %d point%s", cpuResignLevel * cubeVal, cpuResignLevel * cubeVal > 1 ? "s" : "");
      static const char* const AR[2] = {"Accept", "Refuse"};
      setDialog(t, AR, 2);
      snprintf(msg, sizeof msg, "CPU offers to resign");
      phase = RESOFFER; cpuMarks = false;
      return;
    }
  }
  // Before rolling, the CPU may double (cube centred or its own).
  if (cubeOwn <= 0 && cubeVal < 64) {
    BgCubeCall c = cubeCall(bg_swap(g), -cubeOwn);
    Serial.printf("cpu cube: nd %.3f dt %.3f dp %.3f -> %s\n", c.nd, c.dt, c.dp, c.dbl ? "double" : "no double");
    if (c.dbl) {
      phase = OFFER;
      snprintf(msg, sizeof msg, "CPU doubles to %d", cubeVal * 2);
      static const char* const TD[2] = {"Take", "Drop"};
      setDialog(msg, TD, 2);
      float q[6];
      net.eval(bg_swap(g), q);  // CPU on roll: your wins are its losses (cubeless)
      snprintf(offerTxt, sizeof offerTxt, "Your chances: %.0f%%", (q[3] + q[4] + q[5]) * 100);
      cpuMarks = false;
      return;
    }
  }
  cpuTurn(random(1, 7), random(1, 7), false);  // the board is already on screen
}

// You double before rolling: the CPU takes or drops by the same model.
static void humanDouble() {
  BgCubeCall c = cubeCall(g, cubeOwn);
  Serial.printf("you double: nd %.3f dt %.3f dp %.3f -> cpu %s\n", c.nd, c.dt, c.dp, c.take ? "takes" : "drops");
  if (!c.take) { gameOver(0, true); return; }
  cubeVal *= 2; cubeOwn = -1;
  snprintf(msg, sizeof msg, "CPU takes - cube at %d", cubeVal);
}

static bool canDouble() { return phase == ROLL && cubeOwn >= 0 && cubeVal < 64; }

static void applyPath(const Path& p) {
  if (nsteps < 8) { steps[nsteps].b = g; memcpy(steps[nsteps].rem, rem, sizeof rem); steps[nsteps++].nrem = nrem; }
  sel = -1; npaths = 0;  // no selection / target markers while it moves
  for (int k = 0; k < p.n; k++) {
    animateStep(g, p.step[k].result, p.step[k].from, p.step[k].to, true);
    g = p.step[k].result;
    for (int i = 0; i < nrem; i++) if (rem[i] == p.step[k].die) { rem[i] = rem[--nrem]; break; }
  }
  if (int r = bg_result(g)) { gameOver(r, true); return; }
  sel = -1; npaths = 0;
  nsubs = bg_next_submoves(g, rem, nrem, subs, 64);
  if (!nsubs) { phase = DONE; snprintf(msg, sizeof msg, "Tap the dice to finish"); }
  else snprintf(msg, sizeof msg, "Your move: %d-%d", dice[0], dice[1]);
}

// ---- menu actions ----
// Undo the last checker you moved this turn and give its dice back.
static void undoStep() {
  const Step& s = steps[--nsteps];
  g = s.b;
  memcpy(rem, s.rem, sizeof rem); nrem = s.nrem;
  sel = -1; npaths = 0; hintMarks = false;
  nsubs = bg_next_submoves(g, rem, nrem, subs, 64);
  phase = MOVE;
  snprintf(msg, sizeof msg, "Step undone: %d to play", nrem);
}

static void takeBack() {
  cpuMarks = hintMarks = false;
  if ((phase == MOVE || phase == DONE) && memcmp(&g, &turnStart, sizeof g)) {
    g = turnStart;  // undo this turn's moves
  } else {
    if (phase != ROLL) nhist--;  // drop the current turn, go to the one before
    g = hist[nhist - 1].b;
  }
  const Snap& s = hist[nhist - 1];
  cubeVal = s.cubeVal; cubeOwn = s.cubeOwn;
  setEval();
  startMove(s.d1, s.d2, false);
  if (phase == MOVE) snprintf(msg, sizeof msg, "Taken back: play %d-%d", s.d1, s.d2);
}

static void hint() {
  if (phase == ROLL) {  // before rolling: cube advice
    if (cubeOwn < 0) { snprintf(msg, sizeof msg, "CPU owns the cube"); return; }
    BgCubeCall c = cubeCall(g, cubeOwn);
    bool tooGood = !c.dbl && !c.take && c.nd > c.dp;
    snprintf(msg, sizeof msg, "%s", c.dbl ? (c.take ? "Double - CPU should take" : "Double - CPU should drop")
                                 : tooGood ? "Too good - play on" : "No double");
    return;
  }
  if (memcmp(&g, &turnStart, sizeof g)) { g = turnStart; startMove(dice[0], dice[1], false); }
  int n = bg_genmoves(g, dice[0], dice[1], kids, 1024);
  float s;
  int best = bg_best(net, kids, n, &s);
  hintBefore = g; hintAfter = kids[best];
  hintMarks = true; cpuMarks = false;
  snprintf(msg, sizeof msg, "Hint shown (%+.2f)", s);
}

// ---- resigning and claiming a win ----
static const char* const LEVEL[4] = {"", "single", "gammon", "backgammon"};
static const char* const LEVEL_C[4] = {"", "Single", "Gammon", "Backgammon"};
static bool claiming;      // the open dialog / counter is a claim (else a resignation)
static int counterLevel;   // what the CPU would settle for instead

static void pointsLabel(char* out, size_t n, const char* head, int level) {
  int pts = level * cubeVal;
  snprintf(out, n, "%s\n%d point%s", head, pts, pts > 1 ? "s" : "");
}

// Menu > Resign / Claim win: choose a single / gammon / backgammon (or cancel).
static void openSettle(bool claim) {
  resumePhase = phase;
  claiming = claim;
  sel = -1; npaths = 0; hintMarks = false;
  char l[4][20];
  for (int k = 1; k <= 3; k++) pointsLabel(l[k - 1], sizeof l[k - 1], LEVEL_C[k], k);
  snprintf(l[3], sizeof l[3], "Cancel");
  const char* lp[4] = {l[0], l[1], l[2], l[3]};
  setDialog(claim ? "Claim how much?" : "Resign how much?", lp, 4);
  snprintf(offerTxt, sizeof offerTxt, "The CPU may refuse");
  snprintf(msg, sizeof msg, claim ? "Claim a win?" : "Resign?");
  phase = RESIGN;
}

// Your cubeless equity (per cube) from playing on, as the CPU judges it: with
// you on roll at the start of your turn, or - once your move is done or you
// can't move - with the CPU on roll in the position now.
static float yourPlayOnEquity() {
  float p[6];
  if (resumePhase == DONE || resumePhase == PASS) { net.eval(bg_swap(g), p); return -bg_equity(p); }
  net.eval(resumePhase == MOVE ? turnStart : g, p);
  return bg_equity(p);
}

// Resign: the CPU accepts anything worth at least what it expects from playing
// on. Claim: it concedes anything no worse than what it expects to lose. Either
// way a refusal says why, and offers the deal it would take when there is one.
static void settleOffer(int level) {
  const float TOL = 0.01f;
  float you = yourPlayOnEquity();
  if (!claiming) {
    float cpuExp = -you;
    Serial.printf("you resign %d: cpu expects %.3f\n", level, cpuExp);
    if (level >= cpuExp - TOL) { gameOver(level, false, "you resigned"); return; }
    counterLevel = cpuExp - TOL <= 2 ? 2 : 3;  // the smallest it would take
    snprintf(offerTxt, sizeof offerTxt, "It expects %.2f by playing on", cpuExp);
  } else {
    Serial.printf("you claim %d: you expect %.3f\n", level, you);
    if (level <= you + TOL) { gameOver(level, true, "CPU conceded"); return; }
    counterLevel = you + TOL >= 2 ? 2 : you + TOL >= 1 ? 1 : 0;  // the most it would give
    if (!counterLevel) {
      phase = (Phase)resumePhase;
      snprintf(msg, sizeof msg, "CPU refuses: you expect only %.2f", you);
      return;
    }
    snprintf(offerTxt, sizeof offerTxt, "You expect %.2f by playing on", you);
  }
  char t[40], l[2][20];
  snprintf(t, sizeof t, "CPU refuses a %s", LEVEL[level]);
  snprintf(l[0], sizeof l[0], "%s\n%s (%d)", claiming ? "Take" : "Resign", LEVEL[counterLevel], counterLevel * cubeVal);
  snprintf(l[1], sizeof l[1], "Play on");
  const char* lp[2] = {l[0], l[1]};
  setDialog(t, lp, 2);
  snprintf(msg, sizeof msg, "CPU counter-offers");
  phase = COUNTER;
}

static void menuTap(int x, int y) {
  int hit = -1;
  for (int i = 0; i < NMENU; i++)
    if (x >= buttonX(i) && x < buttonX(i) + BW && y >= buttonY(i) && y < buttonY(i) + BH) hit = i;
  bool inside = x >= MX && x < MX + MW && y >= MY && y < MY + MH;
  if (hit < 0) { if (!inside) { menuOpen = false; draw(); } return; }
  if (!menuEnabled(hit)) return;
  menuOpen = false;
  switch (hit) {
    case 0: undoStep(); break;
    case 1: newGame(); break;
    case 2: takeBack(); break;
    case 3: hint(); break;
    case 4: openSettle(false); break;
    case 5: scoreYou = scoreCpu = 0; snprintf(msg, sizeof msg, "Score reset"); break;
    case 6: calibrate(); break;
    case 7: openSettle(true); break;
  }
  draw();
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
// Resistive-touch boards calibrate on the board (4 corners) and keep it in NVS;
// capacitive ones (BOARD_TOUCH_CALIBRATION 0) need nothing.
static void calibrate() {
  if (!BOARD_TOUCH_CALIBRATION) return;
  uint16_t cal[8];
  touch_calibrate(cal);
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
  tapAt = millis();
  if (menuOpen) { menuTap(x, y); return; }
  if (y < SB + ss(4) && x < sx(40)) { menuOpen = true; sel = -1; npaths = 0; draw(); return; }
  if (phase == OVER) {  // only NEW GAME (the tray) starts the next game
    if (x >= TRX && y >= MIDY - TAP_H && y <= MIDY + TAP_D) { newGame(); draw(); }
    return;
  }
  if (phase == PASS) {  // no legal move: the dice hand over, as for DONE
    if (x >= TRX && y >= MIDY - TAP_H && y <= MIDY + TAP_D) { endHumanTurn(); draw(); }
    else { snprintf(msg, sizeof msg, "No legal move: tap the dice"); draw(); }
    return;
  }
  if (phase == OFFER) {
    int i = dlgHit(x, y);
    if (i == 1) gameOver(0, false);  // drop: the CPU wins the current cube
    else if (i == 0) { cubeVal *= 2; cubeOwn = 1; cpuTurn(random(1, 7), random(1, 7)); }
    if (i >= 0) draw();
    return;
  }
  if (phase == RESIGN) {
    int i = dlgHit(x, y);
    if (i == 3) { phase = (Phase)resumePhase; snprintf(msg, sizeof msg, "Play on"); }
    else if (i >= 0) settleOffer(i + 1);
    if (i >= 0) draw();
    return;
  }
  if (phase == COUNTER) {  // the CPU's counter-offer: take it, or play on
    int i = dlgHit(x, y);
    if (i == 0) gameOver(counterLevel, claiming, claiming ? "CPU conceded" : "you resigned");
    else if (i == 1) { phase = (Phase)resumePhase; snprintf(msg, sizeof msg, "Play on"); }
    if (i >= 0) draw();
    return;
  }
  if (phase == RESOFFER) {
    int i = dlgHit(x, y);
    if (i == 0) gameOver(cpuResignLevel, true, "CPU resigned");
    else if (i == 1) { cpuResignRefused = true; cpuTurn(random(1, 7), random(1, 7)); }
    if (i >= 0) draw();
    return;
  }
  if (phase == ROLL) {
    // Tap the cube to double.
    if (x >= BARX - ss(4) && x < BARX + BARW + ss(4) && abs(y - (cubeY() + CUBE_S / 2)) < CUBE_S) {
      if (canDouble()) { humanDouble(); draw(); }
      return;
    }
    if (x >= TRX && y >= MIDY - TAP_H && y <= MIDY + TAP_D) {
      cpuMarks = false;
      int d1 = random(1, 7), d2 = random(1, 7);
      tumble(d1, d2, false);
      startMove(d1, d2);
      if (phase == MOVE) snprintf(msg, sizeof msg, "Your move: %d-%d", dice[0], dice[1]);
      draw();
    }
    return;
  }
  if (phase == DONE) {  // only the dice do anything now: hand over
    if (x >= TRX && y >= MIDY - TAP_H && y <= MIDY + TAP_D) { endHumanTurn(); draw(); }
    else { snprintf(msg, sizeof msg, "Tap the dice to finish"); draw(); }
    return;
  }
  // MOVE (undo is in the menu)
  if (y < FT) return;  // status bar / frame: ignore
  hintMarks = false;
  int s = hitSpot(x, y);
  // Forgiving hit tests (resistive touch is a few px off): the nearest target
  // marker within reach, else the nearest movable checker column.
  int tgt = -1, src = -1;
  bool sIsTarget = false;
  for (int i = 0; i < npaths; i++) sIsTarget |= paths[i].to == s;
  bool switching = s > 0 && s != sel && canMoveFrom(s) && !sIsTarget;  // picking another checker
  if (sel > 0 && !switching) {
    int best = ss(28) * ss(28);
    for (int i = 0; i < npaths; i++) {
      int tx, ty; targetXY(paths[i].to, tx, ty);
      int d = (tx - x) * (tx - x) + (ty - y) * (ty - y);
      if (paths[i].to == s) d = 0;  // inside the destination's own column
      if (d < best) { best = d; tgt = i; }
    }
  }
  if (tgt < 0 && y >= FT && y < FB) {
    if (s > 0 && canMoveFrom(s)) src = s;
    else {
      int best = PW / 2 + ss(9);  // up to ~8 px into a neighbouring column
      for (int p = 1; p <= 25; p++) {
        if (!canMoveFrom(p)) continue;
        int cx = p == 25 ? BARX + BARW / 2 : pointCX(p);
        if (p != 25 && (p >= 13) != (y < MIDY)) continue;  // wrong half
        if (abs(cx - x) < best) { best = abs(cx - x); src = p; }
      }
    }
  }
  Serial.printf("tap %d,%d -> spot %d, target %d, source %d (sel %d)\n", x, y, s,
                tgt >= 0 ? paths[tgt].to : -1, src, sel);
  if (tgt >= 0) { applyPath(paths[tgt]); draw(); return; }
  if (src > 0 && src != sel) {
    sel = src;
    computePaths();
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
  netOk = net.load(net_bin, BOARD_NET_PLACE);  // first: no-PSRAM boards need its 32 KB chunks
  display_begin();
  cv.setColorDepth(16);
  cv.setPsram(BOARD_SPRITE_PSRAM);  // PSRAM boards: keep internal SRAM for the net and the display
  if (!cv.createSprite(W, BAND)) Serial.println("sprite alloc failed");
  loadFonts();
  Serial.printf("%s: net %s, free heap %u, largest %u\n", BOARD_NAME, netOk ? "ok" : "LOAD FAILED",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  auto c = [](uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); };
  C_FRAME = c(0x5b, 0x3a, 0x1f); C_FELT = c(0x1e, 0x5a, 0x38);
  C_PTA = c(0xdc, 0xc9, 0xa0);  C_PTB = c(0xa3, 0x39, 0x2b);
  C_ME = c(0xf3, 0xee, 0xe0);   C_MERIM = c(0x8d, 0x86, 0x76); C_MEIN = c(0xd6, 0xcf, 0xbd);
  C_OP = c(0x26, 0x26, 0x26);   C_OPRIM = c(0xa0, 0xa0, 0xa0); C_OPIN = c(0x3c, 0x3c, 0x3c);
  C_OPHI = c(0x3e, 0x3e, 0x3e);  C_MEHI = c(0xfb, 0xf8, 0xf0);  // faint highlights
  C_BAR = c(0x4a, 0x2f, 0x18);  C_TRAY = c(0x17, 0x3f, 0x28);  C_SEL = c(0xff, 0xd2, 0x3c);
  C_TEXT = c(0xe8, 0xe2, 0xd2); C_DIM = c(0x8a, 0x8a, 0x8a);
  C_GOOD = c(0x7f, 0xe0, 0x8a); C_BAD = c(0xff, 0x8a, 0x7a); C_STATUS = c(0x10, 0x10, 0x10);
  C_CPU = c(0xff, 0x9a, 0x3c);    C_HINT = c(0x4c, 0xd6, 0xf0);
  makeTriangle();
  makeFace(faceMe, true);
  makeFace(faceOp, false);
  randomSeed(esp_random());
  prefs.begin("bg", false);
  uint16_t cal[8];
  if (prefs.getBytes("cal", cal, sizeof cal) == sizeof cal) touch_set_calibration(cal);
  else calibrate();
  if (!netOk) {
    for (oy = 0; oy < H; oy += BAND) {  // a red screen saying so
      cv.fillScreen(TFT_RED);
      cv.setFont(&F_B21); cv.setTextColor(TFT_WHITE); cv.setTextDatum(middle_center);
      cv.drawString("Net failed to load", W / 2, H / 2 - oy);
      display_push(cv, oy, 0, W);
    }
    return;
  }
  newGame();
  draw();
}

void loop() {
  // Wink the prompt you're expected to act on (~2 Hz), redrawing only it.
  static uint32_t lastBlink = 0;
  if (netOk && !menuOpen && millis() - lastBlink > 450 && (phase == ROLL || phase == PASS || phase == OVER || phase == DONE)) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    redrawRegion(TRX, MIDY - TAP_H, W, MIDY + TAP_D + ss(14));
  } else if (phase == MOVE || phase == OFFER) blinkOn = true;
  // Capacitive touch reports a clean position at once: act on the press.
  // Resistive touch: the first samples of a press are unreliable, so collect
  // the whole press and act on release at the median position.
  static int16_t sx[64], sy[64];
  static int ns = 0, idle = 0;
  static bool held;  // capacitive: this press has been handled
  int32_t x, y;
  bool down = touch_get(&x, &y);
  if (!BOARD_TOUCH_CALIBRATION) {
    if (down && !held) {
      held = true;
      uint32_t t0 = millis();
      Phase before = phase;
      if (netOk) tap(x, y);
      if (touchLog) Serial.printf("touch %d,%d phase %d -> %d, handled in %u ms\n", x, y, before, phase, millis() - t0);
      if (!menuOpen) display_cross(x, y);
    } else if (!down) held = false;
  } else if (down) {
    if (ns < 64) { sx[ns] = x; sy[ns] = y; ns++; }
    idle = 0;
  } else if (ns && ++idle >= 3) {  // ~30 ms without contact = released
    int skip = ns > 4 ? 2 : 0, m = ns - skip;
    std::sort(sx + skip, sx + ns);
    std::sort(sy + skip, sy + ns);
    int tx = sx[skip + m / 2], ty = sy[skip + m / 2];
    uint32_t t0 = millis();
    Phase before = phase;
    if (netOk) tap(tx, ty);
    if (touchLog) Serial.printf("touch %d,%d (%d samples) phase %d -> %d, handled in %u ms\n", tx, ty, ns, before, phase, millis() - t0);
    // Where the screen thinks you touched (gone at the next redraw).
    if (!menuOpen) {
      display_cross(tx, ty);
    }
    ns = 0;
  }
  if (Serial.available()) {
    int ch = Serial.read();
    if (!netOk && ch != 'd' && ch != 'k') ch = 0;  // nothing that evaluates without a net
    if (ch == 'v') verify();
    else if (ch == 'n') { newGame(); draw(); }
    else if (ch == 'L') { touchLog = !touchLog; Serial.printf("touch log %s\n", touchLog ? "on" : "off"); }
    else if (ch == 'T') {  // touch test: print the raw readings for 10 s
      uint32_t t0 = millis(); bool was = false;
      while (millis() - t0 < 10000) {
        int32_t tx, ty; bool d = touch_get(&tx, &ty);
        if (d != was) { Serial.printf("touch %s %d,%d\n", d ? "down" : "up", tx, ty); was = d; }
        delay(10);
      }
      Serial.println("touch test done");
    }
    else if (ch == 'i')  // info (USB-native boards lose the boot log)
      Serial.printf("%s %dx%d: net %s, band sprite %s, free internal %u (largest %u), PSRAM %u\n", BOARD_NAME, W, H,
                    netOk ? "ok" : "FAILED", cv.getBuffer() ? "ok" : "MISSING",
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    else if (ch == 'q') {
      // You're about to be gammoned (the CPU has 2 left, you have none off):
      // resigning a single should be refused, a gammon accepted.
      memset(&g, 0, sizeof g);
      g.pts[24] = -2; g.off[1] = 13; g.pts[6] = 15;
      cubeVal = 1; cubeOwn = 0; nhist = 0; dice[0] = dice[1] = 0;
      cpuMarks = hintMarks = false;
      setEval();
      phase = ROLL; snprintf(msg, sizeof msg, "Test: about to be gammoned");
      draw();
    }
    else if (ch == 'r') {
      // CPU-resign test: you have 2 left on the ace point, the CPU hasn't borne
      // any off, and your turn has just ended - the CPU should resign a gammon.
      memset(&g, 0, sizeof g);
      g.pts[1] = 2; g.off[0] = 13; g.pts[19] = -15;
      cubeVal = 1; cubeOwn = 0; nhist = 0; dice[0] = dice[1] = 0;
      cpuMarks = hintMarks = false; cpuResignRefused = false;
      setEval();
      endHumanTurn();
      draw();
    }
    else if (ch == 'p') {
      // Pass test: you on the bar against a closed board - every roll dances.
      memset(&g, 0, sizeof g);
      g.bar[0] = 1; g.pts[6] = 14;
      for (int q = 19; q <= 24; q++) g.pts[q] = -2;
      g.pts[1] = -3;
      cubeVal = 1; cubeOwn = 0; nhist = 0; dice[0] = dice[1] = 0;
      cpuMarks = hintMarks = false;
      setEval();
      phase = ROLL; snprintf(msg, sizeof msg, "Test: closed out");
      draw();
    }
    else if (ch == 'g' || ch == 'h') {
      // Cube tests on a race. 'g': CPU 60 pips vs your 75, your turn just
      // ended (the CPU decides whether to double). 'h': the reverse, you on roll.
      memset(&g, 0, sizeof g);
      bool cpuAhead = ch == 'g';
      int a[3] = {4, 5, 6}, b[3] = {3, 4, 5};   // 75 and 60 pips
      for (int i = 0; i < 3; i++) {
        g.pts[cpuAhead ? a[i] : b[i]] = 5;           // you
        g.pts[25 - (cpuAhead ? b[i] : a[i])] = -5;   // CPU (its point k is your 25-k)
      }
      cubeVal = 1; cubeOwn = 0; nhist = 0; dice[0] = dice[1] = 0;
      cpuMarks = hintMarks = false;
      setEval();
      if (cpuAhead) endHumanTurn();
      else { phase = ROLL; snprintf(msg, sizeof msg, "Test: you lead 60-75"); }
      draw();
    }
    else if (ch == 'a') {
      // Self-play: the net plays both sides for N exchanges (yours, then the CPU's) with
      // random dice, the last CPU move shown as usual - real-game positions for screenshots.
      int n = Serial.parseInt();
      for (int i = 0; i < n && phase != OVER; i++) {
        int d1 = phase == MOVE ? dice[0] : random(1, 7), d2 = phase == MOVE ? dice[1] : random(1, 7);
        int k = bg_genmoves(g, d1, d2, kids, 1024);
        g = kids[bg_best(net, kids, k)];
        if (int r = bg_result(g)) { gameOver(r, true); break; }
        if (i < n - 1) {
          BgBoard me = bg_swap(g);
          int m = bg_genmoves(me, random(1, 7), random(1, 7), kids, 1024);
          BgBoard nb = kids[bg_best(net, kids, m)];
          g = bg_swap(nb);
          if (int r = bg_result(nb)) { gameOver(r, false); break; }
          phase = ROLL;
        } else {
          cpuTurn(random(1, 7), random(1, 7));
        }
      }
      nhist = 0; hintMarks = false;
      draw();
    }
    else if (ch == 'd') dumpFrame();
    else if (ch == 'k') { calibrate(); draw(); }
    else if (ch == 't') { int tx = Serial.parseInt(), ty = Serial.parseInt(); tap(tx, ty); Serial.printf("tap %d %d phase=%d sel=%d\n", tx, ty, phase, sel); }
  }
  delay(10);
}
