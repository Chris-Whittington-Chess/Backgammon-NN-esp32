#include "bg.h"
#include <string.h>

BgBoard bg_start() {
  BgBoard b;
  memset(&b, 0, sizeof b);
  b.pts[24] = 2; b.pts[13] = 5; b.pts[8] = 3; b.pts[6] = 5;
  b.pts[1] = -2; b.pts[12] = -5; b.pts[17] = -3; b.pts[19] = -5;
  return b;
}

BgBoard bg_swap(const BgBoard& b) {
  BgBoard s;
  s.pts[0] = 0;
  for (int i = 1; i <= 24; i++) s.pts[i] = -b.pts[25 - i];
  s.bar[0] = b.bar[1]; s.bar[1] = b.bar[0];
  s.off[0] = b.off[1]; s.off[1] = b.off[0];
  return s;
}

static int win_points(const BgBoard& b, int winner) {
  int loser = 1 - winner;
  if (b.off[loser] > 0) return 1;
  bool bg = b.bar[loser] > 0;
  for (int p = 1; p <= 6 && !bg; p++)
    bg = winner == 0 ? b.pts[p] < 0 : b.pts[25 - p] > 0;
  return bg ? 3 : 2;
}

int bg_result(const BgBoard& b) {
  if (b.off[0] == 15) return win_points(b, 0);
  if (b.off[1] == 15) return -win_points(b, 1);
  return 0;
}

uint32_t bg_hash(const BgBoard& b) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint8_t v) { h ^= v; h *= 16777619u; };
  for (int p = 1; p <= 24; p++) mix((uint8_t)b.pts[p]);
  mix(b.bar[0]); mix(b.bar[1]); mix(b.off[0]); mix(b.off[1]);
  return h;
}

// ---- single-die moves (for_each_single) ----
struct Single { int8_t from, to; BgBoard b; };

static bool all_home(const BgBoard& b) {
  if (b.bar[0]) return false;
  for (int p = 7; p <= 24; p++) if (b.pts[p] > 0) return false;
  return true;
}
static int highest(const BgBoard& b) {
  for (int p = 24; p >= 1; p--) if (b.pts[p] > 0) return p;
  return 0;
}
static void land(BgBoard& c, int to) {
  if (c.pts[to] == -1) { c.pts[to] = 0; c.bar[1]++; }
  c.pts[to]++;
}

// Calls f(from, to, child) for each legal use of `die`. from 25 = bar, to 0 = off.
template <class F>
static void for_each_single(const BgBoard& b, int die, F f) {
  if (b.bar[0]) {
    int to = 25 - die;
    if (b.pts[to] > -2) { BgBoard c = b; c.bar[0]--; land(c, to); f(25, to, c); }
    return;
  }
  bool home = all_home(b);
  int hi = highest(b);
  for (int p = 1; p <= 24; p++) {
    if (b.pts[p] <= 0) continue;
    int d = p - die;
    if (d >= 1) {
      if (b.pts[d] > -2) { BgBoard c = b; c.pts[p]--; land(c, d); f(p, d, c); }
    } else if (home && (d == 0 || p == hi)) {
      BgBoard c = b; c.pts[p]--; c.off[0]++; f(p, 0, c);
    }
  }
}

// ---- dice as a count multiset (DiceBag) ----
struct Bag { uint8_t c[7]; int total; };

static int max_used(const BgBoard& b, const Bag& bag) {
  int best = 0;
  for (int d = 1; d <= 6 && best < bag.total; d++) {
    if (!bag.c[d]) continue;
    Bag b2 = bag; b2.c[d]--; b2.total--;
    for_each_single(b, d, [&](int, int, const BgBoard& c) {
      if (best == bag.total) return;  // can't do better than every die
      int u = 1 + max_used(c, b2);
      if (u > best) best = u;
    });
  }
  return best;
}

// Unique-board collector: open-addressed hash of indices into `out`.
struct Collect {
  BgBoard* out; int n, cap;
  uint16_t slot[2048];
  void init(BgBoard* o, int c) { out = o; n = 0; cap = c; memset(slot, 0xff, sizeof slot); }
  void add(const BgBoard& b) {
    uint32_t h = bg_hash(b);
    for (uint32_t i = h & 2047;; i = (i + 1) & 2047) {
      if (slot[i] == 0xffff) {
        if (n >= cap) return;
        slot[i] = n; out[n++] = b;
        return;
      }
      if (!memcmp(&out[slot[i]], &b, sizeof b)) return;
    }
  }
};

static void emit(const BgBoard& b, const Bag& bag, int left, Collect& col) {
  if (!left) { col.add(b); return; }
  for (int d = 1; d <= 6; d++) {
    if (!bag.c[d]) continue;
    Bag b2 = bag; b2.c[d]--; b2.total--;
    for_each_single(b, d, [&](int, int, const BgBoard& c) { emit(c, b2, left - 1, col); });
  }
}

static Bag make_bag(int d1, int d2) {
  Bag g; memset(&g, 0, sizeof g);
  if (d1 == d2) { g.c[d1] = 4; g.total = 4; }
  else { g.c[d1]++; g.c[d2]++; g.total = 2; }
  return g;
}

int bg_genmoves(const BgBoard& b, int d1, int d2, BgBoard* out, int cap) {
  static Collect col;  // 4 KB slot table: keep it off the stack
  col.init(out, cap);
  Bag bag = make_bag(d1, d2);
  int m = max_used(b, bag);
  if (!m) { col.add(b); return col.n; }
  if (m == 1 && d1 != d2) {
    for (int d = 6; d >= 1; d--) {
      if (!bag.c[d]) continue;
      int before = col.n;
      for_each_single(b, d, [&](int, int, const BgBoard& c) { col.add(c); });
      if (col.n > before) break;
    }
  } else {
    emit(b, bag, m, col);
  }
  return col.n;
}

int bg_next_submoves(const BgBoard& b, const int* dice, int n, BgSub* out, int cap) {
  Bag bag; memset(&bag, 0, sizeof bag);
  for (int i = 0; i < n; i++) { bag.c[dice[i]]++; bag.total++; }
  int m = max_used(b, bag);
  if (!m) return 0;
  int k = 0;
  for (int d = 1; d <= 6; d++) {
    if (!bag.c[d]) continue;
    Bag b2 = bag; b2.c[d]--; b2.total--;
    for_each_single(b, d, [&](int from, int to, const BgBoard& c) {
      if (k < cap && 1 + max_used(c, b2) == m) out[k++] = BgSub{(int8_t)from, (int8_t)to, (int8_t)d, c};
    });
  }
  // Larger-die rule: if only one die can be played and the dice differ.
  if (m == 1) {
    int distinct = 0, larger = 0;
    for (int d = 1; d <= 6; d++) if (bag.c[d]) { distinct++; larger = d; }
    if (distinct >= 2) {
      bool any = false;
      for (int i = 0; i < k; i++) any |= out[i].die == larger;
      if (any) {
        int j = 0;
        for (int i = 0; i < k; i++) if (out[i].die == larger) out[j++] = out[i];
        k = j;
      }
    }
  }
  return k;
}
