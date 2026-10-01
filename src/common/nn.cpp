#include "nn.h"
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>

static const int RACE_EDGES[2] = {57, 96};
static const int CRASHED_EDGES[1] = {127};
static const int CONTACT_EDGES[6] = {168, 202, 232, 259, 284, 309};

int bg_pip_count(const BgBoard& b, int side) {
  int s = 0;
  for (int p = 1; p <= 24; p++) {
    int c = b.pts[p];
    if (side == 0 && c > 0) s += c * p;
    if (side == 1 && c < 0) s += -c * (25 - p);
  }
  return s + b.bar[side] * 25;
}

static bool no_contact(const BgBoard& b) {
  if (b.bar[0] || b.bar[1]) return false;
  int m = 0, o = 0;
  for (int p = 24; p >= 1; p--) if (b.pts[p] > 0) { m = p; break; }
  for (int p = 1; p <= 24; p++) if (b.pts[p] < 0) { o = p; break; }
  if (!m || !o) return true;
  return m < o;
}

// gnubg ClassifyPosition "crashed" test, N = 6 (board.rs Board::crashed).
static bool crashed(const BgBoard& b) {
  const int N = 6;
  for (int side = 0; side < 2; side++) {
    int tot = 15 - b.off[side];
    int ace = side == 0 ? (b.pts[1] > 0 ? b.pts[1] : 0) : (b.pts[24] < 0 ? -b.pts[24] : 0);
    int two = side == 0 ? (b.pts[2] > 0 ? b.pts[2] : 0) : (b.pts[23] < 0 ? -b.pts[23] : 0);
    bool c;
    if (tot <= N) c = true;
    else if (ace > 1) c = tot <= N + ace || (two > 1 && (1 + tot - ace - two) <= N);
    else c = tot <= N + (two - 1);
    if (c) return true;
  }
  return false;
}

int bg_route_bucket(const BgBoard& b) {
  int total = bg_pip_count(b, 0) + bg_pip_count(b, 1);
  auto sub = [total](const int* e, int n) { int k = 0; for (int i = 0; i < n; i++) k += total >= e[i]; return k; };
  if (no_contact(b)) return sub(RACE_EDGES, 2);
  if (crashed(b)) return 3 + sub(CRASHED_EDGES, 1);
  return 5 + sub(CONTACT_EDGES, 6);
}

float bg_equity(const float p[6]) {
  return p[0] + 2 * p[1] + 3 * p[2] - p[3] - 2 * p[4] - 3 * p[5];
}

static float* copy_to(const float* src, size_t n, uint32_t caps) {
  float* d = (float*)heap_caps_malloc(n * sizeof(float), caps);
  if (d) memcpy(d, src, n * sizeof(float));
  return d;
}

void BgNet::chunk_w2() {
  for (int i = 0; i < 4; i++) W2c[i] = W2T + (size_t)i * (h1 / 4) * h2;
}

bool BgNet::load(const uint8_t* blob, NetPlace where) {
  if (memcmp(blob, "BGN1", 4)) return false;
  uint32_t hdr[4];
  memcpy(hdr, blob + 4, sizeof hdr);
  n_in = hdr[0]; h1 = hdr[1]; h2 = hdr[2]; n_heads = hdr[3];
  const float* f = (const float*)(blob + 20);
  const float* s[6];
  size_t n[6] = {(size_t)n_in * h1, (size_t)h1, (size_t)h1 * h2, (size_t)h2,
                 (size_t)n_heads * 6 * h2, (size_t)n_heads * 6};
  for (int i = 0; i < 6; i++) { s[i] = f; f += n[i]; }
  if (where == NET_FLASH) {
    if ((uintptr_t)blob & 3) return false;
    W1T = s[0]; b1 = s[1]; W2T = s[2]; b2 = s[3]; WH = s[4]; bH = s[5];
    chunk_w2();
    return true;
  }
  if (where == NET_FLASH_SRAM) {
    if ((uintptr_t)blob & 3) return false;
    const uint32_t IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t q = (size_t)(h1 / 4) * h2;  // 32 KB each: fits a fragmented heap
    W1T = s[0]; b1 = s[1]; W2T = s[2]; b2 = s[3]; WH = s[4]; bH = s[5];
    for (int i = 0; i < 4; i++)
      if (!(W2c[i] = copy_to(s[2] + i * q, q, IN))) return false;
    return true;
  }
  const uint32_t PS = MALLOC_CAP_SPIRAM, IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  // INTERNAL: only the dense layer-2 matrix (128 KB, read in full every eval)
  // goes on-chip; layer 1 and the heads are touched sparsely, so PSRAM is fine.
  W1T = copy_to(s[0], n[0], PS);
  b1 = copy_to(s[1], n[1], PS);
  W2T = copy_to(s[2], n[2], where == NET_INTERNAL ? IN : PS);
  b2 = copy_to(s[3], n[3], PS);
  WH = copy_to(s[4], n[4], PS);
  bH = copy_to(s[5], n[5], PS);
  if (W2T) chunk_w2();
  return W1T && b1 && W2T && b2 && WH && bH;
}

// Quantize an input-major [rows][cols] float matrix to int8 with one scale per
// column (= per output unit), so the scale factors out of each accumulation.
static int8_t* quant_cols(const float* w, int rows, int cols, float** scale_out) {
  const uint32_t IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  int8_t* q = (int8_t*)heap_caps_malloc((size_t)rows * cols, IN);
  float* s = (float*)heap_caps_malloc(cols * sizeof(float), IN);
  if (!q || !s) return nullptr;
  for (int j = 0; j < cols; j++) {
    float m = 0;
    for (int i = 0; i < rows; i++) m = fmaxf(m, fabsf(w[(size_t)i * cols + j]));
    s[j] = m > 0 ? m / 127.0f : 1.0f;
    for (int i = 0; i < rows; i++) q[(size_t)i * cols + j] = (int8_t)lrintf(w[(size_t)i * cols + j] / s[j]);
  }
  *scale_out = s;
  return q;
}

bool BgNetQ::build(const BgNet& f) {
  h1 = f.h1; h2 = f.h2;
  float *a, *c;
  W1T = quant_cols(f.W1T, f.n_in, h1, &a);
  W2T = quant_cols(f.W2T, h1, h2, &c);
  s1 = a; s2 = c;
  b1 = f.b1; b2 = f.b2; WH = f.WH; bH = f.bH;
  return W1T && W2T;
}

void BgNetQ::eval(const BgBoard& b, float probs[6], int* route_out) const {
  int32_t acc1[256], acc2[128];
  int16_t x1[256];
  memset(acc1, 0, h1 * sizeof(int32_t));
  // Inputs in 1/30 fixed point: 1 -> 30, (n-3)/2 -> 15(n-3), bar/2 -> 15 bar, off/15 -> 2 off.
  auto add = [&](int i, int x) {
    const int8_t* w = W1T + (size_t)i * h1;
    for (int j = 0; j < h1; j++) acc1[j] += x * w[j];
  };
  for (int p = 1; p <= 24; p++) {
    int c = b.pts[p];
    if (!c) continue;
    int base = (c > 0 ? 0 : 96) + (p - 1) * 4, n = c > 0 ? c : -c;
    add(base, 30);
    if (n >= 2) add(base + 1, 30);
    if (n >= 3) add(base + 2, 30);
    if (n > 3) add(base + 3, 15 * (n - 3));
  }
  if (b.bar[0]) add(192, 15 * b.bar[0]);
  if (b.bar[1]) add(193, 15 * b.bar[1]);
  if (b.off[0]) add(194, 2 * b.off[0]);
  if (b.off[1]) add(195, 2 * b.off[1]);
  add(196, 30);

  // Layer-1 activations -> ReLU -> int16 with one per-eval scale.
  float a1[256], amax = 0;
  for (int j = 0; j < h1; j++) {
    float v = b1[j] + acc1[j] * (s1[j] / 30.0f);
    a1[j] = v > 0 ? v : 0;
    amax = fmaxf(amax, a1[j]);
  }
  float sa = amax > 0 ? amax / 32767.0f : 1.0f, inv = 1.0f / sa;
  for (int j = 0; j < h1; j++) x1[j] = (int16_t)lrintf(a1[j] * inv);

  memset(acc2, 0, h2 * sizeof(int32_t));
  for (int k = 0; k < h1; k++) {
    int x = x1[k];
    if (!x) continue;
    const int8_t* w = W2T + (size_t)k * h2;
    for (int j = 0; j < h2; j++) acc2[j] += x * w[j];
  }
  float a2[128];
  for (int j = 0; j < h2; j++) {
    float v = b2[j] + acc2[j] * (sa * s2[j]);
    a2[j] = v > 0 ? v : 0;
  }

  int r = bg_route_bucket(b);
  if (route_out) *route_out = r;
  float z[6], mx = -1e30f;
  for (int o = 0; o < 6; o++) {
    const float* w = WH + (size_t)(r * 6 + o) * h2;
    float s = bH[r * 6 + o];
    for (int j = 0; j < h2; j++) s += w[j] * a2[j];
    z[o] = s;
    if (s > mx) mx = s;
  }
  float sum = 0;
  for (int o = 0; o < 6; o++) { z[o] = expf(z[o] - mx); sum += z[o]; }
  for (int o = 0; o < 6; o++) probs[o] = z[o] / sum;
}

void BgNet::eval(const BgBoard& b, float probs[6], int* route_out) const {
  float a1[256], a2[128];
  memcpy(a1, b1, h1 * sizeof(float));
  auto add_row = [&](int i, float x) {
    const float* w = W1T + (size_t)i * h1;
    for (int j = 0; j < h1; j++) a1[j] += x * w[j];
  };
  auto add_unit = [&](int i) {
    const float* w = W1T + (size_t)i * h1;
    for (int j = 0; j < h1; j++) a1[j] += w[j];
  };
  // Sparse 198-input encoding (features.rs): 4 units per point per side.
  for (int p = 1; p <= 24; p++) {
    int c = b.pts[p];
    if (!c) continue;
    int base = (c > 0 ? 0 : 96) + (p - 1) * 4, n = c > 0 ? c : -c;
    add_unit(base);
    if (n >= 2) add_unit(base + 1);
    if (n >= 3) add_unit(base + 2);
    if (n > 3) add_row(base + 3, (n - 3) * 0.5f);
  }
  if (b.bar[0]) add_row(192, b.bar[0] * 0.5f);
  if (b.bar[1]) add_row(193, b.bar[1] * 0.5f);
  if (b.off[0]) add_row(194, b.off[0] / 15.0f);
  if (b.off[1]) add_row(195, b.off[1] / 15.0f);
  add_unit(196);  // side to move one-hot (always the mover)

  // Layer 2, input-major so ReLU-zero units are skipped outright.
  const int q4 = h1 / 4;
  memcpy(a2, b2, h2 * sizeof(float));
  for (int k = 0; k < h1; k++) {
    float x = a1[k];
    if (x <= 0) continue;
    const float* w = W2c[k / q4] + (size_t)(k % q4) * h2;
    for (int j = 0; j < h2; j++) a2[j] += x * w[j];
  }
  for (int j = 0; j < h2; j++) if (a2[j] < 0) a2[j] = 0;

  int r = bg_route_bucket(b);
  if (route_out) *route_out = r;
  float z[6], mx = -1e30f;
  for (int o = 0; o < 6; o++) {
    const float* w = WH + (size_t)(r * 6 + o) * h2;
    float s = bH[r * 6 + o];
    for (int j = 0; j < h2; j++) s += w[j] * a2[j];
    z[o] = s;
    if (s > mx) mx = s;
  }
  float sum = 0;
  for (int o = 0; o < 6; o++) { z[o] = expf(z[o] - mx); sum += z[o]; }
  for (int o = 0; o < 6; o++) probs[o] = z[o] / sum;
}
