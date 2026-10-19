// Local Shelly Gen2+ RPC over the board's own Wi-Fi network.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct ShellyStatus {
  bool on;
  float watts, volts, wh;
};

bool shellyRpc(const String &ip, const char *method, JsonDocument &params, JsonDocument *result = nullptr,
               uint16_t timeoutMs = 2000);
bool shellyGetStatus(const String &ip, ShellyStatus &st);
bool shellySet(const String &ip, bool on, uint32_t toggleAfterS = 0);

// Joins a factory-fresh plug's setup AP and points it at our network with a
// fixed IP. The plug's own AP stays up until shellyHarden() confirms it joined.
bool shellyProvision(const String &setupSsid, const String &staticIp, const char *ourSsid, const char *ourPass);
// Run once the plug is reachable on our network: turn off its AP and BLE.
bool shellyHarden(const String &ip, const char *name);
