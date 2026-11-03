// Plug history feed. Each plug's change log (histlog.h) goes out in sealed
// batches, about hourly, to retained topics plugctl/<tid>/h/<seq>. A GitHub
// Action (tools/collect_history.py) files them permanently and clears them,
// and asks for any missing seq on plugctl/<tid>/hr, sealed with the owner's
// history key (which can request resends but cannot read the batches).
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

void histBegin(Preferences *prefs);
std::vector<String> histTopics();  // to subscribe
// One poll of plug i. ok=false only when the plug is declared unreachable.
void histSample(int plug, bool ok, bool on, float watts);
void histLoop();
// Returns true if the message was for history (handled or rejected).
bool histInbound(const String &topic, const uint8_t *data, size_t len);
