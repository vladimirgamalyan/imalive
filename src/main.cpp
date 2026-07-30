// imalive — an "I'm alive" heartbeat device on ESP32-C3 SuperMini.
//
// On a genuine power-on the device connects to WiFi, syncs time over NTP, and
// sends an "I'm alive" message to a Telegram chat with a notification. While it
// stays powered it edits that same message every HEARTBEAT_MIN minutes (a silent
// heartbeat). The message_id is kept in NVS, so a soft reboot (watchdog / OTA)
// or a planned muted restart silently resumes the same message instead of
// pinging again. When power is lost the device dies and the message freezes at
// its last "Last seen" value. See CONCEPT.md and docs/adr/ for the rationale.
//
// Firmware updates over the air: an admin sends the new .bin as a Telegram
// document captioned /update; a boot-count watchdog rolls back to the previous
// image if the new one fails to confirm (ADR-0013).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

#include "config.h"

// Default the alignment flag on if a pre-existing config.h predates it.
#ifndef HEARTBEAT_ALIGN
#define HEARTBEAT_ALIGN 1
#endif

// Fixed tunables (not exposed in config.h).
static const char* NTP_SERVER = "pool.ntp.org";
static const uint32_t WIFI_BLINK_MS = 250;      // status-LED blink period while connecting
static const uint32_t NTP_TIMEOUT_MS = 30000;
static const uint32_t WDT_TIMEOUT_S = 60;
static const uint32_t HEARTBEAT_MS = (uint32_t)HEARTBEAT_MIN * 60UL * 1000UL;
static const uint32_t CMD_POLL_MS = 4000;       // /status command poll interval
// Cap on the TLS handshake. The Arduino core defaults to 120 s, twice the
// watchdog period, so a stalled handshake (link up but no packets getting
// through) reboots the device instead of failing the request.
static const uint32_t TLS_HANDSHAKE_S = 10;

// Heartbeat edits are phase-locked to wall-clock multiples of HEARTBEAT_MIN when
// HEARTBEAT_ALIGN is on and the interval divides an hour evenly (so 5/10/15/30/60
// tile the hour cleanly: "Last seen" lands on :00, :05, :10, ...). A boundary
// falling within HEARTBEAT_GUARD_S of the previous update is skipped to the next
// one, so a power-on never fires two edits seconds apart. 30 s is a wide margin
// under Telegram's ~1 request/second-per-chat limit.
static const bool HEARTBEAT_ALIGNED = (HEARTBEAT_ALIGN != 0) && (60 % HEARTBEAT_MIN == 0);
static const int  HEARTBEAT_GUARD_S = 30;

static const char* FW_VERSION  = "0.2.3";
static const char* BUILD_STAMP = __DATE__ " " __TIME__;  // set at compile time

// Telegram user IDs allowed to issue commands (config.h); others are ignored.
static const long long g_adminIds[] = TG_ADMIN_IDS;
static const size_t g_adminCount = sizeof(g_adminIds) / sizeof(g_adminIds[0]);

// Runtime state. The tracked message_id, the session's "online since", and the
// startup-ping mute flag survive reboots in NVS (see the Persistent state
// section); everything else is RAM only.
static long g_messageId = -1;   // Telegram message id of the tracked message (NVS)
static String g_onlineSince;    // local-time string of the session start
static time_t g_onlineSinceEpoch = 0;  // session start epoch, persistent across reboots (NVS)
static time_t g_bootEpoch = 0;  // epoch of this boot, for uptime-since-boot
static bool g_muted = true;     // if set, a power-on resumes silently — no ping (NVS); muted by default
static uint32_t g_lastHeartbeatMs = 0;
static time_t g_nextHeartbeatEpoch = 0;  // next aligned heartbeat, wall-clock (RAM; aligned mode only)
static long g_updateOffset = 0; // getUpdates acknowledge cursor (RAM only)
static uint32_t g_lastPollMs = 0;

// OTA confirmation state (ADR-0013). A flashed update must confirm itself
// within OTA_MAX_BOOTS boots or the previous image is restored.
enum : uint8_t { OTA_IDLE = 0, OTA_PENDING = 1, OTA_ROLLED_BACK = 2 };
static uint8_t g_otaState = OTA_IDLE;   // NVS
static long long g_otaChat = 0;         // chat that initiated the update (NVS)

// Boot diagnostics (NVS). Each boot stores its own epoch, so the next one can
// report how long this session lasted, and a watchdog reset bumps a lifetime
// counter. Both are reported in the admin boot notice: a single unlucky network
// stall looks the same as a systematic hang without them.
static time_t g_prevBootEpoch = 0;   // epoch of the previous boot (NVS)
static uint32_t g_wdtReboots = 0;    // lifetime count of watchdog reboots (NVS)

// ---------------------------------------------------------------------------
// Status LED
// Onboard blue LED on the ESP32-C3 SuperMini: GPIO8, active-low (LOW = lit).
// Blinks while connecting to WiFi, solid on once connected.
// ---------------------------------------------------------------------------

static const int  LED_PIN = 8;
static const bool LED_ACTIVE_LOW = true;

static void ledSet(bool on) {
  bool level = LED_ACTIVE_LOW ? !on : on;
  digitalWrite(LED_PIN, level ? HIGH : LOW);
}

static void ledToggle() {
  static bool state = false;
  state = !state;
  ledSet(state);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

static String formatLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String("--:-- -- ---");
  }
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M %d %b", &timeinfo);
  return String(buf);
}

// Format an arbitrary epoch as local "dd.mm HH:MM" (for a stored session start).
static String formatLocalTimeAt(time_t t) {
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M %d %b", &timeinfo);
  return String(buf);
}

// Human-readable "Xd Xh Xm" from a whole-second span. uint32_t seconds spans
// ~136 years, so this never wraps in practice.
static String formatDuration(uint32_t seconds) {
  uint32_t d = seconds / 86400; seconds %= 86400;
  uint32_t h = seconds / 3600;  seconds %= 3600;
  uint32_t m = seconds / 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "%lud %luh %lum",
           (unsigned long)d, (unsigned long)h, (unsigned long)m);
  return String(buf);
}

// Human-readable span from an epoch timestamp up to now. Uses wall-clock time,
// so it does not suffer the ~49-day millis() wrap.
static String formatDurationSince(time_t start) {
  time_t now = time(nullptr);
  uint32_t seconds = (now > start) ? (uint32_t)(now - start) : 0;
  return formatDuration(seconds);
}

// Epoch of the next wall-clock-aligned heartbeat: the next local minute that is a
// multiple of HEARTBEAT_MIN with seconds zeroed, pushed forward until it is at
// least HEARTBEAT_GUARD_S after `notBefore` (the previous update). Returns 0 if
// the clock is not set. Only meaningful when HEARTBEAT_ALIGNED.
static time_t nextAlignedHeartbeat(time_t notBefore) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 0;
  }
  timeinfo.tm_sec = 0;
  // Snap up to the next multiple-of-interval minute; mktime normalizes a value
  // >= 60 into the following hour.
  timeinfo.tm_min = (timeinfo.tm_min / HEARTBEAT_MIN + 1) * HEARTBEAT_MIN;
  time_t next = mktime(&timeinfo);
  while ((long)(next - notBefore) < HEARTBEAT_GUARD_S) {
    next += (time_t)HEARTBEAT_MIN * 60;
  }
  return next;
}

// Start SNTP and block until the clock is set or NTP_TIMEOUT_MS elapses.
static bool syncTime() {
  configTzTime(TZ_STRING, NTP_SERVER);
  struct tm timeinfo;
  uint32_t start = millis();
  while (!getLocalTime(&timeinfo, 1000)) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      return false;
    }
    Serial.println("Waiting for NTP time sync...");
    esp_task_wdt_reset();
  }
  return true;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

// Block until connected. The router may still be booting after power returns,
// so we keep retrying (CONCEPT.md §7).
static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    ledSet(true);  // solid on = connected
    return;
  }
  Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int ticks = 0;
  while (WiFi.status() != WL_CONNECTED) {
    ledToggle();            // blink while connecting
    delay(WIFI_BLINK_MS);
    esp_task_wdt_reset();
    // Re-issue begin() roughly every 20 s in case the router is still booting.
    if (++ticks % (20000 / WIFI_BLINK_MS) == 0) {
      Serial.println("Still no WiFi, re-issuing begin()...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  ledSet(true);  // solid on = connected
  Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Persistent state (NVS)
// A power loss wipes RAM, so the tracked message_id, the session's "online
// since", and the mute flag live in NVS. Writes happen only when a new message
// is created, the mute flag is toggled, or a boot records its diagnostics — all
// rare, so flash wear is negligible. This narrows ADR-0003 (see ADR-0008).
// ---------------------------------------------------------------------------

static Preferences g_prefs;

// Open NVS and load the persisted state into the globals.
static void stateBegin() {
  g_prefs.begin("imalive", false);
  g_messageId = g_prefs.getLong("msg_id", -1);
  g_onlineSinceEpoch = (time_t)g_prefs.getLong64("since", 0);
  g_muted = g_prefs.getBool("muted", true);  // muted by default: silent until /unmute
  g_otaState = g_prefs.getUChar("ota_state", OTA_IDLE);
  g_otaChat = (long long)g_prefs.getLong64("ota_chat", 0);
  g_prevBootEpoch = (time_t)g_prefs.getLong64("boot_ep", 0);
  g_wdtReboots = g_prefs.getULong("wdt_n", 0);
}

// Persist the tracked message and its session start (after creating a message).
static void saveSession() {
  g_prefs.putLong("msg_id", (int32_t)g_messageId);
  g_prefs.putLong64("since", (int64_t)g_onlineSinceEpoch);
}

static void saveMuted() {
  g_prefs.putBool("muted", g_muted);
}

static void saveOtaState() {
  g_prefs.putUChar("ota_state", g_otaState);
  g_prefs.putLong64("ota_chat", (int64_t)g_otaChat);
}

// Persist this boot's diagnostics: its epoch (read back by the next boot as the
// start of this session) and the watchdog counter. Call once the clock is set,
// so the stored epoch is real wall-clock time.
static void saveBootStats(esp_reset_reason_t reason) {
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT) {
    g_wdtReboots++;
    g_prefs.putULong("wdt_n", g_wdtReboots);
  }
  g_prefs.putLong64("boot_ep", (int64_t)g_bootEpoch);
}

// ---------------------------------------------------------------------------
// Telegram Bot API
// ---------------------------------------------------------------------------

static bool tgApiPost(const char* method, const String& body, String& response) {
  WiFiClientSecure client;
  // No server certificate validation. Simple and memory-light; acceptable for a
  // personal device. Pin Telegram's root CA here for a hardened setup.
  client.setInsecure();
  client.setHandshakeTimeout(TLS_HANDSHAKE_S);

  HTTPClient https;
  https.setTimeout(15000);  // response read only; the handshake has its own cap
  String url = String("https://api.telegram.org/bot") + TG_BOT_TOKEN + "/" + method;
  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin() failed");
    return false;
  }
  https.addHeader("Content-Type", "application/json");

  int code = https.POST(body);
  bool ok = false;
  if (code > 0) {
    response = https.getString();
    ok = (code == HTTP_CODE_OK);
    if (!ok) {
      Serial.printf("Telegram %s HTTP %d: %s\n", method, code, response.c_str());
    }
  } else {
    Serial.printf("Telegram %s failed: %s\n", method, https.errorToString(code).c_str());
  }
  https.end();
  // Every path above is time-bounded (handshake and read timeouts), so a request
  // that returns here means the loop is making progress. Feed the watchdog so a
  // burst of consecutive requests — a heartbeat edit falling back to a fresh
  // send, or the several sends in setup() — cannot add up past WDT_TIMEOUT_S.
  esp_task_wdt_reset();
  return ok;
}

// Post a prepared sendMessage document; returns the new message_id, or -1.
static long tgSendPrepared(JsonDocument& doc) {
  String body;
  serializeJson(doc, body);

  String response;
  if (!tgApiPost("sendMessage", body, response)) {
    return -1;
  }

  JsonDocument res;
  DeserializationError err = deserializeJson(res, response);
  if (err || !res["ok"].as<bool>()) {
    Serial.println("sendMessage: unexpected response");
    return -1;
  }
  return res["result"]["message_id"].as<long>();
}

// Send a new message to the configured recipient; returns its message_id, or -1.
// When silent, Telegram delivers it without a notification (disable_notification).
static long tgSendMessage(const String& text, bool silent) {
  JsonDocument doc;
  doc["chat_id"] = TG_CHAT_ID;
  doc["text"] = text;
  if (silent) {
    doc["disable_notification"] = true;
  }
  return tgSendPrepared(doc);
}

// Reply to a specific chat (used by command handling). Chat ids are 64-bit.
static void tgReply(long long chatId, const String& text, bool silent = false) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  if (silent) {
    doc["disable_notification"] = true;
  }
  tgSendPrepared(doc);
}

// Edit an existing message in place; returns true on success.
static bool tgEditMessage(long messageId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = TG_CHAT_ID;
  doc["message_id"] = messageId;
  doc["text"] = text;
  String body;
  serializeJson(doc, body);

  String response;
  if (tgApiPost("editMessageText", body, response)) {
    return true;
  }
  // Telegram rejects an edit whose text equals the current message content with
  // "message is not modified". That means the message already shows what we want,
  // so treat it as success rather than posting a fresh message.
  return response.indexOf("message is not modified") >= 0;
}

// Register the bot's command menu (setMyCommands) so the commands are discoverable
// in the Telegram UI. Idempotent; called once per boot.
static void tgSetCommands() {
  JsonDocument doc;
  JsonArray cmds = doc["commands"].to<JsonArray>();
  JsonObject s = cmds.add<JsonObject>();
  s["command"] = "status";
  s["description"] = "Show device status";
  JsonObject m = cmds.add<JsonObject>();
  m["command"] = "mute";
  m["description"] = "Mute startup notification";
  JsonObject u = cmds.add<JsonObject>();
  u["command"] = "unmute";
  u["description"] = "Re-arm startup notification";
  JsonObject o = cmds.add<JsonObject>();
  o["command"] = "update";
  o["description"] = "OTA update (attach .bin with caption /update)";
  String body;
  serializeJson(doc, body);
  String response;
  tgApiPost("setMyCommands", body, response);
}

// ---------------------------------------------------------------------------
// OTA update (ADR-0013)
// An admin sends the firmware .bin as a document captioned /update; it is
// fetched via getFile and streamed into the free OTA slot. The new firmware
// must confirm itself (first successful update of the tracked message) within
// OTA_MAX_BOOTS boots, or otaCheckOnBoot() restores the previous image.
// ---------------------------------------------------------------------------

static const uint8_t OTA_MAX_BOOTS = 3;

// Resolve a Telegram file_id to its download path on api.telegram.org.
static String tgGetFilePath(const char* fileId) {
  JsonDocument doc;
  doc["file_id"] = fileId;
  String body;
  serializeJson(doc, body);

  String response;
  if (!tgApiPost("getFile", body, response)) {
    return String();
  }
  JsonDocument res;
  if (deserializeJson(res, response) || !res["ok"].as<bool>()) {
    return String();
  }
  return res["result"]["file_path"].as<String>();
}

// Download the firmware image and stream it into the free OTA slot. Returns an
// empty string on success, or a short error description.
static String otaDownloadAndFlash(const String& filePath) {
  WiFiClientSecure client;
  client.setInsecure();  // same tradeoff as tgApiPost
  client.setHandshakeTimeout(TLS_HANDSHAKE_S);

  HTTPClient https;
  https.setTimeout(15000);
  String url = String("https://api.telegram.org/file/bot") + TG_BOT_TOKEN + "/" + filePath;
  if (!https.begin(client, url)) {
    return String("HTTPS begin() failed");
  }
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    https.end();
    return "download HTTP " + String(code);
  }
  int total = https.getSize();
  if (total <= 0) {
    https.end();
    return String("missing Content-Length");
  }
  if (!Update.begin(total)) {
    https.end();
    return "Update.begin: " + String(Update.errorString());
  }

  WiFiClient* stream = https.getStreamPtr();
  // Static, not stack: this function is inlined into the command poller, and a
  // 4 KB stack buffer overflows the 8 KB loopTask stack on every poll once the
  // TLS machinery is entered. Single-task use (loopTask only), so this is safe.
  static uint8_t buf[4096];
  int written = 0;
  uint32_t lastData = millis();
  while (written < total) {
    esp_task_wdt_reset();  // the download may outlast the watchdog period
    size_t avail = stream->available();
    if (avail == 0) {
      if (!client.connected() || millis() - lastData > 20000) {
        break;  // connection lost or stalled
      }
      delay(10);
      continue;
    }
    size_t n = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
    if (n == 0 || Update.write(buf, n) != n) {
      break;
    }
    written += n;
    lastData = millis();
  }
  https.end();

  if (written < total || !Update.end()) {
    String err = "flash failed at " + String(written) + "/" + String(total)
               + " (" + Update.errorString() + ")";
    Update.abort();
    return err;
  }
  return String();
}

// Handle an /update-captioned document from an admin: flash it into the free
// slot and reboot into the new image. On success this function does not return.
static void handleOtaDocument(long long chatId, const char* fileId, long fileSize) {
  Serial.printf("OTA: update request from %lld, %ld bytes\n", chatId, fileSize);
  tgReply(chatId, "Updating: downloading " + String(fileSize / 1024) + " KB...");

  String filePath = tgGetFilePath(fileId);
  if (filePath.length() == 0) {
    tgReply(chatId, "Update failed: getFile error.");
    return;
  }
  String err = otaDownloadAndFlash(filePath);
  if (err.length() > 0) {
    tgReply(chatId, "Update failed: " + err);
    Serial.printf("OTA: %s\n", err.c_str());
    return;
  }

  // Arm the confirmation watchdog: the new firmware must report in within
  // OTA_MAX_BOOTS boots or otaCheckOnBoot() restores this image.
  g_otaState = OTA_PENDING;
  g_otaChat = chatId;
  g_prefs.putUChar("ota_boots", 0);
  saveOtaState();

  tgReply(chatId, "Flashed OK, rebooting; will confirm shortly.");
  Serial.println("OTA: flashed, restarting");
  delay(500);
  ESP.restart();
}

// Count boots while an update awaits confirmation; after OTA_MAX_BOOTS failed
// attempts restore the previous image. Runs before networking so even a
// firmware that cannot get online is undone.
static void otaCheckOnBoot() {
  if (g_otaState != OTA_PENDING) {
    return;
  }
  uint8_t boots = g_prefs.getUChar("ota_boots", 0) + 1;
  g_prefs.putUChar("ota_boots", boots);
  Serial.printf("OTA: awaiting confirmation, boot %u/%u\n", boots, OTA_MAX_BOOTS);
  if (boots <= OTA_MAX_BOOTS) {
    return;
  }
  if (Update.canRollBack()) {
    Serial.println("OTA: confirmation failed, rolling back");
    g_otaState = OTA_ROLLED_BACK;
    saveOtaState();
    Update.rollBack();
    ESP.restart();
  } else {
    // No valid image to return to; stop counting and keep running.
    Serial.println("OTA: rollback not possible; keeping this image");
    g_otaState = OTA_IDLE;
    saveOtaState();
  }
}

// Once the tracked message has been updated successfully (Telegram reachable),
// resolve a pending update: confirm the new image, or report a rollback.
static void otaReportOnline() {
  if (g_otaState == OTA_IDLE) {
    return;
  }
  if (g_otaState == OTA_PENDING) {
    tgReply(g_otaChat, String("Update confirmed: running ") + FW_VERSION + " (" + BUILD_STAMP + ")");
    Serial.println("OTA: confirmed");
  } else {  // OTA_ROLLED_BACK
    tgReply(g_otaChat, String("Update FAILED and was rolled back; running ") + FW_VERSION + " (" + BUILD_STAMP + ")");
    Serial.println("OTA: rollback reported");
  }
  g_otaState = OTA_IDLE;
  g_prefs.putUChar("ota_boots", 0);
  saveOtaState();
}

// ---------------------------------------------------------------------------
// Message content
// ---------------------------------------------------------------------------

static String buildMessage(const String& lastSeen) {
  String msg;
  if (strlen(DEVICE_NAME) > 0) {
    msg += DEVICE_NAME;
    msg += "\n";
  }
  if (!g_muted) {
    // Only when armed: a new message means a genuine power-on, so g_onlineSince
    // is the real turn-on time. Hidden while muted, where the message is reused
    // indefinitely and that time would be stale.
    msg += "On since: ";
    msg += g_onlineSince;
    msg += "\n";
  }
  msg += "Last seen: ";
  msg += lastSeen;
  msg += " (updated every " + String(HEARTBEAT_MIN) + "m)";
  return msg;
}

// ---------------------------------------------------------------------------
// Inbound commands (/status)
// ---------------------------------------------------------------------------

static String buildStatusMessage() {
  String msg = "Status";
  msg += "\nName: ";
  msg += (strlen(DEVICE_NAME) > 0) ? DEVICE_NAME : "(none)";
  msg += "\nOnline since: " + g_onlineSince;
  msg += "\nNow: " + formatLocalTime();
  msg += "\nUptime: " + formatDurationSince(g_bootEpoch);
  msg += "\nHeartbeat: every " + String(HEARTBEAT_MIN) + "m";
  msg += "\nMute: " + String(g_muted ? "on" : "off");
  msg += "\nWiFi: " + String(WIFI_SSID) + " (" + String(WiFi.RSSI()) + " dBm)";
  msg += "\nIP: " + WiFi.localIP().toString();
  msg += "\nChat: " + String(TG_CHAT_ID);
  msg += "\nTZ: " + String(TZ_STRING);
  msg += "\nAdmins: ";
  for (size_t i = 0; i < g_adminCount; i++) {
    char idbuf[24];
    snprintf(idbuf, sizeof(idbuf), "%lld", g_adminIds[i]);
    if (i) {
      msg += ", ";
    }
    msg += idbuf;
  }
  msg += "\nVersion: ";
  msg += FW_VERSION;
  msg += " (";
  msg += BUILD_STAMP;
  msg += ")";
  msg += "\nSlot: ";
  msg += esp_ota_get_running_partition()->label;
  return msg;
}

// Advance the offset past any updates received before boot, so stale commands
// are not answered. Offset lives in RAM only (ADR-0003).
static void drainPendingUpdates() {
  JsonDocument req;
  req["timeout"] = 0;
  req["offset"] = -1;  // return only the most recent update
  String body;
  serializeJson(req, body);

  String response;
  if (!tgApiPost("getUpdates", body, response)) {
    return;
  }
  JsonDocument res;
  if (deserializeJson(res, response) || !res["ok"].as<bool>()) {
    return;
  }
  for (JsonObject upd : res["result"].as<JsonArray>()) {
    g_updateOffset = upd["update_id"].as<long>() + 1;
  }
}

// True if the given Telegram user id may issue commands (config TG_ADMIN_IDS).
static bool isAdmin(long long id) {
  for (size_t i = 0; i < g_adminCount; i++) {
    if (g_adminIds[i] == id) {
      return true;
    }
  }
  return false;
}

// Short-poll getUpdates and reply to /status.
static void pollCommands() {
  JsonDocument req;
  req["timeout"] = 0;  // return immediately with whatever is pending
  if (g_updateOffset != 0) {
    req["offset"] = g_updateOffset;
  }
  JsonArray allowed = req["allowed_updates"].to<JsonArray>();
  allowed.add("message");
  String body;
  serializeJson(req, body);

  String response;
  if (!tgApiPost("getUpdates", body, response)) {
    return;
  }
  JsonDocument res;
  if (deserializeJson(res, response) || !res["ok"].as<bool>()) {
    return;
  }

  for (JsonObject upd : res["result"].as<JsonArray>()) {
    g_updateOffset = upd["update_id"].as<long>() + 1;  // acknowledge

    long long fromId = upd["message"]["from"]["id"].as<long long>();
    if (!isAdmin(fromId)) {
      Serial.printf("Ignoring command from non-admin %lld\n", fromId);
      continue;
    }

    long long chatId = upd["message"]["chat"]["id"].as<long long>();

    // A document captioned /update carries a firmware image (ADR-0013).
    const char* otaFileId = upd["message"]["document"]["file_id"];
    const char* caption = upd["message"]["caption"] | "";
    if (otaFileId != nullptr && strncmp(caption, "/update", 7) == 0) {
      handleOtaDocument(chatId, otaFileId,
                        upd["message"]["document"]["file_size"].as<long>());
      continue;
    }

    const char* text = upd["message"]["text"] | "";
    if (strncmp(text, "/status", 7) == 0) {
      tgReply(chatId, buildStatusMessage());
      Serial.printf("Replied to /status from chat %lld\n", chatId);
    } else if (strncmp(text, "/mute", 5) == 0) {
      g_muted = true;
      saveMuted();
      tgReply(chatId, "Startup ping muted — safe to power off now. Send /unmute to re-arm.");
      Serial.println("Muted startup ping");
    } else if (strncmp(text, "/unmute", 7) == 0) {
      g_muted = false;
      saveMuted();
      tgReply(chatId, "Startup ping armed. The next real power-on will notify.");
      Serial.println("Armed startup ping");
    } else if (strncmp(text, "/update", 7) == 0) {
      tgReply(chatId, "Attach the firmware .bin as a document with caption /update.");
    }
  }
}

// ---------------------------------------------------------------------------
// Admin boot notice
// Every boot sends a short silent note to each admin's private chat (never the
// main chat): reset reason and firmware version. This makes soft reboots
// (watchdog / panic / OTA) visible without disturbing the main chat, which
// deliberately hides them (ADR-0008).
// ---------------------------------------------------------------------------

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT:      return "external reset";
    default:               return "unknown";
  }
}

static void notifyAdminsBoot(esp_reset_reason_t reason) {
  String msg;
  if (strlen(DEVICE_NAME) > 0) {
    msg += DEVICE_NAME;
    msg += ": ";
  }
  msg += "booted (";
  msg += resetReasonName(reason);
  msg += ")\nVersion: ";
  msg += FW_VERSION;
  msg += " (";
  msg += BUILD_STAMP;
  msg += ")";
  // How long the previous session lasted: the span between the two boots, so it
  // includes the few seconds a reboot takes. Absent on the first boot after
  // flashing, when nothing is stored yet.
  if (g_prevBootEpoch > 0 && g_bootEpoch > g_prevBootEpoch) {
    msg += "\nPrevious session: ";
    msg += formatDuration((uint32_t)(g_bootEpoch - g_prevBootEpoch));
  }
  if (g_wdtReboots > 0) {
    msg += "\nWatchdog reboots: ";
    msg += String(g_wdtReboots);
  }
  // An admin who has never messaged the bot cannot be reached (Telegram forbids
  // bot-initiated chats); the send fails and is just logged by tgApiPost.
  for (size_t i = 0; i < g_adminCount; i++) {
    tgReply(g_adminIds[i], msg, true);  // silent: informational, no buzz
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[imalive] boot");

  pinMode(LED_PIN, OUTPUT);
  ledSet(false);

  // Reboot if the main loop hangs (always-on reliability, CONCEPT.md §7).
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  stateBegin();     // load persisted message_id / online-since / mute / OTA state
  otaCheckOnBoot(); // roll back a pending update that keeps failing to boot

  connectWiFi();

  // Never announce with a wrong clock: if NTP does not sync, restart and retry.
  if (!syncTime()) {
    Serial.println("NTP sync failed; restarting...");
    delay(1000);
    ESP.restart();
  }

  g_bootEpoch = time(nullptr);  // uptime is measured from this boot

  // A genuine power-on (mains returned) with notifications armed is the only
  // event that announces with a sound. A soft reboot (watchdog / OTA) or a
  // planned muted restart instead silently resumes the existing message and
  // keeps the persistent "online since". See ADR-0008.
  esp_reset_reason_t reason = esp_reset_reason();
  saveBootStats(reason);
  bool announce = (reason == ESP_RST_POWERON) && !g_muted;

  if (announce || g_onlineSinceEpoch <= 0) {
    g_onlineSinceEpoch = time(nullptr);  // fresh session (or first boot ever)
  }
  g_onlineSince = formatLocalTimeAt(g_onlineSinceEpoch);
  Serial.printf("Boot: reason %d, muted %d, online since %s\n",
                (int)reason, (int)g_muted, g_onlineSince.c_str());

  String text = buildMessage(formatLocalTime());

  // Continuation first: try to resume (edit) the existing message silently.
  bool resumed = !announce && g_messageId > 0 && tgEditMessage(g_messageId, text);
  bool messageOk = resumed;

  if (!resumed) {
    // A new message is needed. It carries a notification only on an armed
    // power-on; every other case (soft reboot, muted, or a missing message) is
    // sent silently, exactly like a heartbeat edit.
    bool silent = !announce;
    long id = -1;
    for (int i = 0; i < 3 && id <= 0; i++) {
      id = tgSendMessage(text, silent);
      if (id <= 0) {
        delay(2000);
        esp_task_wdt_reset();
      }
    }
    if (id > 0) {
      g_messageId = id;
      saveSession();
      messageOk = true;
      Serial.printf("Sent %s message, id %ld\n", silent ? "silent" : "notifying", id);
    } else {
      Serial.println("Initial send failed; heartbeat will retry.");
    }
  } else {
    Serial.printf("Resumed message id %ld\n", g_messageId);
  }

  if (messageOk) {
    otaReportOnline();  // resolve a pending update now that Telegram works
  }

  notifyAdminsBoot(reason);  // startup notice to admins, never the main chat

  tgSetCommands();        // publish the command menu (/status, /mute, /unmute)
  drainPendingUpdates();  // ignore commands received before this boot

  g_lastHeartbeatMs = millis();
  if (HEARTBEAT_ALIGNED) {
    g_nextHeartbeatEpoch = nextAlignedHeartbeat(time(nullptr));
  }
  g_lastPollMs = millis();
}

void loop() {
  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost; reconnecting...");
    connectWiFi();
  }

  if (millis() - g_lastPollMs >= CMD_POLL_MS) {
    g_lastPollMs = millis();
    pollCommands();
  }

  bool heartbeatDue = HEARTBEAT_ALIGNED
      ? (g_nextHeartbeatEpoch > 0 && time(nullptr) >= g_nextHeartbeatEpoch)
      : (millis() - g_lastHeartbeatMs >= HEARTBEAT_MS);
  if (heartbeatDue) {
    g_lastHeartbeatMs = millis();

    String lastSeen = formatLocalTime();
    String text = buildMessage(lastSeen);

    bool ok = (g_messageId > 0) && tgEditMessage(g_messageId, text);
    if (!ok) {
      // Fallback (ADR-0004): message too old to edit, or never sent — send a
      // fresh one silently (no restart happened) and keep editing that one.
      Serial.println("Edit failed; sending a fresh (silent) message.");
      long newId = tgSendMessage(text, true);
      if (newId > 0) {
        g_messageId = newId;
        saveSession();
        ok = true;
      }
    }
    if (ok) {
      otaReportOnline();  // resolve a pending update now that Telegram works
    }
    if (HEARTBEAT_ALIGNED) {
      g_nextHeartbeatEpoch = nextAlignedHeartbeat(time(nullptr));
    }
    Serial.printf("Heartbeat: last seen %s (message id %ld)\n", lastSeen.c_str(), g_messageId);
  }

  delay(1000);
}
