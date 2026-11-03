// Host test for firmware/src/sched.h.
//   c++ -std=c++17 -I firmware/src tools/test_sched.cpp -o /tmp/s && /tmp/s
#include <assert.h>
#include <stdio.h>
#include "sched.h"

int main() {
  const time_t ws = 100000, H = 3600;
  // Plain window: on the whole time, off outside.
  assert(windowOnUntil(ws, ws + H, 0, 0, ws) == ws + H);
  assert(windowOnUntil(ws, ws + H, 0, 0, ws + H - 1) == ws + H);
  assert(windowOnUntil(ws, ws + H, 0, 0, ws + H) == 0);
  assert(windowOnUntil(ws, ws + H, 0, 0, ws - 1) == 0);

  // 15 on / 45 off for 4 h: on at 0, 60, 120, 180 min for 15 min each.
  int ons = 0, switches = 0;
  bool prev = false;
  for (time_t t = ws - 600; t < ws + 4 * H + 600; t += 5) {
    bool on = windowOnUntil(ws, ws + 4 * H, 15, 45, t) > 0;
    if (on && !prev) ons++;
    if (on != prev) switches++;
    prev = on;
  }
  assert(ons == 4 && switches == 8);
  assert(windowOnUntil(ws, ws + 4 * H, 15, 45, ws + 60 * 60 + 10) == ws + 75 * 60);  // ends with that period
  assert(windowOnUntil(ws, ws + 4 * H, 15, 45, ws + 20 * 60) == 0);                  // off phase

  // 5 on / 5 off for 24 h: 144 on periods.
  ons = 0, prev = false;
  for (time_t t = ws; t < ws + 24 * H; t += 5) {
    bool on = windowOnUntil(ws, ws + 24 * H, 5, 5, t) > 0;
    if (on && !prev) ons++;
    prev = on;
  }
  assert(ons == 144);

  // A last on period cut short by the window end: 25/10 for 60 min -> 0-25, 35-60.
  assert(windowOnUntil(ws, ws + H, 25, 10, ws + 40 * 60) == ws + H);
  assert(windowOnUntil(ws, ws + H, 25, 10, ws + 30 * 60) == 0);

  // Joining part way through (controller rebooted mid-rotation): picks up the phase.
  assert(windowOnUntil(ws, ws + 4 * H, 10, 20, ws + 2 * H + 5 * 60) == ws + 2 * H + 10 * 60);
  printf("all passed\n");
}
