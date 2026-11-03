// Change log for plug history: records only what changed (power level, relay,
// reachability), so 2 s polling costs bytes per change, not per sample. Free
// of Arduino types so it can be tested on a computer (tools/test_histlog.cpp).
//
// Each event is varint((dt << 2) | kind), plus varint(watts) for HIST_W. dt is
// seconds since the previous event, or since the log's start time. The web
// app decodes this in docs/history.js.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

enum : uint8_t { HIST_W = 0, HIST_OFF = 1, HIST_ON = 2, HIST_GONE = 3 };

static const int HIST_DRAW_W = 3;  // above this the load counts as drawing

struct HistState {
  bool ok = false, on = false;
  int w = -1;  // last recorded watts; -1 = none yet (or the plug has no meter)
};

class HistLog {
 public:
  std::vector<uint8_t> buf;
  HistState start, cur;  // state at the log's start time, and now
  uint32_t t0 = 0, last = 0;

  void begin(uint32_t t) {
    buf.clear();
    t0 = last = t;
    start = cur;
  }

  // One poll. ok=false: the plug stopped answering. watts < 0: no meter.
  void sample(uint32_t t, bool ok, bool on, float watts) {
    if (!ok) {
      if (cur.ok) put(t, HIST_GONE);
      cur.ok = false;
      return;
    }
    bool resumed = !cur.ok;  // after a gap, restate everything
    cur.ok = true;
    if (resumed || on != cur.on) put(t, on ? HIST_ON : HIST_OFF);
    cur.on = on;
    if (watts < 0) return;
    int w = (int)lroundf(watts);
    if (resumed || cur.w < 0 || changed(cur.w, w)) {
      put(t, HIST_W, w);
      cur.w = w;
    }
  }

  // Starting or stopping drawing always counts; otherwise >5 W and >10%.
  static bool changed(int a, int b) {
    if ((a > HIST_DRAW_W) != (b > HIST_DRAW_W)) return true;
    int d = abs(a - b);
    return d > 5 && d * 10 > a;
  }

 private:
  void varint(uint32_t v) {
    while (v >= 0x80) {
      buf.push_back((uint8_t)(v | 0x80));
      v >>= 7;
    }
    buf.push_back((uint8_t)v);
  }
  void put(uint32_t t, uint8_t kind, int w = 0) {
    uint32_t dt = t > last ? t - last : 0;
    last = t > last ? t : last;
    varint(dt << 2 | kind);
    if (kind == HIST_W) varint((uint32_t)(w < 0 ? 0 : w));
  }
};
