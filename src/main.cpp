// imalive — an "I'm alive" heartbeat device on ESP32-C3 SuperMini.
//
// On power-on the device connects to WiFi, syncs time over NTP, and sends one
// "I'm alive" message to a Telegram chat. While it stays powered it edits that
// same message every HEARTBEAT_MIN minutes (a silent heartbeat — no new
// notification). When power is lost the device dies and the message freezes at
// its last "Last seen" value. See CONCEPT.md and docs/adr/ for the rationale.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_task_wdt.h"

#include "config.h"

// Fixed tunables (not exposed in config.h).
static const char* NTP_SERVER = "pool.ntp.org";
static const uint32_t WIFI_BLINK_MS = 250;      // status-LED blink period while connecting
static const uint32_t NTP_TIMEOUT_MS = 30000;
static const uint32_t WDT_TIMEOUT_S = 60;
static const uint32_t HEARTBEAT_MS = (uint32_t)HEARTBEAT_MIN * 60UL * 1000UL;
static const uint32_t CMD_POLL_MS = 4000;       // /status command poll interval

static const char* FW_VERSION  = "0.1.0";
static const char* BUILD_STAMP = __DATE__ " " __TIME__;  // set at compile time

// Runtime state — RAM only, never persisted (ADR-0003: stateless, no NVS).
static long g_messageId = -1;   // Telegram message id for the current power session
static String g_onlineSince;    // local time this power session started
static time_t g_onlineSinceEpoch = 0;  // wall-clock epoch when this session started
static time_t g_bootEpoch = 0;  // wall-clock epoch of boot, for overflow-free uptime
static uint32_t g_lastHeartbeatMs = 0;
static long g_updateOffset = 0; // getUpdates acknowledge cursor (RAM only)
static uint32_t g_lastPollMs = 0;

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
    return String("--.-- --:--");
  }
  char buf[16];
  strftime(buf, sizeof(buf), "%d.%m %H:%M", &timeinfo);
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
// Telegram Bot API
// ---------------------------------------------------------------------------

static bool tgApiPost(const char* method, const String& body, String& response) {
  WiFiClientSecure client;
  // No server certificate validation. Simple and memory-light; acceptable for a
  // personal device. Pin Telegram's root CA here for a hardened setup.
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(15000);
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
static long tgSendMessage(const String& text) {
  JsonDocument doc;
  doc["chat_id"] = TG_CHAT_ID;
  doc["text"] = text;
  return tgSendPrepared(doc);
}

// Reply to a specific chat (used by command handling). Chat ids are 64-bit.
static void tgReply(long long chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
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
  return tgApiPost("editMessageText", body, response);
}

// ---------------------------------------------------------------------------
// Message content
// ---------------------------------------------------------------------------

static String buildMessage(const String& lastSeen) {
  String msg = "I'm alive";
  if (strlen(DEVICE_NAME) > 0) {
    msg += " — ";  // em dash
    msg += DEVICE_NAME;
  }
  msg += "\nOnline since: ";
  msg += g_onlineSince;
  msg += " (" + formatDurationSince(g_onlineSinceEpoch) + " ago)";
  msg += "\nLast seen: ";
  msg += lastSeen;
  return msg;
}

// ---------------------------------------------------------------------------
// Inbound commands (/status)
// ---------------------------------------------------------------------------

static String buildStatusMessage() {
  String msg = "Status";
  if (strlen(DEVICE_NAME) > 0) {
    msg += " — ";
    msg += DEVICE_NAME;
  }
  msg += "\nOnline since: " + g_onlineSince;
  msg += "\nNow: " + formatLocalTime();
  msg += "\nUptime: " + formatDurationSince(g_bootEpoch);
  msg += "\nWiFi: " + String(WiFi.RSSI()) + " dBm";
  msg += "\nIP: " + WiFi.localIP().toString();
  msg += "\nVersion: ";
  msg += FW_VERSION;
  msg += " (";
  msg += BUILD_STAMP;
  msg += ")";
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

    const char* text = upd["message"]["text"] | "";
    if (strncmp(text, "/status", 7) == 0) {
      long long chatId = upd["message"]["chat"]["id"].as<long long>();
      tgReply(chatId, buildStatusMessage());
      Serial.printf("Replied to /status from chat %lld\n", chatId);
    }
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

  connectWiFi();

  // Never announce with a wrong clock: if NTP does not sync, restart and retry.
  if (!syncTime()) {
    Serial.println("NTP sync failed; restarting...");
    delay(1000);
    ESP.restart();
  }

  g_onlineSince = formatLocalTime();
  g_onlineSinceEpoch = time(nullptr);
  g_bootEpoch = g_onlineSinceEpoch - millis() / 1000;
  Serial.printf("Time synced. Online since %s\n", g_onlineSince.c_str());

  // One new message per power-on (ADR-0004). Retry a few times on a transient
  // failure; otherwise the heartbeat below keeps trying.
  String text = buildMessage(g_onlineSince);
  for (int i = 0; i < 3 && g_messageId <= 0; i++) {
    g_messageId = tgSendMessage(text);
    if (g_messageId <= 0) {
      delay(2000);
      esp_task_wdt_reset();
    }
  }
  if (g_messageId > 0) {
    Serial.printf("Sent 'I'm alive', message id %ld\n", g_messageId);
  } else {
    Serial.println("Initial send failed; heartbeat will retry.");
  }

  drainPendingUpdates();  // ignore commands received before this boot

  g_lastHeartbeatMs = millis();
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

  if (millis() - g_lastHeartbeatMs >= HEARTBEAT_MS) {
    g_lastHeartbeatMs = millis();

    String lastSeen = formatLocalTime();
    String text = buildMessage(lastSeen);

    bool ok = (g_messageId > 0) && tgEditMessage(g_messageId, text);
    if (!ok) {
      // Fallback (ADR-0004): message too old to edit, or never sent — send a
      // fresh one and keep editing that from now on.
      Serial.println("Edit failed; sending a fresh message.");
      long newId = tgSendMessage(text);
      if (newId > 0) {
        g_messageId = newId;
      }
    }
    Serial.printf("Heartbeat: last seen %s (message id %ld)\n", lastSeen.c_str(), g_messageId);
  }

  delay(1000);
}
