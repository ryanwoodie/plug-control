#include "link.h"
#include <Preferences.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include "board.h"

#define SerialAT Serial1

struct Msg {
  String topic;
  std::vector<uint8_t> data;
};

static TinyGsm modem(SerialAT);
static TinyGsmClient gsm(modem, 0);
static QueueHandle_t outQ, inQ;
static SemaphoreHandle_t infoLock;
static LinkInfo info{false, 99, "", 0, 0, 0, false};
static std::vector<String> subs;
static Preferences store;

// ---- data meter --------------------------------------------------------
// The modem cannot report billed bytes, so we estimate: payload bytes plus
// ~52 B per IP packet and its ACK, and a fixed cost per TCP connection.
static uint32_t meterDay = 0, meterMonth = 0, lastMeterSave = 0;
// Breakdown since boot, for the serial log.
static uint32_t dbgConn, dbgTx, dbgRx, dbgOvh, dbgReconnects, dbgPub, dbgRecv;

static void meterAdd(uint32_t n) {
  xSemaphoreTake(infoLock, portMAX_DELAY);
  info.today += n;
  info.month += n;
  info.total += n;
  xSemaphoreGive(infoLock);
}

static void meterSave() {
  store.putUInt("day", meterDay);
  store.putUInt("mon", meterMonth);
  store.putUInt("today", info.today);
  store.putUInt("month", info.month);
  store.putUInt("total", info.total);
  lastMeterSave = millis();
}

static void meterRoll() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;  // no clock yet
  struct tm t;
  gmtime_r(&now, &t);
  uint32_t day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
  uint32_t mon = day / 100;
  if (day != meterDay || mon != meterMonth) {
    xSemaphoreTake(infoLock, portMAX_DELAY);
    if (day != meterDay) info.today = 0;
    if (mon != meterMonth) info.month = 0;
    xSemaphoreGive(infoLock);
    meterDay = day;
    meterMonth = mon;
    meterSave();
  }
  if (millis() - lastMeterSave > 30 * 60 * 1000UL) meterSave();
}

class MeteredClient : public Client {
 public:
  explicit MeteredClient(Client &c) : c(c) {}
  int connect(IPAddress ip, uint16_t port) override { meterAdd(400); dbgConn += 400; return c.connect(ip, port); }
  int connect(const char *host, uint16_t port) override { meterAdd(550); dbgConn += 550; return c.connect(host, port); }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t n) override {
    meterAdd(n + 104);
    dbgTx += n;
    dbgOvh += 104;
    return c.write(buf, n);
  }
  int available() override {
    int a = c.available();
    if (a > 0 && !burst) {
      meterAdd(104);
      dbgOvh += 104;
    }
    burst = a > 0;
    return a;
  }
  int read() override {
    int r = c.read();
    if (r >= 0) {
      meterAdd(1);
      dbgRx++;
    }
    return r;
  }
  int read(uint8_t *buf, size_t n) override {
    int r = c.read(buf, n);
    if (r > 0) {
      meterAdd(r);
      dbgRx += r;
    }
    return r;
  }
  int peek() override { return c.peek(); }
  void flush() override { c.flush(); }
  void stop() override { c.stop(); }
  uint8_t connected() override { return c.connected(); }
  operator bool() override { return c.connected(); }

 private:
  Client &c;
  bool burst = false;
};

static MeteredClient metered(gsm);
static PubSubClient mqtt(metered);

// ---- modem ---------------------------------------------------------------
static bool modemAlive() {
  for (int i = 0; i < 5; i++) {
    if (modem.testAT(1000)) return true;
  }
  return false;
}

static bool modemWasOn = false;

static bool modemPowerOn() {
  pinMode(BOARD_POWER_ON_PIN, OUTPUT);
  digitalWrite(BOARD_POWER_ON_PIN, HIGH);
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  if (modemAlive()) {  // already on (ESP32 restarted alone)
    modemWasOn = true;
    return true;
  }
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  for (int i = 0; i < 20; i++) {
    if (modem.testAT(1000)) return true;
  }
  return false;
}

static time_t civilToUtc(int y, int mo, int d, int h, int mi, int s) {
  y -= mo <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468;
  return days * 86400L + h * 3600 + mi * 60 + s;
}

static void syncNetworkTime() {
  int y, mo, d, h, mi, s;
  float tz;
  // The driver reports the CCLK zone field (quarter-hours) as "hours". An
  // unset modem clock reads 70/01/01, which the driver turns into 2070.
  if (modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz) && y >= 2024 && y < 2060) {
    noteTime(civilToUtc(y, mo, d, h, mi, s) - (time_t)(tz * 15 * 60), "net");
  }
}

static void readNetInfo() {
  int csq = modem.getSignalQuality();
  String op;
  modem.sendAT(GF("+COPS?"));
  if (modem.waitResponse(3000, GF("+COPS:")) == 1) {
    String line = modem.stream.readStringUntil('\n');
    int a = line.indexOf('"'), b = line.indexOf('"', a + 1);
    if (a >= 0 && b > a) op = line.substring(a + 1, b);
    modem.waitResponse();
  }
  xSemaphoreTake(infoLock, portMAX_DELAY);
  info.csq = csq;
  info.op = op;
  xSemaphoreGive(infoLock);
}

static void setOnline(bool v) {
  xSemaphoreTake(infoLock, portMAX_DELAY);
  info.online = v;
  xSemaphoreGive(infoLock);
}

// ---- MQTT ----------------------------------------------------------------
static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  dbgRecv++;
  Serial.printf("[link] recv %u B on %s\n", len, topic);
  Msg *m = new Msg{String(topic), std::vector<uint8_t>(payload, payload + len)};
  if (xQueueSend(inQ, &m, 0) != pdTRUE) delete m;
}

static bool mqttConnect() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char id[24];
  snprintf(id, sizeof id, "pc-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(30);
  mqtt.setBufferSize(2048);
  mqtt.setCallback(onMessage);
  // Persistent session: the broker keeps our subscriptions and queues QoS 1
  // messages while we are away, so a reconnect costs a bare CONNECT instead
  // of re-subscribing and re-downloading every retained message.
  if (!mqtt.connect(id, nullptr, nullptr, nullptr, 0, false, nullptr, false)) {
    Serial.printf("[link] mqtt connect failed state=%d\n", mqtt.state());
    return false;
  }
  if (!mqtt.sessionPresent()) {
    for (auto &t : subs) mqtt.subscribe(t.c_str(), 1);
  }
  dbgReconnects++;
  Serial.printf("[link] mqtt online (session %s)\n", mqtt.sessionPresent() ? "resumed" : "new");
  return true;
}

static bool capReached() {
  xSemaphoreTake(infoLock, portMAX_DELAY);
  bool c = info.today >= DAILY_CAP_BYTES || info.month >= MONTHLY_CAP_BYTES;
  info.capped = c;
  xSemaphoreGive(infoLock);
  return c;
}

static void linkTask(void *) {
  uint32_t nextTry = 0, lastNetInfo = 0, lastTimeSync = 0;
  int fails = 0;
  bool dataOff = false, wasOnline = false;
  uint32_t onlineSince = 0;

  while (!modemPowerOn()) {
    Serial.println("[link] modem not responding, retrying in 60s");
    delay(60000);
  }
  modem.init();
  if (modemWasOn) modem.gprsDisconnect();  // drop the previous boot's sockets
  modem.sendAT(GF("+CGDCONT=1,\"IP\",\"" APN "\""));
  modem.waitResponse();
  Serial.printf("[link] modem %s\n", modem.getModemName().c_str());

  for (;;) {
    meterRoll();

    static uint32_t lastDbg = 0;
    if (!lastDbg || millis() - lastDbg > 10 * 60 * 1000UL) {
      lastDbg = millis();
      Serial.printf("[meter] up=%lus online=%d today=%u month=%u | boot: conn=%u tx=%u rx=%u ovh=%u sessions=%u pub=%u recv=%u\n",
                    millis() / 1000, mqtt.connected(), info.today, info.month, dbgConn, dbgTx, dbgRx, dbgOvh,
                    dbgReconnects, dbgPub, dbgRecv);
    }

    if (capReached()) {
      // Close the data session but stay registered: registration uses no
      // data and keeps the network clock coming, which the cap needs to know
      // when the day rolls over (radio off here deadlocked after a reboot).
      if (!dataOff) {
        Serial.println("[link] data cap reached, data session closed until the period rolls over");
        if (mqtt.connected()) mqtt.disconnect();
        modem.gprsDisconnect();
        setOnline(false);
        meterSave();
        dataOff = true;
        wasOnline = false;
      }
      if (time(nullptr) < 1700000000 && modem.isNetworkConnected()) syncNetworkTime();
      delay(60000);
      continue;
    }
    if (dataOff) {
      Serial.println("[link] data cap cleared");
      dataOff = false;
    }

    if (!mqtt.connected()) {
      if (wasOnline) {
        Serial.printf("[link] mqtt lost after %lus online (state=%d)\n", (millis() - onlineSince) / 1000, mqtt.state());
        wasOnline = false;
      }
      setOnline(false);
      if ((int32_t)(millis() - nextTry) < 0) {
        delay(250);
        continue;
      }
      bool ok = false;
      if (!modem.isNetworkConnected()) {
        Serial.println("[link] waiting for network");
        modem.waitForNetwork(120000L);
      }
      if (modem.isNetworkConnected()) {
        if (!lastTimeSync || millis() - lastTimeSync > 24 * 3600 * 1000UL || time(nullptr) < 1700000000) {
          syncNetworkTime();
          lastTimeSync = millis();
        }
        if (!modem.isGprsConnected()) {
          Serial.println("[link] opening data session");
          modem.gprsConnect(APN);
        }
        if (modem.isGprsConnected()) {
          // Applies to sockets opened afterwards; only accepted once the data
          // session is up, and not saved, so set it before every connect.
          modem.sendAT(GF("+CTCPKA=1,"), TCP_KEEPIDLE_MIN, GF(",3,1"));
          modem.waitResponse(9000L);
        }
        ok = modem.isGprsConnected() && mqttConnect();
      }
      if (ok) {
        fails = 0;
        setOnline(true);
        wasOnline = true;
        onlineSince = millis();
        readNetInfo();
        lastNetInfo = millis();
      } else {
        fails++;
        uint32_t backoff = min<uint32_t>(30000UL << min(fails, 6), 30 * 60 * 1000UL);
        nextTry = millis() + backoff;
        Serial.printf("[link] connect failed (%d), retry in %lus\n", fails, backoff / 1000);
        if (fails % 4 == 0) {
          Serial.println("[link] resetting radio");
          modem.gprsDisconnect();
          modem.setPhoneFunctionality(0);
          delay(2000);
          modem.setPhoneFunctionality(1);
        }
      }
      continue;
    }

    mqtt.loop();
    Msg *m;
    while (xQueueReceive(outQ, &m, 0) == pdTRUE) {
      mqtt.publish(m->topic.c_str(), m->data.data(), m->data.size(), true);
      dbgPub++;
      Serial.printf("[link] sent %u B on %s\n", (unsigned)m->data.size(), m->topic.c_str());
      delete m;
    }
    if (millis() - lastNetInfo > 10 * 60 * 1000UL) {
      if (time(nullptr) < 1700000000) {
        syncNetworkTime();
        // Still no clock (no NITZ while roaming, no GPS fix indoors): one NTP
        // exchange is ~200 B on the SIM.
        if (time(nullptr) < 1700000000) {
          modem.sendAT(GF("+CNTP=\"pool.ntp.org\",0"));
          modem.waitResponse();
          modem.sendAT(GF("+CNTP"));
          if (modem.waitResponse(15000L, GF("+CNTP: 0")) == 1) syncNetworkTime();
        }
      }
      readNetInfo();
      lastNetInfo = millis();
    }
    delay(20);
  }
}

void linkBegin(const std::vector<String> &subscribeTopics) {
  subs = subscribeTopics;
  outQ = xQueueCreate(8, sizeof(Msg *));
  inQ = xQueueCreate(8, sizeof(Msg *));
  infoLock = xSemaphoreCreateMutex();
  store.begin("data");
  meterDay = store.getUInt("day");
  meterMonth = store.getUInt("mon");
  info.today = store.getUInt("today");
  info.month = store.getUInt("month");
  info.total = store.getUInt("total");
  // Idle priority: TinyGSM busy-waits (up to 75 s on a socket open) and would
  // otherwise starve IDLE0 and trip the task watchdog.
  xTaskCreatePinnedToCore(linkTask, "link", 12288, nullptr, tskIDLE_PRIORITY, nullptr, 0);
}

void linkPublish(const String &topic, std::vector<uint8_t> &&payload) {
  Msg *m = new Msg{topic, std::move(payload)};
  if (xQueueSend(outQ, &m, 0) != pdTRUE) delete m;
}

bool linkReceive(String &topic, std::vector<uint8_t> &payload) {
  Msg *m;
  if (xQueueReceive(inQ, &m, 0) != pdTRUE) return false;
  topic = std::move(m->topic);
  payload = std::move(m->data);
  delete m;
  return true;
}

LinkInfo linkInfo() {
  xSemaphoreTake(infoLock, portMAX_DELAY);
  LinkInfo copy = info;
  xSemaphoreGive(infoLock);
  return copy;
}
