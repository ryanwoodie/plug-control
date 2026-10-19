#include "shelly.h"
#include <HTTPClient.h>
#include <WiFi.h>

bool shellyRpc(const String &ip, const char *method, JsonDocument &params, JsonDocument *result, uint16_t timeoutMs) {
  JsonDocument req;
  req["id"] = 1;
  req["method"] = method;
  req["params"] = params;
  String body;
  serializeJson(req, body);

  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin("http://" + ip + "/rpc")) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    JsonDocument resp;
    if (!deserializeJson(resp, http.getString()) && !resp["error"].is<JsonObject>()) {
      ok = true;
      if (result) *result = resp["result"];
    } else {
      Serial.printf("[shelly] %s %s error\n", ip.c_str(), method);
    }
  }
  http.end();
  return ok;
}

bool shellyGetStatus(const String &ip, ShellyStatus &st) {
  JsonDocument p, r;
  p["id"] = 0;
  if (!shellyRpc(ip, "Switch.GetStatus", p, &r, 1500)) return false;
  st.on = r["output"] | false;
  st.watts = r["apower"] | 0.0f;
  st.volts = r["voltage"] | 0.0f;
  st.wh = r["aenergy"]["total"] | 0.0f;
  return true;
}

bool shellySet(const String &ip, bool on, uint32_t toggleAfterS) {
  JsonDocument p;
  p["id"] = 0;
  p["on"] = on;
  if (toggleAfterS) p["toggle_after"] = toggleAfterS;
  return shellyRpc(ip, "Switch.Set", p);
}

bool shellyProvision(const String &setupSsid, const String &staticIp, const char *ourSsid, const char *ourPass) {
  Serial.printf("[shelly] provisioning %s -> %s\n", setupSsid.c_str(), staticIp.c_str());
  WiFi.begin(setupSsid.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    return false;
  }

  JsonDocument p;
  JsonObject sta = p["config"]["sta"].to<JsonObject>();
  sta["ssid"] = ourSsid;
  sta["pass"] = ourPass;
  sta["enable"] = true;
  sta["ipv4mode"] = "static";
  sta["ip"] = staticIp;
  sta["netmask"] = "255.255.255.0";
  sta["gw"] = "192.168.4.1";
  sta["nameserver"] = "192.168.4.1";
  bool ok = shellyRpc("192.168.33.1", "WiFi.SetConfig", p, nullptr, 4000);
  WiFi.disconnect();
  Serial.printf("[shelly] provision %s\n", ok ? "sent" : "failed");
  return ok;
}

bool shellyHarden(const String &ip, const char *name) {
  JsonDocument ap;
  ap["config"]["ap"]["enable"] = false;
  JsonDocument ble;
  ble["config"]["enable"] = false;
  JsonDocument sw;
  sw["id"] = 0;
  sw["config"]["initial_state"] = "off";  // after a power cut the board re-applies schedules
  sw["config"]["name"] = name;
  JsonDocument sys;
  sys["config"]["device"]["name"] = name;
  bool ok = shellyRpc(ip, "Switch.SetConfig", sw) & shellyRpc(ip, "Sys.SetConfig", sys) &
            shellyRpc(ip, "BLE.SetConfig", ble) & shellyRpc(ip, "WiFi.SetConfig", ap);
  Serial.printf("[shelly] harden %s %s\n", ip.c_str(), ok ? "ok" : "partial");
  return ok;
}
