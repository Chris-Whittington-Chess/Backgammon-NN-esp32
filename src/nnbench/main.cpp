// On-device parity + timing for the Backgammon-NN value net.
// Runs at boot (results on screen) and again on serial 'b' (results on serial).
#include <M5Unified.h>
#include <esp_timer.h>
#include "../common/nn.h"

extern const uint8_t net_bin[] asm("_binary_data_net_bin_start");
extern const uint8_t tests_bin[] asm("_binary_data_tests_bin_start");

struct Test { BgBoard b; int route, kids; float probs[6]; };
static Test* tests;
static int ntests;
static String report;

static void loadTests() {
  memcpy(&ntests, tests_bin, 4);
  tests = (Test*)heap_caps_malloc(ntests * sizeof(Test), MALLOC_CAP_SPIRAM);
  for (int i = 0; i < ntests; i++) {
    const uint8_t* r = tests_bin + 4 + i * 56;
    Test& t = tests[i];
    memset(&t.b, 0, sizeof t.b);
    for (int p = 0; p < 24; p++) t.b.pts[p + 1] = (int8_t)r[p];
    t.b.bar[0] = r[24]; t.b.bar[1] = r[25]; t.b.off[0] = r[26]; t.b.off[1] = r[27];
    t.route = r[28]; t.kids = r[29];
    memcpy(t.probs, r + 32, 24);
  }
}

static void line(const char* fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  report += buf; report += "\n";
  M5.Display.println(buf);
}

template <class Net>
static void bench(const char* name, const Net& net) {
  float maxp = 0, maxeq = 0, sumeq = 0; int badRoute = 0;
  for (int i = 0; i < ntests; i++) {
    float p[6]; int r;
    net.eval(tests[i].b, p, &r);
    if (r != tests[i].route) badRoute++;
    for (int o = 0; o < 6; o++) maxp = fmaxf(maxp, fabsf(p[o] - tests[i].probs[o]));
    float de = fabsf(bg_equity(p) - bg_equity(tests[i].probs));
    maxeq = fmaxf(maxeq, de);
    sumeq += de;
  }
  const int PASSES = 5;
  float p[6];
  int64_t t0 = esp_timer_get_time();
  for (int k = 0; k < PASSES; k++)
    for (int i = 0; i < ntests; i++) net.eval(tests[i].b, p);
  double us = double(esp_timer_get_time() - t0) / (PASSES * ntests);
  // Cost of a 0-ply move choice = one eval per legal child for that roll.
  long kids = 0; int maxk = 0;
  for (int i = 0; i < ntests; i++) { kids += tests[i].kids; maxk = max(maxk, tests[i].kids); }
  double meanKids = double(kids) / ntests;
  line("%s: %.0f us/eval", name, us);
  line("  move: avg %.1f ms (%.1f kids), worst %.0f ms (%d)", us * meanKids / 1000, meanKids,
       us * maxk / 1000, maxk);
  line("  parity: max|dp| %.1e  |dEq| max %.1e mean %.1e  route miss %d", maxp, maxeq,
       sumeq / ntests, badRoute);
}

static BgNet netFlash, netPsram, netSram;
static BgNetQ netQ;
static bool okFlash, okPsram, okSram, okQ;

static void runAll() {
  report = "";
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  line("CoreS3 %d MHz, free internal %u KB (largest %u), PSRAM %u KB", getCpuFrequencyMhz(),
       heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
       heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  line("%d test positions", ntests);
  if (okFlash) bench("f32 flash", netFlash); else line("f32 flash: load failed");
  if (okPsram) bench("f32 psram", netPsram); else line("f32 psram: load failed");
  if (okSram) bench("f32 sram", netSram); else line("f32 sram: load failed");
  if (okQ) bench("int8 sram", netQ); else line("int8 sram: build failed");
}
void setup() {
  Serial.begin(115200);
  M5.begin(M5.config());
  M5.Display.setRotation(1);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  loadTests();
  okFlash = netFlash.load(net_bin, NET_FLASH);
  okPsram = netPsram.load(net_bin, NET_PSRAM);
  okSram = netSram.load(net_bin, NET_INTERNAL);
  okQ = okPsram && netQ.build(netPsram);
  runAll();
}

void loop() {
  M5.update();
  if (Serial.available() && Serial.read() == 'b') {
    runAll();
    Serial.print(report);
    Serial.println("DONE");
  }
  delay(10);
}
