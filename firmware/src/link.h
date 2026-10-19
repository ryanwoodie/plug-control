// Cellular link: A7670 modem + one long-lived MQTT session, run in its own
// FreeRTOS task so the plug logic never blocks on the network.
#pragma once
#include <Arduino.h>
#include <vector>

struct LinkInfo {
  bool online;   // MQTT session up
  int csq;       // 0-31, 99 = unknown
  String op;     // operator MCC/MNC
  uint32_t today, month, total;  // estimated bytes on the SIM
  bool capped;   // data cap reached; link idle until the period rolls over
};

void linkBegin(const std::vector<String> &subscribeTopics);
void linkPublish(const String &topic, std::vector<uint8_t> &&payload);
bool linkReceive(String &topic, std::vector<uint8_t> &payload);
LinkInfo linkInfo();

// Implemented by the app: called with UTC from the cell network clock.
void noteTime(time_t utc, const char *source);
