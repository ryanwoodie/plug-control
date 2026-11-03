// Host test for firmware/src/histlog.h: simulates heater patterns polled every
// 2 s, decodes the log and checks it against the truth.
//   c++ -std=c++17 -I firmware/src tools/test_histlog.cpp -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <functional>
#include "histlog.h"

struct Ev { uint32_t t; int kind, w; };

static std::vector<Ev> decode(const HistLog &l) {
  std::vector<Ev> out;
  uint32_t t = l.t0;
  size_t i = 0;
  auto varint = [&]() {
    uint32_t v = 0;
    for (int s = 0;; s += 7) {
      uint8_t b = l.buf[i++];
      v |= (uint32_t)(b & 0x7f) << s;
      if (!(b & 0x80)) return v;
    }
  };
  while (i < l.buf.size()) {
    uint32_t v = varint();
    t += v >> 2;
    int kind = v & 3, w = kind == HIST_W ? (int)varint() : -1;
    out.push_back({t, kind, w});
  }
  return out;
}

// Runs truth(t) -> {ok, on, watts} for `secs` at 2 s polls. Returns the log.
using Truth = std::function<void(uint32_t, bool &, bool &, float &)>;
static HistLog run(uint32_t secs, Truth truth) {
  HistLog l;
  l.begin(1000);
  for (uint32_t t = 1000; t < 1000 + secs; t += 2) {
    bool ok, on;
    float w;
    truth(t - 1000, ok, on, w);
    l.sample(t, ok, on, w);
  }
  return l;
}

static int drawStarts(const std::vector<Ev> &ev) {
  int n = 0, prev = 0;
  for (auto &e : ev)
    if (e.kind == HIST_W) {
      if (e.w > HIST_DRAW_W && prev <= HIST_DRAW_W) n++;
      prev = e.w;
    }
  return n;
}

int main() {
  // 1. Thermostat heater: 150 W for 4 min, off 6 min, for an hour, with +-2 W noise.
  HistLog a = run(3600, [](uint32_t t, bool &ok, bool &on, float &w) {
    ok = on = true;
    w = (t % 600 < 240) ? 150 + (int)(t * 7 % 5) - 2 : 0;
  });
  auto ea = decode(a);
  printf("thermostat 4/6 min, 1 h: %zu events, %zu bytes, %d heat cycles\n", ea.size(), a.buf.size(), drawStarts(ea));
  assert(drawStarts(ea) == 6);
  assert(a.buf.size() < 60);  // noise must not create events

  // 2. Rapid fault: 10 s on / 10 s off for an hour.
  HistLog b = run(3600, [](uint32_t t, bool &ok, bool &on, float &w) {
    ok = on = true;
    w = (t % 20 < 10) ? 140 : 0;
  });
  auto eb = decode(b);
  printf("rapid 10 s/10 s, 1 h: %zu events, %zu bytes, %d heat cycles\n", eb.size(), b.buf.size(), drawStarts(eb));
  assert(drawStarts(eb) == 180);
  for (size_t i = 2; i < eb.size(); i++) assert(eb[i].t - eb[i - 1].t == 10);  // 0, 1: ON and W at t=0

  // 3. Relay off for 30 min, plug unreachable for 5 min, back on.
  HistLog c = run(3600, [](uint32_t t, bool &ok, bool &on, float &w) {
    ok = !(t >= 2400 && t < 2700);
    on = t < 1800 || t >= 2100;
    w = on ? 150 : 0;
  });
  auto ec = decode(c);
  printf("relay/unreachable: ");
  for (auto &e : ec) printf("[%u %s%s] ", e.t - 1000, (const char *[]){"W", "OFF", "ON", "GONE"}[e.kind],
                            e.kind == HIST_W ? (":" + std::to_string(e.w)).c_str() : "");
  printf("\n");
  assert(ec.size() == 9);
  assert(ec[4].kind == HIST_ON && ec[6].kind == HIST_GONE && ec[6].t - 1000 == 2400);
  assert(ec[7].kind == HIST_ON && ec[7].t - 1000 == 2700 && ec[8].w == 150);

  // 4. Plug with no meter (Kasa): relay changes only.
  HistLog d = run(600, [](uint32_t t, bool &ok, bool &on, float &w) {
    ok = true;
    on = t >= 300;
    w = -1;
  });
  auto ed = decode(d);
  assert(ed.size() == 2 && ed[0].kind == HIST_OFF && ed[1].kind == HIST_ON);

  // 5. Slow drift (warming element) records a few steps, not every poll.
  HistLog e = run(1200, [](uint32_t t, bool &ok, bool &on, float &w) {
    ok = on = true;
    w = 200 - t * 0.05f;  // 200 W down to 140 W
  });
  printf("drift 200->140 W: %zu events\n", decode(e).size());
  assert(decode(e).size() <= 6);

  // Big gap: dt above 2^14 s needs multi-byte varints.
  HistLog f;
  f.begin(0);
  f.sample(0, true, true, 0);
  f.sample(100000, true, true, 500);
  auto ef = decode(f);
  assert(ef.back().t == 100000 && ef.back().w == 500);

  // A vector for the web app's decoder test (docs/history.js).
  printf("vector t0=%u start=%d/%d/%d hex=", c.t0, c.start.ok, c.start.on, c.start.w);
  for (auto x : c.buf) printf("%02x", x);
  printf("\nall passed\n");
}
