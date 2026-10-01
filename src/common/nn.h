// Backgammon-NN value net on the ESP32-S3: 198 -> 256 -> 128 -> 12x6, ReLU.
// Mirrors bgcore (features.rs, board.rs route_bucket, eval/nn.rs fold) and the
// weights exported by tools/export_net.py.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct BgBoard {
  int8_t pts[25];  // [1..24], mover-relative: + mover, - opponent
  uint8_t bar[2];  // [mover, opp]
  uint8_t off[2];
};

// NET_FLASH_SRAM: no-PSRAM chips - read the blob in place, but copy the dense
// layer-2 matrix into internal SRAM as two halves (no 128 KB contiguous block).
enum NetPlace { NET_INTERNAL, NET_PSRAM, NET_FLASH, NET_FLASH_SRAM };

struct BgNet {
  int n_in, h1, h2, n_heads;
  const float *W1T, *b1, *W2T, *b2, *WH, *bH;
  const float* W2hi;  // rows h1/2.. of W2T (== W2T + h1/2*h2 unless split)
  // Load from the net.bin blob. INTERNAL puts the dense layer-2 and heads in
  // on-chip SRAM (layer 1 stays in PSRAM); FLASH reads the blob in place.
  bool load(const uint8_t* blob, NetPlace where);
  // Softmax of the routed head: [win s, g, bg, lose s, g, bg].
  void eval(const BgBoard& b, float probs[6], int* route_out = nullptr) const;
};

// Int8-weight version of a loaded BgNet, all in on-chip SRAM. Layers 1-2 run
// in integer arithmetic (int8 weights, per-output-unit scales; inputs in 1/30
// fixed point, layer-1 activations as int16); the routed head stays float.
struct BgNetQ {
  int h1, h2;
  const int8_t *W1T, *W2T;  // same input-major layouts as BgNet
  const float *s1, *s2;      // per-output-unit weight scales
  const float *b1, *b2, *WH, *bH;
  bool build(const BgNet& f);
  void eval(const BgBoard& b, float probs[6], int* route_out = nullptr) const;
};

int bg_pip_count(const BgBoard& b, int side);
int bg_route_bucket(const BgBoard& b);
float bg_equity(const float p[6]);
