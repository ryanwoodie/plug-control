#include "history.h"
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <deque>
#include "crypto.h"
#include "histlog.h"
#include "link.h"
#include "secrets.h"

static const uint32_t BATCH_S = 3600;       // send a batch with changes about hourly
static const uint32_t QUIET_BATCH_S = 6 * 3600;  // ...and an empty one this often
static const uint32_t MIN_BATCH_S = 60;     // seq numbers rely on this (see histBegin)
static const size_t BATCH_MAX_BYTES = 600;  // keeps the sealed message well under 2048
static const size_t BACKLOG_BYTES = 48 * 1024;  // kept for resends, oldest dropped first
static const int RESENDS_PER_DAY = 40;

struct Sent {
  uint8_t owner;
  uint32_t seq;
  std::vector<uint8_t> data;
  bool pending;  // not yet handed to the link
};

static Preferences *prefs;
static HistLog logs[PLUG_COUNT];
static bool started[PLUG_COUNT];
static uint32_t seq[OWNER_COUNT], lastReq[OWNER_COUNT];
static std::deque<Sent> backlog;
static size_t backlogBytes = 0;
static int resendsToday = 0, resendDay = -1;

static bool clockOk() { return time(nullptr) > 1700000000; }
static String base(uint8_t o) { return "plugctl/" + String(OWNERS[o].tid) + "/"; }

void histBegin(Preferences *p) {
  prefs = p;
  for (uint8_t o = 0; o < OWNER_COUNT; o++) {
    seq[o] = prefs->getUInt(("hq" + String(o)).c_str(), 0);
    lastReq[o] = prefs->getUInt(("hr" + String(o)).c_str(), 0);
  }
}

std::vector<String> histTopics() {
  std::vector<String> t;
  for (uint8_t o = 0; o < OWNER_COUNT; o++) t.push_back(base(o) + "hr");
  return t;
}

void histSample(int i, bool ok, bool on, float watts) {
  if (!clockOk()) return;
  uint32_t now = time(nullptr);
  if (!started[i]) {
    logs[i].begin(now);
    started[i] = true;
  }
  logs[i].sample(now, ok, on, watts);
}

static void dropOld() {
  while (backlogBytes > BACKLOG_BYTES && !backlog.empty()) {
    backlogBytes -= backlog.front().data.size();
    backlog.pop_front();
  }
}

static void sendBatch(int i, uint32_t now) {
  uint8_t o = PLUGS[i].owner;
  if (!seq[o]) seq[o] = now / 60;  // fresh (or reset) board: start above any earlier seq
  uint32_t q = ++seq[o];
  prefs->putUInt(("hq" + String(o)).c_str(), seq[o]);

  HistLog &l = logs[i];
  size_t n = 0;
  mbedtls_base64_encode(nullptr, 0, &n, l.buf.data(), l.buf.size());
  std::vector<uint8_t> b64(n + 1, 0);
  mbedtls_base64_encode(b64.data(), b64.size(), &n, l.buf.data(), l.buf.size());

  JsonDocument d;
  d["p"] = PLUGS[i].mac;
  d["q"] = q;
  d["a"] = l.t0;
  d["b"] = now;
  JsonArray s = d["s"].to<JsonArray>();
  s.add(l.start.ok ? 1 : 0);
  s.add(l.start.on ? 1 : 0);
  s.add(l.start.w);
  d["e"] = (const char *)b64.data();
  String json;
  serializeJson(d, json);

  std::vector<uint8_t> sealed;
  if (sealMsg(OWNERS[o].key, base(o) + "h/" + q, json, sealed)) {
    backlogBytes += sealed.size();
    backlog.push_back({o, q, std::move(sealed), true});
    dropOld();
  }
  Serial.printf("[hist] %s batch %lu: %u B of changes over %lus\n", PLUGS[i].mac, (unsigned long)q,
                (unsigned)l.buf.size(), (unsigned long)(now - l.t0));
  l.begin(now);
}

void histLoop() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 1000 || !clockOk()) return;
  lastMs = millis();
  uint32_t now = time(nullptr);
  for (int i = 0; i < PLUG_COUNT; i++) {
    if (!started[i]) continue;
    HistLog &l = logs[i];
    uint32_t age = now - l.t0;
    if (age < MIN_BATCH_S) continue;
    if ((age >= BATCH_S && !l.buf.empty()) || age >= QUIET_BATCH_S || l.buf.size() >= BATCH_MAX_BYTES)
      sendBatch(i, now);
  }
  // One message per second at most, and only while the link is up, so a long
  // outage leaves batches here instead of overflowing the link's queue.
  if (!linkInfo().online) return;
  for (auto &b : backlog) {
    if (!b.pending) continue;
    std::vector<uint8_t> copy = b.data;
    linkPublish(base(b.owner) + "h/" + b.seq, std::move(copy));
    b.pending = false;
    break;
  }
}

bool histInbound(const String &topic, const uint8_t *data, size_t len) {
  for (uint8_t o = 0; o < OWNER_COUNT; o++) {
    if (topic != base(o) + "hr") continue;
    if (!len) return true;  // the collector cleared its request
    String json;
    JsonDocument d;
    if (!openMsg(OWNERS[o].hkey, topic, data, len, json) || deserializeJson(d, json)) {
      Serial.println("[hist] rejected resend request");
      return true;
    }
    uint32_t t = d["t"] | 0u;
    if (t <= lastReq[o]) return true;  // already handled (retained copy on reconnect)
    lastReq[o] = t;
    prefs->putUInt(("hr" + String(o)).c_str(), t);
    int day = time(nullptr) / 86400;
    if (day != resendDay) resendDay = day, resendsToday = 0;
    int n = 0;
    for (uint32_t q : d["r"].as<JsonArray>()) {
      for (auto &b : backlog) {
        if (b.owner == o && b.seq == q && !b.pending && resendsToday < RESENDS_PER_DAY) {
          b.pending = true;
          resendsToday++;
          n++;
        }
      }
    }
    Serial.printf("[hist] resend request: %d of %u batches still here\n", n, (unsigned)d["r"].size());
    return true;
  }
  return false;
}
