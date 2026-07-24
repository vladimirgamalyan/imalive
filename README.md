# imalive

An **"I'm alive" heartbeat device** on an ESP32-C3 SuperMini. It helps you tell
whether **mains power is present** at a remote location (home, cottage, office) —
without any voltage sensor.

The trick is inversion: the device is powered by the very mains supply it watches,
so *the device being online is the signal*. It never claims "power is back" (it
cannot tell power from internet or a hang) — it only ever reports that **it is
alive**, and you infer the rest.

## How it works

1. On power-on it connects to WiFi, syncs the clock over NTP, and sends **one**
   Telegram message: `I'm alive — <name>` with the time it came online.
2. While powered, every `HEARTBEAT_MIN` minutes it **edits that same message** to
   refresh a `Last seen` line. Editing raises **no** notification, so the chat is
   not spammed.
3. When power drops, the device dies and the message **freezes** at its last
   `Last seen` — telling you until when power (and the device) was definitely there.

Each power-on posts a new message; the device keeps no state across reboots, so the
chat naturally becomes a log of power cycles. See [`CONCEPT.md`](CONCEPT.md) and the
[decision records](docs/adr/) for the full rationale.

### Message example

```
I'm alive — The Fine Place Resort
🕒 Online since: 24.07 14:32
Last seen: 24.07 16:02
```

## Hardware

- **ESP32-C3 SuperMini** (2.4 GHz WiFi; the C3 has no 5 GHz).
- Powered from USB / a 5 V adapter plugged into the socket you want to monitor.
- Onboard blue LED (GPIO8) is used as a WiFi status indicator:
  - **blinking** — connecting to WiFi;
  - **solid** — connected.

## Getting started

### 1. Install PlatformIO

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/)
(`pio`) or the PlatformIO IDE extension for VS Code.

### 2. Configure

Copy the template and fill in your values:

```sh
cp include/config.example.h include/config.h
```

`include/config.h` is git-ignored and must never be committed.

| Field | Meaning |
| --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | Your 2.4 GHz WiFi credentials |
| `TG_BOT_TOKEN` | Bot token from [@BotFather](https://t.me/BotFather) |
| `TG_CHAT_ID` | **Recipient** chat id — a personal chat or a channel |
| `HEARTBEAT_MIN` | Heartbeat interval in minutes (default `30`) |
| `DEVICE_NAME` | Optional label shown in the message, e.g. `"Cottage"` |
| `TZ_STRING` | POSIX timezone, DST-aware (default `"ICT-7"`, Asia/Bangkok) |

### 3. Telegram setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy its token.
2. **Send your bot a message first** (e.g. `/start`). A bot cannot start a
   conversation, so without this it can never message you.
3. Find your chat id: open
   `https://api.telegram.org/bot<TOKEN>/getUpdates` and read
   `result[].message.chat.id`.
   - ⚠️ This is the **recipient's** id, *not* the bot's own id (the number before
     `:` in the token). Sending to the bot's own id fails.
   - For a **channel**: add the bot as an **administrator** with post permission
     and use a `chat_id` like `-100...` (or `@channelusername`).

Multiple devices can share one chat/channel — each edits only its own message.
Give them distinct `DEVICE_NAME`s to tell them apart.

### 4. Build and flash

```sh
pio run                 # build
pio run -t upload       # build + flash over USB
pio device monitor      # optional serial console (115200 baud)
```

Watch the blue LED: once it stays solid, WiFi is up and the first message should
arrive in your chat within a few seconds.

## Timezone

`TZ_STRING` is a POSIX TZ string, so daylight-saving transitions are handled
automatically. Examples:

- `ICT-7` — Asia/Bangkok (UTC+7, no DST) — the shipped default.
- `EET-2EEST,M3.5.0/3,M10.5.0/4` — Eastern European (UTC+2 / +3).
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European (UTC+1 / +2).
- `MSK-3` — Moscow (UTC+3, no DST).

## Limitations

By design the device cannot distinguish **no power** from **no internet** or a
**hang** — all three simply stop the heartbeat. It also does not send an
in-the-moment "power lost" message (no supercapacitor), and a spontaneous reboot
looks like a real power cycle. These are deliberate tradeoffs; see
[`CONCEPT.md` §7](CONCEPT.md) and the [ADRs](docs/adr/).

## Repository layout

```
platformio.ini            PlatformIO project (ESP32-C3 SuperMini, Arduino)
include/config.example.h  config template (copy to config.h)
src/main.cpp              firmware
CONCEPT.md                design concept and tradeoffs
docs/adr/                 architecture decision records
```
