#include "kasa.h"
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <vector>

bool kasaCmd(const String &ip, JsonDocument &req, JsonDocument *resp, uint16_t timeoutMs) {
  String body;
  serializeJson(req, body);
  // Autokey XOR, key starts at 171, behind a 4-byte big-endian length.
  std::vector<uint8_t> buf(4 + body.length());
  uint32_t n = body.length();
  buf[0] = n >> 24, buf[1] = n >> 16, buf[2] = n >> 8, buf[3] = n;
  uint8_t k = 171;
  for (uint32_t i = 0; i < n; i++) buf[4 + i] = k = k ^ (uint8_t)body[i];

  WiFiClient c;
  if (!c.connect(ip.c_str(), 9999, timeoutMs)) return false;
  c.setTimeout(timeoutMs / 1000 + 1);
  c.write(buf.data(), buf.size());

  uint8_t hdr[4];
  bool ok = false;
  if (c.readBytes(hdr, 4) == 4) {
    uint32_t len = (uint32_t)hdr[0] << 24 | hdr[1] << 16 | hdr[2] << 8 | hdr[3];
    if (len && len < 4096) {
      std::vector<uint8_t> d(len);
      if (c.readBytes(d.data(), len) == len) {
        k = 171;
        String s;
        s.reserve(len);
        for (auto b : d) {
          s += (char)(k ^ b);
          k = b;
        }
        JsonDocument r;
        if (!deserializeJson(r, s)) {
          // Every module/method answers with its own err_code.
          ok = true;
          for (JsonPair mod : r.as<JsonObject>())
            for (JsonPair m : mod.value().as<JsonObject>())
              if ((m.value()["err_code"] | 0) != 0) {
                Serial.printf("[kasa] %s %s.%s error %d\n", ip.c_str(), mod.key().c_str(), m.key().c_str(),
                              m.value()["err_code"] | 0);
                ok = false;
              }
          if (resp) *resp = r;
        }
      }
    }
  }
  c.stop();
  return ok;
}

bool kasaGetStatus(const String &ip, KasaStatus &st) {
  JsonDocument q, r;
  q["system"]["get_sysinfo"].to<JsonObject>();
  q["time"]["get_time"].to<JsonObject>();
  if (!kasaCmd(ip, q, &r, 1500)) return false;
  st.on = (r["system"]["get_sysinfo"]["relay_state"] | 0) == 1;
  st.clockSet = (r["time"]["get_time"]["year"] | 0) >= 2020;  // unset reads as 2000
  return true;
}

bool kasaSetClock(const String &ip) {
  // Only countdowns are used, so any running clock will do: real UTC when we
  // have it, else a fixed date that still counts as set.
  time_t now = time(nullptr);
  if (now < 1700000000) now = 1735689600 + millis() / 1000;
  struct tm t;
  gmtime_r(&now, &t);
  JsonDocument q;
  JsonObject z = q["time"]["set_timezone"].to<JsonObject>();
  z["year"] = t.tm_year + 1900;
  z["month"] = t.tm_mon + 1;
  z["mday"] = t.tm_mday;
  z["hour"] = t.tm_hour;
  z["min"] = t.tm_min;
  z["sec"] = t.tm_sec;
  z["index"] = 39;  // timezone is irrelevant for countdowns
  return kasaCmd(ip, q);
}

bool kasaSet(const String &ip, bool on, uint32_t offAfterS) {
  // Relay first: switching the relay by hand disables a countdown, so it must
  // not come after the new rule.
  JsonDocument q;
  q["system"]["set_relay_state"]["state"] = on ? 1 : 0;
  q["count_down"]["delete_all_rules"].to<JsonObject>();
  if (on && offAfterS) {
    JsonObject r = q["count_down"]["add_rule"].to<JsonObject>();
    r["enable"] = 1;
    r["delay"] = offAfterS;
    r["act"] = 0;
    r["name"] = "max";
  }
  if (kasaCmd(ip, q)) return true;
  // Never leave it on without the timer it was meant to have.
  if (on && offAfterS) {
    JsonDocument off;
    off["system"]["set_relay_state"]["state"] = 0;
    kasaCmd(ip, off);
  }
  return false;
}

String kasaFindIp(const char *mac) {
  wifi_sta_list_t wl;
  esp_netif_sta_list_t nl;
  if (esp_wifi_ap_get_sta_list(&wl) != ESP_OK || esp_netif_get_sta_list(&wl, &nl) != ESP_OK) return "";
  for (int i = 0; i < nl.num; i++) {
    char m[13];
    const uint8_t *b = nl.sta[i].mac;
    snprintf(m, sizeof m, "%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
    if (!strcmp(m, mac) && nl.sta[i].ip.addr) return IPAddress(nl.sta[i].ip.addr).toString();
  }
  return "";
}

bool kasaProvision(const String &setupSsid, const char *ourSsid, const char *ourPass) {
  Serial.printf("[kasa] provisioning %s\n", setupSsid.c_str());
  WiFi.begin(setupSsid.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    return false;
  }
  kasaSetClock("192.168.0.1");
  JsonDocument q;
  JsonObject s = q["netif"]["set_stainfo"].to<JsonObject>();
  s["ssid"] = ourSsid;
  s["password"] = ourPass;
  s["key_type"] = 3;  // WPA2
  bool ok = kasaCmd("192.168.0.1", q, nullptr, 4000);
  WiFi.disconnect();
  Serial.printf("[kasa] provision %s\n", ok ? "sent" : "failed");
  return ok;
}

bool kasaHarden(const String &ip, const char *name) {
  JsonDocument q;
  q["system"]["set_dev_alias"]["alias"] = name;
  bool ok = kasaCmd(ip, q);
  Serial.printf("[kasa] harden %s %s\n", ip.c_str(), ok ? "ok" : "failed");
  return ok;
}
