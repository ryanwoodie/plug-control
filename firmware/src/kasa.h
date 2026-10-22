// Local TP-Link Kasa (HS103 etc.) control: the legacy XOR protocol on TCP 9999.
// Firmware that switched to KLAP (port 80) is not supported.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct KasaStatus {
  bool on;
  bool clockSet;  // false after a power cut: the plug has no clock source of its own
};

bool kasaCmd(const String &ip, JsonDocument &req, JsonDocument *resp = nullptr, uint16_t timeoutMs = 2000);
bool kasaGetStatus(const String &ip, KasaStatus &st);
// The countdown (the plug's own max-on timer) only runs once the clock is set.
bool kasaSetClock(const String &ip);
bool kasaSet(const String &ip, bool on, uint32_t offAfterS = 0);

// Kasa plugs take DHCP addresses only: look one up by MAC on our AP.
String kasaFindIp(const char *mac);
// Joins a factory-fresh plug's open setup AP and points it at our network.
bool kasaProvision(const String &setupSsid, const char *ourSsid, const char *ourPass);
bool kasaHarden(const String &ip, const char *name);
