#pragma once

// Copy this file to config.h and fill in your values.
// config.h is gitignored and must never be committed (see .gitignore / ADR-0003).

// --- WiFi ---
#define WIFI_SSID      "your-wifi-ssid"
#define WIFI_PASSWORD  "your-wifi-password"

// --- Telegram ---
// Bot token from @BotFather.
#define TG_BOT_TOKEN   "123456789:ABCdefGhIJKlmNoPQRstuVWxyz"
// Destination chat: a personal chat (positive numeric id) or a channel
// ("-100..." or "@channel"). For a channel, add the bot as an administrator
// with post permission.
#define TG_CHAT_ID     "123456789"
// User IDs allowed to send commands (/status, /mute, /unmute). Commands from any
// other sender are ignored. At least one id is required. Find yours via
// @userinfobot. Braces required, e.g. { 111, 222 }.
#define TG_ADMIN_IDS   { 123456789 }

// --- Behaviour ---
#define HEARTBEAT_MIN  30      // heartbeat interval, minutes
#define DEVICE_NAME    ""      // optional label in the message, e.g. "Cottage" (avoid < > &)

// --- Timezone (POSIX TZ string, DST-aware; see CONCEPT.md "Time source") ---
// Asia/Bangkok, UTC+7, no DST.
#define TZ_STRING      "ICT-7"
