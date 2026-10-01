// Backgammon rules and move generation, ported from bgcore (board.rs,
// moves.rs, game.rs). Boards are mover-relative like BgBoard in nn.h.
#pragma once
#include "nn.h"

// Resulting boards of every legal full turn (maximal dice use, larger-die rule,
// de-duplicated) - the set bgcore's genmoves / genmoves_playout produce. A
// dance yields one board equal to the input. Returns the count (<= cap).
int bg_genmoves(const BgBoard& b, int d1, int d2, BgBoard* out, int cap);

// One legal next checker move inside a partially played turn (next_submoves).
struct BgSub {
  int8_t from;  // 1..24, or 25 = bar
  int8_t to;    // 1..24, or 0 = off
  int8_t die;
  BgBoard result;
};
// dice[0..n) are the dice still to play. Returns the count; 0 = turn over.
int bg_next_submoves(const BgBoard& b, const int* dice, int n, BgSub* out, int cap);

BgBoard bg_swap(const BgBoard& b);
BgBoard bg_start();
// +points if the mover has borne everything off (1/2/3), -points if the
// opponent has, 0 if the game is still on.
int bg_result(const BgBoard& b);
uint32_t bg_hash(const BgBoard& b);  // FNV-1a over points[1..24], bar, off

// ---- doubling cube (money play, Janowski's cubeful model) ----
// p: the net's 6 outcome probabilities for the player ON ROLL (pre-roll).
// own: cube owner from that player's view: +1 owns it, 0 centred, -1 opponent.
// x: cube efficiency (0 = dead cube, 1 = fully live); ~0.68 is the usual pick.
// Equities are per unit of the current cube value.
float bg_cubeful(const float p[6], int own, float x);
struct BgCubeCall {
  float nd, dt, dp;   // no double, double/take, double/pass (doubler's view)
  bool dbl, take;     // doubler should double; opponent should take
};
// The decision for the player on roll, who may double (own >= 0).
BgCubeCall bg_cube_call(const float p[6], int own, float x);

// 0-ply choice (game.rs EvalEngine): the child maximising the mover's score,
// where a won child scores its points and any other -equity(net(swap(child))).
template <class Net>
int bg_best(const Net& net, const BgBoard* kids, int n, float* score_out = nullptr) {
  int best = 0;
  float bs = -1e30f;
  for (int i = 0; i < n; i++) {
    int r = bg_result(kids[i]);
    float s;
    if (r > 0) s = r;
    else {
      float p[6];
      net.eval(bg_swap(kids[i]), p);
      s = -bg_equity(p);
    }
    if (s > bs) { bs = s; best = i; }
  }
  if (score_out) *score_out = bs;
  return best;
}
