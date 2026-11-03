#include "fwupdate.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include "link.h"
#include "secrets.h"

// Tells the Arduino core not to accept a new firmware at boot; updateLoop()
// does that once it has proven itself. See update.h.
extern "C" bool verifyRollbackLater() { return true; }  // overrides a weak C symbol

static WebServer *srv;
static const char *fwVersion;
static bool pinOk, failed;
static String error;

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width'><title>Firmware update</title>"
    "<style>body{font:16px system-ui;margin:16px;max-width:520px}input,button{font:inherit;padding:10px;"
    "margin:6px 0;width:100%;box-sizing:border-box}button{border-radius:8px}progress{width:100%}"
    ".muted{color:#667}</style><h2>Firmware update</h2>";

static String otherSlotInfo() {
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  esp_app_desc_t d;
  if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) return "";
  return String(d.date) + " " + d.time;
}

static void page() {
  String h = FPSTR(PAGE_HEAD);
  h += "<p>Running firmware " + String(fwVersion) + " (" + esp_ota_get_running_partition()->label + ")</p>";
  h += F("<p class=muted>Pick the firmware.bin you were sent, enter the update PIN and tap Install. "
         "The controller restarts by itself in about a minute. Schedules and settings are kept.</p>"
         "<input id=pin type=password inputmode=numeric placeholder='Update PIN' autocomplete=off>"
         "<input id=file type=file accept='.bin'>"
         "<button id=go>Install</button><progress id=bar value=0 max=1 hidden></progress><p id=msg></p>"
         "<script>"
         "go.onclick=()=>{const f=file.files[0];if(!f||!pin.value){msg.textContent='Pick a file and enter the PIN.';return}"
         "const x=new XMLHttpRequest(),fd=new FormData();fd.append('fw',f);"
         "x.open('POST','/update?pin='+encodeURIComponent(pin.value));bar.hidden=false;go.disabled=true;"
         "x.upload.onprogress=e=>{bar.value=e.loaded/e.total;msg.textContent='Uploading '+Math.round(100*e.loaded/e.total)+'%'};"
         "x.onload=()=>{msg.textContent=x.responseText;go.disabled=false};"
         "x.onerror=()=>{msg.textContent='Upload failed: check you are on the controller Wi-Fi.';go.disabled=false};"
         "x.send(fd)};"
         "</script>");
  String prev = otherSlotInfo();
  if (prev.length())
    h += "<hr><p class=muted>Previous firmware available (built " + prev +
         ").</p><form method=post action='/rollback'><input name=pin type=password inputmode=numeric "
         "placeholder='Update PIN'><button>Switch back to previous firmware</button></form>";
  srv->send(200, "text/html", h);
}

static void restartSoon(const String &msg) {
  srv->send(200, "text/plain", msg);
  delay(500);
  ESP.restart();
}

void updateSetup(WebServer &web, const char *version) {
  srv = &web;
  fwVersion = version;
  web.on("/update", HTTP_GET, page);
  web.on(
      "/update", HTTP_POST,
      [] {
        if (!pinOk) return srv->send(403, "text/plain", "Wrong PIN. Nothing was changed.");
        if (failed) return srv->send(500, "text/plain", "Update failed: " + error + ". The current firmware is still running.");
        restartSoon("Installed. The controller is restarting; reload this page in about a minute.");
      },
      [] {
        HTTPUpload &u = srv->upload();
        if (u.status == UPLOAD_FILE_START) {
          pinOk = srv->arg("pin") == UPDATE_PIN;
          failed = false;
          error = "";
          if (!pinOk) return;
          Serial.printf("[update] receiving %s\n", u.filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) failed = true, error = Update.errorString();
        } else if (u.status == UPLOAD_FILE_WRITE) {
          if (pinOk && !failed && Update.write(u.buf, u.currentSize) != u.currentSize)
            failed = true, error = Update.errorString();
        } else if (u.status == UPLOAD_FILE_END) {
          // end(true) checks the image's checksum before making it bootable.
          if (pinOk && !failed && !Update.end(true)) failed = true, error = Update.errorString();
          Serial.printf("[update] %s, %u bytes\n", failed ? error.c_str() : "ok", (unsigned)u.totalSize);
        } else if (u.status == UPLOAD_FILE_ABORTED) {
          if (pinOk) Update.abort();
          failed = true, error = "upload interrupted";
        }
      });
  web.on("/rollback", HTTP_POST, [] {
    if (srv->arg("pin") != UPDATE_PIN) return srv->send(403, "text/plain", "Wrong PIN. Nothing was changed.");
    const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
    if (!other || esp_ota_set_boot_partition(other) != ESP_OK)
      return srv->send(500, "text/plain", "No usable previous firmware.");
    restartSoon("Switching back to the previous firmware; reload this page in about a minute.");
  });
}

void updateLoop() {
  static bool accepted = false;
  if (accepted) return;
  uint32_t up = millis();
  if ((linkInfo().online && up > 120000) || up > 600000) {
    esp_ota_mark_app_valid_cancel_rollback();  // harmless when not on trial
    accepted = true;
    Serial.println("[update] firmware accepted");
  }
}
