// imalive — an "I'm alive" heartbeat device on ESP32-C3 SuperMini.
//
// On a genuine power-on the device connects to WiFi, syncs time over NTP, and
// sends an "I'm alive" message to a Telegram chat with a notification. While it
// stays powered it edits that same message every HEARTBEAT_MIN minutes (a silent
// heartbeat). The message_id is kept in NVS, so a soft reboot (watchdog / OTA)
// or a planned muted restart silently resumes the same message instead of
// pinging again. When power is lost the device dies and the message freezes at
// its last "Last seen" value. See CONCEPT.md and docs/adr/ for the rationale.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "esp_task_wdt.h"
#include "esp_system.h"

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

// Runtime state. The tracked message_id, the session's "online since", and the
// startup-ping mute flag survive reboots in NVS (see the Persistent state
// section); everything else is RAM only.
static long g_messageId = -1;   // Telegram message id of the tracked message (NVS)
static String g_onlineSince;    // local-time string of the session start
static time_t g_onlineSinceEpoch = 0;  // session start epoch, persistent across reboots (NVS)
static time_t g_bootEpoch = 0;  // epoch of this boot, for uptime-since-boot
static bool g_muted = true;     // if set, a power-on resumes silently — no ping (NVS); muted by default
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

// Format an arbitrary epoch as local "dd.mm HH:MM" (for a stored session start).
static String formatLocalTimeAt(time_t t) {
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
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
// Persistent state (NVS)
// A power loss wipes RAM, so the tracked message_id, the session's "online
// since", and the mute flag live in NVS. Writes happen only when a new message
// is created or the mute flag is toggled — rare, so flash wear is negligible.
// This narrows ADR-0003 (see ADR-0008).
// ---------------------------------------------------------------------------

static Preferences g_prefs;

// Open NVS and load the persisted state into the globals.
static void stateBegin() {
  g_prefs.begin("imalive", false);
  g_messageId = g_prefs.getLong("msg_id", -1);
  g_onlineSinceEpoch = (time_t)g_prefs.getLong64("since", 0);
  g_muted = g_prefs.getBool("muted", true);  // muted by default: silent until /unmute
}

// Persist the tracked message and its session start (after creating a message).
static void saveSession() {
  g_prefs.putLong("msg_id", (int32_t)g_messageId);
  g_prefs.putLong64("since", (int64_t)g_onlineSinceEpoch);
}

static void saveMuted() {
  g_prefs.putBool("muted", g_muted);
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
  if (tgApiPost("editMessageText", body, response)) {
    return true;
  }
  // Telegram rejects an edit whose text equals the current message content with
  // "message is not modified". That means the message already shows what we want,
  // so treat it as success rather than posting a fresh message.
  return response.indexOf("message is not modified") >= 0;
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
  if (strlen(DEVICE_NAME) > 0) {
    msg += " — ";
    msg += DEVICE_NAME;
  }
  msg += "\nOnline since: " + g_onlineSince;
  msg += "\nNow: " + formatLocalTime();
  msg += "\nUptime: " + formatDurationSince(g_bootEpoch);
  msg += "\nHeartbeat: every " + String(HEARTBEAT_MIN) + "m";
  msg += "\nMute: " + String(g_muted ? "on" : "off");
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
    long long chatId = upd["message"]["chat"]["id"].as<long long>();
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

  stateBegin();  // load persisted message_id / online-since / mute flag

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
      Serial.printf("Sent %s message, id %ld\n", silent ? "silent" : "notifying", id);
    } else {
      Serial.println("Initial send failed; heartbeat will retry.");
    }
  } else {
    Serial.printf("Resumed message id %ld\n", g_messageId);
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
      // fresh one silently (no restart happened) and keep editing that one.
      Serial.println("Edit failed; sending a fresh (silent) message.");
      long newId = tgSendMessage(text, true);
      if (newId > 0) {
        g_messageId = newId;
        saveSession();
      }
    }
    Serial.printf("Heartbeat: last seen %s (message id %ld)\n", lastSeen.c_str(), g_messageId);
  }

  delay(1000);
}
