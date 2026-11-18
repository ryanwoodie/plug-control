// Plug controller: LilyGo T-A7670G runs a private Wi-Fi network for Shelly
// and Kasa plugs, runs their schedules locally, and talks to the web app over one
// low-traffic, end-to-end encrypted MQTT session on the 1NCE SIM.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <sys/time.h>
#include "board.h"
#include "crypto.h"
#include "history.h"
#include "sched.h"
#include "fwupdate.h"
#include "fwfetch.h"
#include "kasa.h"
#include "link.h"
#include "secrets.h"
#include "shelly.h"

#define FW_VERSION "1.2"

// on/off (minutes), both set: rotate within the window (see sched.h).
struct Rule {
  uint8_t days;    // bit 0 = Sunday
  uint16_t start;  // minutes after local midnight
  uint16_t dur;    // minutes
  uint16_t on, off;
};
struct OneOff {
  uint32_t at;  // UTC start
  uint16_t dur;
  uint16_t on, off;
};

struct Plug {
  const PlugDef *def;
  String ip, name;
  std::vector<Rule> rules;
  std::vector<OneOff> ones;
  uint16_t maxOn = 0;  // safety cap for manual "on", minutes (0 = none)
  bool reachable = false, everSeen = false, hardened = false, on = false;
  float watts = 0, volts = 0, wh = 0;
  uint8_t misses = 0;
  time_t timerEnd = 0;
  time_t cycleEnd = 0;  // end of the rotation window it is in, 0 = not rotating
  time_t stopUntil = 0;  // a manual Off stopped the rotation until then
  uint32_t followMs = 0;  // when to send a report with the load's real draw
  char src = '-';  // what last switched it: s=schedule m=manual t=timer b=button/local
  bool prevWant = false, schedPending = false;
  uint32_t lastCmdMs = 0, lastPollMs = 0;
  time_t onSince = 0, onAlertAt = 0;  // for the "on for 12 h" alert
  uint32_t goneMs = 0;  // when it stopped answering
  bool goneAlerted = false;
  bool kasa() const { return def->type == 'k'; }
};

// Report urgency. Urgent changes (on/off, acks) go out in ~3 s; lazy ones
// (plug reachability, heartbeat) wait 90 s so blips and bursts collapse into
// one message, and are dropped if nothing visible changed. Force always sends.
enum : uint8_t { DIRTY_LAZY = 1, DIRTY_URGENT = 2, DIRTY_FORCE = 4 };

struct Owner {
  const OwnerDef *def;
  String topicC, topicX, topicS, tz = "CST6";
  uint64_t cfgVer = 0;  // ms timestamp set by the web app
  std::vector<String> seen;  // recently applied command ids
  uint8_t dirty = DIRTY_FORCE;  // first report ~90 s after boot, once plugs have checked in
  uint32_t dirtyMs = 0;
  uint8_t why = 0;  // Usage bits: what the pending report is for
  bool heldNoted = false;
  uint32_t lastReportMs = 0, plugWinMs = 0;
  uint8_t plugReps = 0;  // reports only about plug Wi-Fi blips in the hour from plugWinMs
  String lastSig;  // state summary of the last report, to skip no-op reports
};

static Plug plugs[PLUG_COUNT];
static Owner owners[OWNER_COUNT];
static Preferences prefs;
static WebServer web(80);
static const char *timeSource = "none";
static float gpsLat = 0, gpsLon = 0;
static bool gpsFix = false;

static bool timeValid() { return time(nullptr) > 1700000000; }

void noteTime(time_t utc, const char *source) {
  time_t now = time(nullptr);
  if (labs((long)(utc - now)) > 2) {
    timeval tv{utc, 0};
    settimeofday(&tv, nullptr);
    Serial.printf("[time] set from %s: %ld\n", source, (long)utc);
  }
  timeSource = source;
}

static void markDirty(uint8_t owner, uint8_t level = DIRTY_URGENT, Usage why = U_CMD) {
  if (!owners[owner].dirty) owners[owner].dirtyMs = millis();
  owners[owner].dirty |= level;
  owners[owner].why |= 1 << why;
}

// Each owner's report shows the other plugs' on/off as a hint: when one
// switches, the other owners get a lazy report (dropped if nothing shows).
static void markOthers(uint8_t owner) {
  for (uint8_t i = 0; i < OWNER_COUNT; i++)
    if (i != owner) markDirty(i, DIRTY_LAZY, U_OTHER);
}

// ---- owner config ----------------------------------------------------------
static void loadConfig(uint8_t oi, const String &json, bool persist) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  Owner &o = owners[oi];
  o.cfgVer = doc["v"].as<uint64_t>();
  o.tz = doc["tz"] | "CST6";
  for (auto &p : plugs) {
    if (p.def->owner != oi) continue;
    JsonObject c = doc["p"][p.def->mac];
    if (c.isNull()) continue;
    p.name = c["n"] | p.def->name;
    p.maxOn = c["max"] | 0;
    p.rules.clear();
    for (JsonObject r : c["r"].as<JsonArray>()) {
      if (!(r["e"] | 1)) continue;
      p.rules.push_back({(uint8_t)(r["d"] | 0), (uint16_t)(r["s"] | 0), (uint16_t)(r["m"] | 0),
                         (uint16_t)(r["on"] | 0), (uint16_t)(r["off"] | 0)});
    }
    p.ones.clear();
    for (JsonObject r : c["o"].as<JsonArray>())
      p.ones.push_back({r["at"] | 0u, (uint16_t)(r["m"] | 0), (uint16_t)(r["on"] | 0), (uint16_t)(r["off"] | 0)});
    p.prevWant = false;  // re-evaluate against the new schedule
  }
  if (persist) prefs.putString(("c" + String(oi)).c_str(), json);
  Serial.printf("[cfg] owner %s config v%llu\n", o.def->user, o.cfgVer);
}

static void saveSeen(uint8_t oi) {
  String s;
  for (auto &id : owners[oi].seen) s += id + ",";
  prefs.putString(("x" + String(oi)).c_str(), s);
}

// ---- plug control --------------------------------------------------------
static bool setPlug(Plug &p, bool on, uint32_t secs, char src) {
  p.lastCmdMs = millis();
  if (p.ip.isEmpty() || !(p.kasa() ? kasaSet(p.ip, on, secs) : shellySet(p.ip, on, secs))) return false;
  if (on != p.on) p.onSince = on && timeValid() ? time(nullptr) : 0, p.onAlertAt = 0;
  p.on = on;
  p.src = src;
  if (!on && src == 'm' && p.cycleEnd) p.stopUntil = p.cycleEnd;  // Off ends this rotation run
  p.timerEnd = (on && secs && timeValid()) ? time(nullptr) + secs : 0;
  // Rotation switches ride along with later reports (the web app shows
  // "rotating"); everything else is reported now, and a switch-on again a
  // minute later so the app sees the load's actual draw.
  bool rotating = src == 's' && p.cycleEnd;
  markDirty(p.def->owner, rotating ? DIRTY_LAZY : DIRTY_URGENT);
  if (!rotating) markOthers(p.def->owner);
  p.followMs = on && !rotating && !p.kasa() ? (millis() + 60000) | 1 : 0;
  Serial.printf("[plug] %s %s %lus (%c)\n", p.def->mac, on ? "ON" : "OFF", (unsigned long)secs, src);
  return true;
}

static bool pollKasa(Plug &p, ShellyStatus &st) {
  String ip = kasaFindIp(p.def->mac);  // DHCP only, so it can move
  if (ip.length()) p.ip = ip;
  KasaStatus ks;
  if (p.ip.isEmpty() || !kasaGetStatus(p.ip, ks)) return false;
  st = {ks.on, 0, 0, 0};
  if (!ks.clockSet) {
    // It lost power. Kasa plugs restore their last state and drop their
    // countdown, so switch it off like a Shelly would; a schedule re-applies.
    Serial.printf("[plug] %s restarted\n", p.def->mac);
    linkNotify(p.name + " lost power", String("It restarted") + (ks.on ? " on, so the controller switched it off." : ".") +
               (p.prevWant ? " Its schedule switches it back on." : ""), 3, "electric_plug");
    kasaSetClock(p.ip);
    if (ks.on && kasaSet(p.ip, false)) st.on = false;
    p.timerEnd = 0;
    if (p.prevWant) p.schedPending = true;
  }
  return true;
}

static void pollPlug(Plug &p) {
  ShellyStatus st;
  if (!(p.kasa() ? pollKasa(p, st) : shellyGetStatus(p.ip, st))) {
    if (p.reachable && ++p.misses >= 30) {  // ~1 min of silence
      p.reachable = false;
      histSample(&p - plugs, false, false, 0);
      markDirty(p.def->owner, DIRTY_LAZY, U_PLUG);
      markOthers(p.def->owner);
      Serial.printf("[plug] %s unreachable\n", p.def->mac);
    }
    return;
  }
  p.misses = 0;
  if (!p.reachable) {
    p.reachable = true;
    p.everSeen = true;
    markDirty(p.def->owner, DIRTY_LAZY, U_PLUG);
    markOthers(p.def->owner);
    Serial.printf("[plug] %s online at %s\n", p.def->mac, p.ip.c_str());
    if (p.prevWant && !st.on && p.src == 's') p.schedPending = true;  // plug lost power mid-window
  }
  if (!p.hardened && (p.kasa() ? kasaHarden(p.ip, p.name.c_str()) : shellyHarden(p.ip, p.name.c_str()))) {
    p.hardened = true;
    prefs.putBool(("h" + String(p.def->mac)).c_str(), true);
  }
  if (st.on != p.on) {
    // The plug's own countdown (set by us) switching it off is expected;
    // anything else that we did not just command was a button press.
    bool timedOff = !st.on && p.timerEnd && labs((long)(time(nullptr) - p.timerEnd)) <= 20;
    if (!timedOff && millis() - p.lastCmdMs > 5000) p.src = 'b';
    if (!st.on) p.timerEnd = 0;
    markDirty(p.def->owner, timedOff && p.cycleEnd ? DIRTY_LAZY : DIRTY_URGENT);
    if (!p.cycleEnd) markOthers(p.def->owner);
  }
  if (st.on != p.on) p.onSince = st.on ? time(nullptr) : 0, p.onAlertAt = 0;
  p.on = st.on;
  p.watts = st.watts;
  p.volts = st.volts;
  p.wh = st.wh;
  if (p.followMs && (int32_t)(millis() - p.followMs) >= 0) {
    p.followMs = 0;
    // Watts aren't in the signature. Optional, so not past the data reserve.
    if (p.on && linkInfo().saving) linkCount(U_HELD);
    else if (p.on) markDirty(p.def->owner, DIRTY_URGENT | DIRTY_FORCE, U_FOLLOW);
  }
  histSample(&p - plugs, true, st.on, p.kasa() ? -1 : st.watts);
}

// ---- scheduler -------------------------------------------------------------
// Returns when the plug's current on period ends, or 0 if it should be off.
// *cycleEnd: end of the rotation window it is in (even in an off phase), or 0.
static time_t scheduleEnd(const Plug &p, time_t now, time_t *cycleEnd) {
  *cycleEnd = 0;
  setenv("TZ", owners[p.def->owner].tz.c_str(), 1);
  tzset();
  struct tm lt;
  localtime_r(&now, &lt);
  int nowMin = lt.tm_hour * 60 + lt.tm_min;
  time_t end = 0;
  for (auto &r : p.rules) {
    for (int back = 0; back <= 1; back++) {  // windows may start yesterday and run past midnight
      int day = (lt.tm_wday - back + 7) % 7;
      if (!(r.days & (1 << day))) continue;
      int start = r.start - back * 1440;
      if (nowMin >= start && nowMin < start + r.dur) {
        time_t we = now + (start + r.dur - nowMin) * 60 - lt.tm_sec, ws = we - r.dur * 60;
        if (r.on && r.off && we > *cycleEnd) *cycleEnd = we;
        time_t e = windowOnUntil(ws, we, r.on, r.off, now);
        if (e > end) end = e;
      }
    }
  }
  for (auto &o : p.ones) {
    time_t ws = o.at, we = ws + o.dur * 60;
    if (o.on && o.off && now >= ws && now < we && we > *cycleEnd) *cycleEnd = we;
    time_t e = windowOnUntil(ws, we, o.on, o.off, now);
    if (e > end) end = e;
  }
  return end;
}

static void runSchedules() {
  if (!timeValid()) return;
  time_t now = time(nullptr);
  for (auto &p : plugs) {
    time_t cyc;
    time_t end = scheduleEnd(p, now, &cyc);
    if (now < p.stopUntil) end = cyc = 0;
    if ((cyc != 0) != (p.cycleEnd != 0)) markDirty(p.def->owner);  // rotation started or ended
    p.cycleEnd = cyc;
    bool want = end > now;
    if (want && !p.prevWant) p.schedPending = true;
    if (!want) {
      if (p.prevWant && p.src == 's' && p.on) setPlug(p, false, 0, 's');
      p.schedPending = false;
    }
    if (want && p.schedPending && p.reachable && setPlug(p, true, end - now, 's')) p.schedPending = false;
    p.prevWant = want;
  }
}

// ---- commands from the web app ---------------------------------------------
static void handleCommands(uint8_t oi, JsonDocument &doc) {
  Owner &o = owners[oi];
  bool changed = false;
  for (JsonObject c : doc["q"].as<JsonArray>()) {
    String id = c["id"] | "";
    if (!id.length() || std::find(o.seen.begin(), o.seen.end(), id) != o.seen.end()) continue;
    o.seen.push_back(id);
    if (o.seen.size() > 12) o.seen.erase(o.seen.begin());
    changed = true;
    String a = c["a"] | "";
    if (a == "more") {
      // An owner allows more data today (lifts the data saver or the cap).
      // Accepted for 2 h: a capped controller only checks in hourly.
      long age = timeValid() ? (long)(time(nullptr) - (time_t)(c["t"] | 0u)) : 0;
      uint32_t kb = c["k"] | 0u;
      if (labs(age) <= 7200 && kb && kb <= 1024) linkAllow(kb * 1024);
      markDirty(oi, DIRTY_URGENT | DIRTY_FORCE);
      continue;
    }
    if (a == "ref") {
      // Past the data reserve a refresh is not answered; the web app says so.
      if (linkInfo().saving) linkCount(U_HELD);
      else markDirty(oi, DIRTY_URGENT | DIRTY_FORCE, U_REF);
      continue;
    }
    markDirty(oi);
    long age = timeValid() ? (long)(time(nullptr) - (time_t)(c["t"] | 0u)) : 0;
    if (labs(age) > CMD_MAX_AGE_S) {
      Serial.printf("[cmd] %s expired (%lds)\n", id.c_str(), age);
      continue;
    }
    for (auto &p : plugs) {
      if (p.def->owner != oi || strcmp(p.def->mac, c["p"] | "") != 0) continue;
      uint32_t mins = c["m"] | 0;
      bool ok = true;
      if (a == "on") ok = setPlug(p, true, p.maxOn * 60, 'm');
      else if (a == "off") ok = setPlug(p, false, 0, 'm');
      else if (a == "tmr" && mins) ok = setPlug(p, true, mins * 60, 't');
      if (!ok) linkNotify("Couldn't switch " + p.name, String("The plug didn't take the ") + (a == "off" ? "Off" : "On") +
                          " command" + (p.reachable ? "." : ": it isn't answering on Wi-Fi."), 4, "warning");
    }
  }
  if (changed) saveSeen(oi);
}

static void handleInbound() {
  String topic;
  std::vector<uint8_t> data;
  while (linkReceive(topic, data)) {
    if (histInbound(topic, data.data(), data.size())) continue;
    if (fetchInbound(topic, data.data(), data.size())) {
      for (uint8_t i = 0; i < OWNER_COUNT; i++) markDirty(i, DIRTY_LAZY | DIRTY_FORCE);  // show "update waiting"
      continue;
    }
    for (uint8_t i = 0; i < OWNER_COUNT; i++) {
      Owner &o = owners[i];
      if (topic != o.topicC && topic != o.topicX) continue;
      String json;
      if (!openMsg(o.def->key, topic, data.data(), data.size(), json)) {
        Serial.printf("[in] rejected message on %s\n", topic.c_str());
        break;
      }
      JsonDocument doc;
      if (deserializeJson(doc, json)) break;
      // Authenticated messages carry the sender's clock: a free fallback.
      if (!timeValid() && (doc["t"] | 0u) > 1700000000) noteTime(doc["t"] | 0u, "app");
      if (topic == o.topicC) {
        if (doc["v"].as<uint64_t>() > o.cfgVer) {
          loadConfig(i, json, true);
          markDirty(i);
        }
      } else {
        handleCommands(i, doc);
      }
      break;
    }
  }
}

// ---- status reports --------------------------------------------------------
static String buildStatus(uint8_t oi) {
  JsonDocument d;
  LinkInfo li = linkInfo();
  d["t"] = (uint32_t)time(nullptr);
  d["up"] = millis() / 1000;
  d["fw"] = FW_VERSION;
  String fwq = fetchWaiting();
  if (fwq.length()) {  // an update is waiting for a hotspot
    JsonObject u = d["fwq"].to<JsonObject>();
    u["v"] = fwq;
    u["h"] = fetchHotspots();
    if (fetchError().length()) u["e"] = fetchError();
  }
  d["cv"] = owners[oi].cfgVer;
  d["tz"] = owners[oi].tz;
  JsonArray ax = d["ax"].to<JsonArray>();
  for (auto &id : owners[oi].seen) ax.add(id);
  for (auto &p : plugs) {
    if (p.def->owner != oi) continue;
    JsonObject j = d["p"][p.def->mac].to<JsonObject>();
    j["n"] = p.name;
    j["ok"] = p.reachable ? 1 : 0;
    j["on"] = p.on ? 1 : 0;
    if (!p.kasa()) {  // Kasa HS103 has no power metering
      j["w"] = roundf(p.watts * 10) / 10;
      j["v"] = roundf(p.volts);
      j["kwh"] = roundf(p.wh) / 1000.0;
    }
    j["te"] = (uint32_t)p.timerEnd;
    j["s"] = String(p.src);
    if (p.cycleEnd) j["cy"] = (uint32_t)p.cycleEnd;
  }
  // The other owners' plugs, as a hint: [name, reachable, on, rotating].
  JsonArray op = d["op"].to<JsonArray>();
  for (auto &p : plugs) {
    if (p.def->owner == oi) continue;
    JsonArray a = op.add<JsonArray>();
    a.add(p.name);
    a.add(p.reachable ? 1 : 0);
    a.add(p.on ? 1 : 0);
    a.add(p.cycleEnd ? 1 : 0);
  }
  JsonObject n = d["net"].to<JsonObject>();
  n["csq"] = li.csq;
  n["op"] = li.op;
  n["d"] = li.today;
  n["m"] = li.month;
  n["tot"] = li.total;
  if (li.capped) n["cap"] = li.capMonth ? 2 : 1;
  if (li.saving) n["sv"] = 1;
  // Data saver and cap points, today and this month (bytes), incl. extra allowed.
  JsonArray lim = n["lim"].to<JsonArray>();
  for (uint32_t v : {li.dayReserve, li.dayCap, li.monthReserve, li.monthCap}) lim.add(v);
  if (li.extraDay) n["x"] = li.extraDay;
  // Today's breakdown: [connect, sent, received, packet overhead, keep-alive]
  // bytes, [sessions, fresh sessions], and message counts by Usage.
  JsonArray ub = n["ub"].to<JsonArray>();
  for (uint32_t v : {li.use.conn, li.use.tx, li.use.rx, li.use.pkt, li.use.ka}) ub.add(v);
  JsonArray us = n["us"].to<JsonArray>();
  us.add(li.use.sess);
  us.add(li.use.fresh);
  JsonArray un = n["un"].to<JsonArray>();
  for (uint16_t v : li.use.n) un.add(v);
  d["ts"] = timeSource;
  if (gpsFix) {
    JsonArray g = d["gps"].to<JsonArray>();
    g.add(roundf(gpsLat * 10000) / 10000);
    g.add(roundf(gpsLon * 10000) / 10000);
  }
  String out;
  serializeJson(d, out);
  return out;
}

// What the web app would show, minus readings that drift (watts, volts, kWh).
static String stateSignature(uint8_t oi) {
  String s = String(owners[oi].cfgVer) + "|";
  if (!owners[oi].seen.empty()) s += owners[oi].seen.back();
  for (auto &p : plugs) {
    if (p.def->owner != oi) continue;
    s += "|" + String(p.def->mac) + (p.reachable ? "1" : "0");
    // While rotating, its on/off flips are not news: the app shows "rotating".
    if (p.cycleEnd) s += "cy" + String((uint32_t)p.cycleEnd);
    else s += (p.on ? "1" : "0") + String(p.src) + String((uint32_t)p.timerEnd);
  }
  for (auto &p : plugs) {
    if (p.def->owner == oi) continue;
    s += "|o" + String(p.reachable ? "1" : "0") + (p.cycleEnd ? "r" : p.on ? "1" : "0");
  }
  return s;
}

static void reportStatus() {
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > HEARTBEAT_S * 1000UL) {
    lastBeat = millis();
    for (uint8_t i = 0; i < OWNER_COUNT; i++) markDirty(i, DIRTY_LAZY | DIRTY_FORCE, U_BEAT);
  }
  LinkInfo li = linkInfo();
  // Just hit the cap: one last report saying so, sent before the link closes.
  static bool wasCapped = false;
  if (li.capped && !wasCapped)
    for (uint8_t i = 0; i < OWNER_COUNT; i++) markDirty(i, DIRTY_URGENT | DIRTY_FORCE, U_BEAT);
  wasCapped = li.capped;
  if (!li.online) return;  // stay dirty until the link is up
  for (uint8_t i = 0; i < OWNER_COUNT; i++) {
    Owner &o = owners[i];
    if (!o.dirty) continue;
    uint32_t wait = (o.dirty & DIRTY_URGENT) ? 3000 : 90000;
    if (millis() - o.dirtyMs < wait) continue;
    String sig = stateSignature(i);
    if (!(o.dirty & DIRTY_FORCE) && sig == o.lastSig) {
      o.dirty = o.why = 0;  // e.g. a plug dropped off Wi-Fi and came back: nothing to tell
      continue;
    }
    bool lazyOnly = !(o.dirty & (DIRTY_URGENT | DIRTY_FORCE));
    // Past the data reserve, lazy news (plug blips, rotation switches) waits
    // for the next heartbeat or command.
    if (lazyOnly && li.saving) {
      if (!o.heldNoted) linkCount(U_HELD), o.heldNoted = true;
      continue;
    }
    // A plug that keeps dropping off Wi-Fi: after 3 reports about it in an
    // hour, at most one every 20 minutes.
    bool plugOnly = lazyOnly && !(o.why & ~((1 << U_PLUG) | (1 << U_OTHER)));
    if (plugOnly) {
      if (millis() - o.plugWinMs > 3600 * 1000UL) o.plugWinMs = millis(), o.plugReps = 0;
      if (o.plugReps >= 3 && millis() - o.lastReportMs < 20 * 60 * 1000UL) continue;
      o.plugReps++;
    }
    std::vector<uint8_t> sealed;
    if (sealMsg(o.def->key, o.topicS, buildStatus(i), sealed)) linkPublish(o.topicS, std::move(sealed));
    static const Usage order[] = {U_REF, U_CMD, U_FOLLOW, U_PLUG, U_OTHER, U_BEAT};
    Usage why = U_BEAT;  // the first report after boot has no other reason
    for (Usage u : order)
      if (o.why & (1 << u)) { why = u; break; }
    linkCount(why);
    o.lastSig = sig;
    o.lastReportMs = millis();
    o.dirty = o.why = 0;
    o.heldNoted = false;
  }
}

// ---- push alerts (ntfy) -----------------------------------------------------
static String hours(time_t s) { return String((s + 1800) / 3600); }
static String localHM(time_t t, uint8_t owner) {
  setenv("TZ", owners[owner].tz.c_str(), 1);
  tzset();
  struct tm lt;
  localtime_r(&t, &lt);
  char b[8];
  strftime(b, sizeof b, "%H:%M", &lt);
  return b;
}

static void checkAlerts() {
  static uint32_t last = 0;
  if (millis() - last < 30000) return;
  last = millis();
  time_t now = time(nullptr);
  for (auto &p : plugs) {
    if (p.reachable) {
      if (p.goneAlerted) linkNotify(p.name + " is back", "It's answering on Wi-Fi again.", 2, "white_check_mark");
      p.goneMs = 0;
      p.goneAlerted = false;
    } else {
      if (!p.goneMs) p.goneMs = millis();
      if (!p.goneAlerted && millis() - p.goneMs > 30 * 60 * 1000UL) {
        p.goneAlerted = true;
        linkNotify(p.name + " isn't answering", String(p.everSeen ? "It dropped off" : "It hasn't joined") +
                   " the controller's Wi-Fi 30 minutes ago. If it's on, its own timer still switches it off.", 4, "warning");
      }
    }
    // On for 12 h straight, and every 12 h after that.
    if (!timeValid()) continue;
    if (!p.on) {
      if (p.reachable) p.onSince = p.onAlertAt = 0;
      continue;
    }
    if (!p.onSince) p.onSince = now;
    if (!p.onAlertAt) p.onAlertAt = p.onSince + 12 * 3600;
    if (now >= p.onAlertAt) {
      p.onAlertAt += 12 * 3600;
      linkNotify(p.name + " has been on " + hours(now - p.onSince) + " h",
                 p.timerEnd ? "Its timer ends at " + localHM(p.timerEnd, p.def->owner) + "." : String("It has no timer running."),
                 4, "fire");
    }
  }
  LinkInfo li = linkInfo();
  static bool wasSaving = false, wasCapped = false;
  if (li.saving && !wasSaving && !li.capped)
    linkNotify("Data saver on", String(li.today / 1024) + " KB used today. Refreshes and minor updates are paused until the day resets; On/Off still work.", 2, "floppy_disk");
  if (li.capped && !wasCapped)
    linkNotify(li.capMonth ? "Monthly data limit reached" : "Daily data limit reached",
               "The controller is offline but for hourly check-ins. Plug timers and schedules keep running. Use Allow more data in the app to bring it back.", 4, "no_entry");
  wasSaving = li.saving;
  wasCapped = li.capped;
}

// Once at boot: why it started, and whether the firmware changed.
static void bootAlerts() {
  String was = prefs.getString("fwv", "");
  if (was != FW_VERSION) {
    prefs.putString("fwv", FW_VERSION);
    if (was.length()) linkNotify("Controller firmware " FW_VERSION, "It was running " + was + " before.", 3, "package");
  }
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: linkNotify("Controller started", "It was powered up (power cut, or plugged in).", 3, "electric_plug"); break;
    case ESP_RST_PANIC: linkNotify("Controller crashed and restarted", "Reason: software fault.", 4, "warning"); break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: linkNotify("Controller hung and restarted", "Reason: watchdog.", 4, "warning"); break;
    case ESP_RST_BROWNOUT: linkNotify("Controller restarted", "Reason: its power supply dipped (brownout).", 4, "warning"); break;
    default: break;
  }
}

// ---- GPS (time source + coarse location) ------------------------------------
static void readGps() {
  static String line;
  while (Serial2.available()) {
    char c = Serial2.read();
    if (c != '\n') {
      if (line.length() < 120) line += c;
      continue;
    }
    // $GNRMC,hhmmss.ss,A,ddmm.mmmm,N,dddmm.mmmm,W,spd,crs,ddmmyy,...
    if (line.startsWith("$GNRMC") || line.startsWith("$GPRMC")) {
      String f[10];
      int idx = 0, from = 0;
      for (int i = 0; i <= (int)line.length() && idx < 10; i++) {
        if (i == (int)line.length() || line[i] == ',') {
          f[idx++] = line.substring(from, i);
          from = i + 1;
        }
      }
      if (idx == 10 && f[2] == "A" && f[1].length() >= 6 && f[9].length() == 6) {
        struct tm t = {};
        t.tm_hour = f[1].substring(0, 2).toInt();
        t.tm_min = f[1].substring(2, 4).toInt();
        t.tm_sec = f[1].substring(4, 6).toInt();
        t.tm_mday = f[9].substring(0, 2).toInt();
        t.tm_mon = f[9].substring(2, 4).toInt() - 1;
        t.tm_year = f[9].substring(4, 6).toInt() + 100;
        setenv("TZ", "UTC0", 1);
        tzset();
        noteTime(mktime(&t), "gps");
        float la = f[3].substring(0, 2).toFloat() + f[3].substring(2).toFloat() / 60;
        float lo = f[5].substring(0, 3).toFloat() + f[5].substring(3).toFloat() / 60;
        gpsLat = f[4] == "S" ? -la : la;
        gpsLon = f[6] == "W" ? -lo : lo;
        gpsFix = true;
      }
    }
    line = "";
  }
}

// ---- provisioning of factory-fresh plugs ------------------------------------
static void provisionMissing() {
  static uint32_t last = 0;
  // A scan briefly knocks plugs off our network, so only scan often for plugs
  // never seen since boot (factory-fresh or reset), and give known plugs 90 s
  // to rejoin after a reboot first.
  bool neverSeen = false, missing = false;
  for (auto &p : plugs) {
    neverSeen |= !p.everSeen;
    missing |= !p.reachable;
  }
  uint32_t interval = neverSeen ? (millis() < 30 * 60 * 1000UL ? 60 * 1000UL : 10 * 60 * 1000UL) : 30 * 60 * 1000UL;
  if (!missing || millis() < 90000 || millis() - last < interval) return;
  last = millis();

  int n = WiFi.scanNetworks(false, false, false, 120);
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    bool shelly = ssid.startsWith("Shelly") || ssid.startsWith("shelly");
    if (!shelly && !ssid.startsWith("TP-LINK_Smart Plug_")) continue;
    for (auto &p : plugs) {
      if (p.reachable || p.kasa() == shelly) continue;
      // Shelly APs end in the whole MAC, Kasa ones in its last 4 digits.
      if (ssid.endsWith(shelly ? p.def->mac : p.def->mac + 8)) {
        if (shelly) shellyProvision(ssid, p.ip, AP_SSID, AP_PASS);
        else kasaProvision(ssid, AP_SSID, AP_PASS);
        p.hardened = false;
        prefs.remove(("h" + String(p.def->mac)).c_str());
      }
    }
  }
  WiFi.scanDelete();
}

// ---- on-site page (only reachable on the board's own Wi-Fi) -----------------
static void setupWeb() {
  web.on("/", [] {
    String h = F("<!doctype html><meta name=viewport content='width=device-width'><title>Plugs</title>"
                 "<style>body{font:16px system-ui;margin:16px}div{border:1px solid #ccc;border-radius:8px;"
                 "padding:12px;margin:8px 0}a{margin-right:12px}</style><h2>Plugs (on-site)</h2>");
    for (int i = 0; i < PLUG_COUNT; i++) {
      Plug &p = plugs[i];
      h += "<div><b>" + p.name + "</b> &middot; " + owners[p.def->owner].def->user + "<br>" +
           (p.reachable ? (p.on ? "ON " : "OFF ") + (p.kasa() ? String() : String(p.watts, 1) + " W")
                        : String("unreachable")) +
           "<br><a href='/set?i=" + i + "&on=1'>On</a><a href='/set?i=" + i + "&on=0'>Off</a>" +
           "<a href='/set?i=" + i + "&on=1&m=60'>On 1h</a></div>";
    }
    LinkInfo li = linkInfo();
    h += "<p>Cell: " + String(li.online ? "online" : "offline") + ", CSQ " + li.csq + ", today ~" +
         String(li.today / 1024.0, 1) + " KB, month ~" + String(li.month / 1024.0, 1) + " KB</p>";
    h += "<p>Firmware " FW_VERSION " &middot; <a href='/update'>Update firmware</a></p>";
    web.send(200, "text/html", h);
  });
  web.on("/set", [] {
    int i = web.arg("i").toInt();
    if (i >= 0 && i < PLUG_COUNT) {
      uint32_t m = web.arg("m").toInt();
      setPlug(plugs[i], web.arg("on") == "1", m * 60, m ? 't' : 'm');
    }
    web.sendHeader("Location", "/");
    web.send(303);
  });
  web.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] plug controller %s\n", FW_VERSION);
  prefs.begin("app");
  // New owner keys (make_secrets.py re-run): saved settings belong to the old
  // logins, so start fresh and re-apply plug names. The data meter is kept.
  String keyId;
  for (uint8_t i = 0; i < OWNER_COUNT; i++) keyId += OWNERS[i].tid;
  if (prefs.getString("keyid", "") != keyId) {
    prefs.clear();
    prefs.putString("keyid", keyId);
    Serial.println("[boot] owner keys changed, settings reset");
  }

  pinMode(GPS_WAKE_PIN, OUTPUT);
  digitalWrite(GPS_WAKE_PIN, HIGH);
  Serial2.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  std::vector<String> subs;
  for (uint8_t i = 0; i < OWNER_COUNT; i++) {
    Owner &o = owners[i];
    o.def = &OWNERS[i];
    String base = "plugctl/" + String(o.def->tid) + "/";
    o.topicC = base + "c";
    o.topicX = base + "x";
    o.topicS = base + "s";
    subs.push_back(o.topicC);
    subs.push_back(o.topicX);
    String seen = prefs.getString(("x" + String(i)).c_str(), "");
    for (int a = 0, b; (b = seen.indexOf(',', a)) > 0; a = b + 1) o.seen.push_back(seen.substring(a, b));
  }
  for (int i = 0; i < PLUG_COUNT; i++) {
    plugs[i].def = &PLUGS[i];
    plugs[i].name = PLUGS[i].name;
    if (!plugs[i].kasa()) plugs[i].ip = "192.168.4." + String(PLUG_IP_BASE + i);
    plugs[i].hardened = prefs.getBool(("h" + String(PLUGS[i].mac)).c_str(), false);
  }
  histBegin(&prefs);
  for (auto &t : histTopics()) subs.push_back(t);
  fetchBegin(&prefs, FW_VERSION);
  subs.push_back(fetchTopic());
  for (uint8_t i = 0; i < OWNER_COUNT; i++) {
    String c = prefs.getString(("c" + String(i)).c_str(), "");
    if (c.length()) loadConfig(i, c, false);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 8);
  Serial.printf("[wifi] AP %s up\n", AP_SSID);
  setupWeb();
  updateSetup(web, FW_VERSION);

  linkBegin(subs);
  bootAlerts();
}

void loop() {
  static uint32_t lastPoll = 0, lastSched = 0;
  static int next = 0;

  web.handleClient();
  readGps();
  handleInbound();

  // Each plug every 2 s for history (it is all local Wi-Fi); plugs that are
  // not answering every 10 s, since each try blocks for the timeout.
  if (millis() - lastPoll > 250) {
    lastPoll = millis();
    for (int k = 0; k < PLUG_COUNT; k++) {
      Plug &p = plugs[(next + k) % PLUG_COUNT];
      if (millis() - p.lastPollMs < (p.reachable ? 2000UL : 10000UL)) continue;
      p.lastPollMs = millis();
      pollPlug(p);
      next = (next + k + 1) % PLUG_COUNT;
      break;
    }
  }
  if (millis() - lastSched > 5000) {  // rotations switch within ~5 s
    lastSched = millis();
    runSchedules();
  }
  provisionMissing();
  reportStatus();
  histLoop();
  checkAlerts();
  fetchLoop();
  updateLoop();
  delay(5);
}
