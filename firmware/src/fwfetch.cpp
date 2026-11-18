#include "fwfetch.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include "crypto.h"
#include "link.h"
#include "secrets.h"

static const uint32_t SCAN_EVERY_MS = 2 * 60 * 1000UL;
static const uint32_t RETRY_AFTER_MS = 10 * 60 * 1000UL;  // after a failed join or download
static const uint8_t FW_KEY_BYTES[16] = FW_KEY;

static Preferences *prefs;
static const char *myVersion;
static String topic, notice;  // notice: the sealed-and-opened JSON of a waiting update
static String want, url, error;
static uint32_t size = 0, nextScan = 0;
static uint8_t fails = 0;
static std::vector<std::pair<String, String>> hotspots;

// "1.10" > "1.9": compare dotted numbers.
static int cmpVersion(const String &a, const String &b) {
  int i = 0, j = 0;
  while (i < (int)a.length() || j < (int)b.length()) {
    long x = 0, y = 0;
    while (i < (int)a.length() && a[i] != '.') x = x * 10 + (a[i++] - '0');
    while (j < (int)b.length() && b[j] != '.') y = y * 10 + (b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    i++, j++;
  }
  return 0;
}

static void take(const String &json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return;
  String v = d["v"] | "";
  hotspots.clear();
  for (JsonArray h : d["h"].as<JsonArray>()) hotspots.push_back({h[0] | "", h[1] | ""});
  if (!v.length() || cmpVersion(v, myVersion) <= 0 || v == prefs->getString("fwbad", "")) {
    want = "";
    return;
  }
  if (v != want) fails = 0, error = "";
  want = v;
  url = d["u"] | "";
  size = d["n"] | 0u;
  nextScan = millis() + 5000;
  Serial.printf("[fetch] firmware %s waiting: looking for an update hotspot every %lu min\n", v.c_str(),
                SCAN_EVERY_MS / 60000);
}

void fetchBegin(Preferences *p, const char *version) {
  prefs = p;
  myVersion = version;
  uint8_t h[32];
  uint8_t in[21];
  memcpy(in, FW_KEY_BYTES, 16);
  memcpy(in + 16, "topic", 5);
  mbedtls_sha256(in, sizeof in, h, 0);
  char tid[21];
  for (int i = 0; i < 10; i++) sprintf(tid + 2 * i, "%02x", h[i]);
  topic = "plugctl/" + String(tid) + "/fw";

  // An update installed by us that did not stick (the bootloader went back to
  // this firmware): don't try that version again.
  String tried = prefs->getString("fwtry", "");
  if (tried.length() && tried != version) {
    prefs->putString("fwbad", tried);
    prefs->remove("fwtry");
    linkNotify("Firmware " + tried + " failed", "It didn't start properly, so the controller went back to " +
               String(version) + ". It won't try " + tried + " again.", 4, "warning");
  }
  notice = prefs->getString("fwnote", "");
  if (notice.length()) take(notice);
}

String fetchTopic() { return topic; }

bool fetchInbound(const String &t, const uint8_t *data, size_t len) {
  if (t != topic) return false;
  if (!len) return true;
  String json;
  if (!openMsg(FW_KEY_BYTES, t, data, len, json)) {
    Serial.println("[fetch] rejected update notice");
    return true;
  }
  if (json != notice) prefs->putString("fwnote", json);
  notice = json;
  take(json);
  return true;
}

String fetchWaiting() { return want; }
String fetchError() { return error; }
String fetchHotspots() {
  String s;
  for (auto &h : hotspots) s += (s.length() ? ", " : "") + h.first;
  return s;
}

static void restoreAp() {
  WiFi.disconnect(false, true);
  delay(200);
  WiFi.softAP(AP_SSID, AP_PASS, 6, 0, 8);  // back to our own channel
}

static bool fail(const String &why) {
  error = why;
  Serial.printf("[fetch] %s\n", why.c_str());
  return false;
}

// Stream url into the other OTA slot, decrypting as it goes. Layout:
// nonce(12) | ciphertext | tag(16), AAD "plugctl-fw <version>".
static bool download() {
  WiFiClientSecure tls;
  tls.setInsecure();  // integrity comes from the GCM tag, not the server
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(tls, url)) return fail("bad update URL");
  int code = http.GET();
  int len = http.getSize();
  if (len <= 0) len = size;  // no Content-Length: trust the notice (the tag still checks it)
  if (code != 200) {
    http.end();
    return fail("download failed (HTTP " + String(code) + ")");
  }
  if (len < 28 + 1024 || (size && (uint32_t)len != size)) {
    http.end();
    return fail("download has the wrong size");
  }
  WiFiClient *s = http.getStreamPtr();
  auto readFull = [&](uint8_t *buf, size_t n) {
    size_t got = 0;
    for (uint32_t t = millis(); got < n && millis() - t < 20000;) {
      int r = s->readBytes(buf + got, n - got);
      if (r > 0) got += r, t = millis();
      else delay(5);
    }
    return got == n;
  };
  uint8_t nonce[12], tag[16], want_tag[16];
  if (!readFull(nonce, 12)) {
    http.end();
    return fail("download cut off");
  }
  size_t ctLen = len - 28;
  if (!Update.begin(ctLen)) {
    http.end();
    return fail(String("can't start install: ") + Update.errorString());
  }
  String aad = "plugctl-fw " + want;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, FW_KEY_BYTES, 128);
  mbedtls_gcm_starts(&gcm, MBEDTLS_GCM_DECRYPT, nonce, 12, (const uint8_t *)aad.c_str(), aad.length());
  static uint8_t in[2048], out[2048];  // multiples of 16, as gcm_update needs
  bool ok = true;
  Serial.printf("[fetch] downloading %s, %u KB\n", want.c_str(), (unsigned)(ctLen / 1024));
  for (size_t done = 0; ok && done < ctLen;) {
    size_t n = min(sizeof in, ctLen - done);
    ok = readFull(in, n) && mbedtls_gcm_update(&gcm, n, in, out) == 0 && Update.write(out, n) == n;
    done += n;
  }
  ok = ok && readFull(tag, 16) && mbedtls_gcm_finish(&gcm, want_tag, 16) == 0;
  mbedtls_gcm_free(&gcm);
  http.end();
  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) diff |= tag[i] ^ want_tag[i];
  if (!ok || diff) {
    Update.abort();
    return fail(ok ? "download failed its check (not our build, or damaged)" : "download cut off");
  }
  if (!Update.end(true)) return fail(String("install failed: ") + Update.errorString());
  return true;
}

void fetchLoop() {
  // The update we installed has proven itself (the trial is 2-10 min).
  static bool settled = false;
  if (!settled && millis() > 11 * 60 * 1000UL) {
    settled = true;
    if (prefs->getString("fwtry", "") == myVersion) prefs->remove("fwtry");
  }
  if (!want.length() || hotspots.empty() || (int32_t)(millis() - nextScan) < 0) return;
  nextScan = millis() + SCAN_EVERY_MS;
  // A scan hops channels for ~2 s; plugs stay joined.
  int n = WiFi.scanNetworks(false, false, false, 120);
  int best = -1, bestRssi = -999;
  for (int i = 0; i < n; i++)
    for (int k = 0; k < (int)hotspots.size(); k++)
      if (WiFi.SSID(i) == hotspots[k].first && WiFi.RSSI(i) > bestRssi) best = k, bestRssi = WiFi.RSSI(i);
  WiFi.scanDelete();
  if (best < 0) return;

  const auto &h = hotspots[best];
  Serial.printf("[fetch] joining %s (%d dBm) for firmware %s\n", h.first.c_str(), bestRssi, want.c_str());
  WiFi.begin(h.first.c_str(), h.second.c_str());
  for (uint32_t t = millis(); millis() - t < 20000 && WiFi.status() != WL_CONNECTED;) delay(100);
  bool ok = WiFi.status() == WL_CONNECTED ? download() : fail("couldn't join " + h.first);
  if (ok) {
    prefs->putString("fwtry", want);
    Serial.printf("[fetch] installed %s, restarting\n", want.c_str());
    delay(300);
    ESP.restart();
  }
  restoreAp();
  nextScan = millis() + RETRY_AFTER_MS;
  if (++fails == 3) linkNotify("Firmware " + want + " update failing", error + ". Still trying every 10 minutes while the hotspot is on.", 3, "warning");
}
