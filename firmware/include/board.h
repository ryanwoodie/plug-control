// LilyGo T-A7670G (ESP32) pin map and system-wide settings.
#pragma once

#define MODEM_TX_PIN 26
#define MODEM_RX_PIN 27
#define MODEM_PWRKEY_PIN 4
#define BOARD_POWER_ON_PIN 12
#define MODEM_RESET_PIN 5
#define MODEM_DTR_PIN 25

#define GPS_RX_PIN 22  // ESP32 receives NMEA here
#define GPS_TX_PIN 21
#define GPS_WAKE_PIN 19

#define APN "iot.1nce.net"

// Public MQTT broker. Payloads are end-to-end encrypted, so the broker only
// relays opaque bytes. Swap for a private broker by changing these two lines
// (and BROKER_WS in docs/app.js).
#define MQTT_HOST "broker.hivemq.com"
#define MQTT_PORT 1883

// Data-budget knobs. 500 MB / 10 years is about 137 KB/day.
// The 1NCE path drops idle TCP connections after 4-6 min (measured
// 2026-10-19: 240 s idle survived, 360 s did not), so ping inside that.
// Each ping is ~160 B on the SIM: ~60 KB/day, the main idle cost.
#define MQTT_KEEPALIVE_S 225
#define HEARTBEAT_S (6 * 3600)             // unsolicited full status
#define DAILY_CAP_BYTES (150UL * 1024)     // estimated; link goes quiet until tomorrow
#define MONTHLY_CAP_BYTES (3000UL * 1024)  // estimated; link goes quiet until next month
#define CMD_MAX_AGE_S 900                  // ignore manual commands older than this

#define PLUG_IP_BASE 200  // plug n gets 192.168.4.(200+n)
