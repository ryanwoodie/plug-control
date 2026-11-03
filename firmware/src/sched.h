// Rotation timing, kept free of Arduino types so tools/test_sched.cpp can
// test it on a computer.
#pragma once
#include <stdint.h>
#include <time.h>

// A schedule window [ws, we). With on and off (minutes) both set it rotates:
// on for `on`, off for `off`, repeating from ws. Returns when the current on
// period ends (never past we), or 0 if the plug should be off now.
static inline time_t windowOnUntil(time_t ws, time_t we, uint16_t on, uint16_t off, time_t now) {
  if (now < ws || now >= we) return 0;
  if (!on || !off) return we;
  time_t period = (time_t)(on + off) * 60, phase = (now - ws) % period;
  if (phase >= (time_t)on * 60) return 0;
  time_t e = now + (time_t)on * 60 - phase;
  return e < we ? e : we;
}
