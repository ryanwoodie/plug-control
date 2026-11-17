// Cellular link: A7670 modem + one long-lived MQTT session, run in its own
// FreeRTOS task so the plug logic never blocks on the network.
#pragma once
#include <Arduino.h>
#include <vector>

// What today's data went on, so a spike can be explained afterwards.
// Counts of messages the app sent, by reason (see linkCount).
enum Usage : uint8_t {
  U_REF,     // status report asked for by the web app's refresh
  U_CMD,     // report after a command, settings change or switch
  U_FOLLOW,  // follow-up report with the power reading after switching on
  U_PLUG,    // report because a plug dropped off Wi-Fi or came back
  U_BEAT,    // 6-hourly heartbeat
  U_HIST,    // history batch (incl. resends)
  U_HELD,    // optional message held back by the data reserve
  U_COUNT
};

struct DayUse {
  uint32_t conn;  // TCP + MQTT connection setup
  uint32_t tx;    // bytes we sent (payload + MQTT headers)
  uint32_t rx;    // bytes we received
  uint32_t pkt;   // per-packet overhead (IP/TCP headers, ACKs)
  uint32_t ka;    // modem TCP keep-alive probes
  uint16_t sess;  // MQTT sessions opened
  uint16_t fresh; // ... of which the broker had forgotten us (re-download)
  uint16_t n[U_COUNT];
};

struct LinkInfo {
  bool online;   // MQTT session up
  int csq;       // 0-31, 99 = unknown
  String op;     // operator MCC/MNC
  uint32_t today, month, total;  // estimated bytes on the SIM
  bool capped;   // data cap reached; link idle until the period rolls over
  bool saving;   // past the data reserve: only essential messages go out
  DayUse use;    // today's breakdown (UTC day, like `today`)
};

void linkBegin(const std::vector<String> &subscribeTopics);
void linkPublish(const String &topic, std::vector<uint8_t> &&payload);
bool linkReceive(String &topic, std::vector<uint8_t> &payload);
LinkInfo linkInfo();
void linkCount(Usage u);

// Implemented by the app: called with UTC from the cell network clock.
void noteTime(time_t utc, const char *source);
