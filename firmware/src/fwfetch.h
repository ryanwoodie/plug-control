// Firmware updates over a phone hotspot, only when one is waiting.
//
// tools/publish_fw.py encrypts a build with FW_KEY, puts it on GitHub Pages
// (docs/fw/) and publishes a sealed notice to the retained topic
// plugctl/<fw tid>/fw: {"v": version, "n": bytes, "u": url, "h": [[ssid, pass], ...]}.
// The notice reaches the controller over the cell link (a few hundred bytes,
// once). Only while a newer version is waiting does it scan for one of the
// listed hotspots, every 2 minutes. When it sees one it joins, downloads over
// the hotspot (no SIM data), checks the AES-GCM tag (so only our own builds
// install, whatever the hotspot or server), installs and restarts. The new
// firmware is on trial as with the /update page: it rolls back if it fails,
// and then this version is not tried again.
//
// Joining the hotspot moves the controller's own Wi-Fi to the hotspot's
// channel, so plugs drop off for a minute or two; their own timers keep
// running.
#pragma once
#include <Arduino.h>
#include <Preferences.h>

void fetchBegin(Preferences *prefs, const char *version);
String fetchTopic();  // to subscribe
bool fetchInbound(const String &topic, const uint8_t *data, size_t len);
void fetchLoop();
// For the status report: the version waiting ("" if none), its hotspot
// names, and the last problem ("" if none).
String fetchWaiting();
String fetchHotspots();
String fetchError();
