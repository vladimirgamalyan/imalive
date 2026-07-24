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
static const uint32_t WIFI_RETRY_DELAY_MS = 1000;
static const uint32_t NTP_TIMEOUT_MS = 30000;
static const uint32_t WDT_TIMEOUT_S = 60;
static const uint32_t HEARTBEAT_MS = (uint32_t)HEARTBEAT_MIN * 60UL * 1000UL;

// Runtime state — RAM only, never persisted (ADR-0003: stateless, no NVS).
static long g_messageId = -1;   // Telegram message id for the current power session
static String g_onlineSince;    // local time this power session started
static uint32_t g_lastHeartbeatMs = 0;

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
    return;
  }
  Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
    esp_task_wdt_reset();
    if (++tries % 20 == 0) {
      Serial.println("\nStill no WiFi, re-issuing begin()...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
  Serial.printf("\nWiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
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

// Send a new message; returns its message_id, or -1 on failure.
static long tgSendMessage(const String& text) {
  JsonDocument doc;
  doc["chat_id"] = TG_CHAT_ID;
  doc["text"] = text;
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
  msg += "\n\U0001F552 Online since: ";  // clock emoji
  msg += g_onlineSince;
  msg += "\nLast seen: ";
  msg += lastSeen;
  return msg;
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[imalive] boot");

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

  g_lastHeartbeatMs = millis();
}

void loop() {
  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost; reconnecting...");
    connectWiFi();
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
