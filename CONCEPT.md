# imalive — an "I'm alive" heartbeat device on ESP32-C3

Status: **implemented**. This is the origin concept; where the firmware refined it,
the `docs/adr/` records win — notably ADR-0008 changed the persistence and
notification model described below.

Purpose: help an operator tell whether **mains power is present** at a remote
location — but the device only ever asserts its own **liveness** ("I'm alive").
Inferring "power is present" from that liveness is left to the human. This keeps
the device honest: it never claims something it cannot actually know.

## 1. Idea

An ESP32-C3 device stays permanently plugged into a wall socket at the monitored
location (home / cottage / office). It keeps a connection to Telegram and, as long
as it is running, periodically reports **"I'm alive"** to a chat.

**Core idea (inversion):** the device is powered by the same mains supply it helps
monitor, so its own liveness carries the signal. If the device is alive and online,
the operator can infer that power is present. We do not need a separate voltage
sensor — the device's own power supply *is* what keeps it alive.

The device deliberately reports **only liveness**, never "power is back". It cannot
distinguish "no power" from "no internet" or "device hung" (see §7), so claiming
anything about power directly would be misleading. The project name — *imalive* /
"I'm alive" — matches exactly what the device asserts.

## 2. What the observer sees

- **Device comes online** → it reports **"I'm alive"** with a `Last seen` line. A
  genuine mains power-on posts a **new** message and is the only sound-notification
  — and only when notifications are *armed* (the device ships **muted**, ADR-0008).
  A spontaneous reboot (watchdog / OTA) or a muted restart **resumes the existing
  message** silently.
- **While the device is alive** → every N minutes it **edits that same message**
  (via `editMessageText`), updating the "last seen" line. Editing a Telegram
  message does **not** raise a new notification — so the chat is not spammed.
- **Device stops** (power lost, internet lost, or a hang) → it goes silent and the
  message "freezes" at its last "last seen" value.

So the operator can always look at the last message and reason:
"the device was definitely alive within the last N minutes" (fresh "last seen") or
"the device went silent around HH:MM" (stale "last seen") — and from that infer
whether power is present.

### Message example

Right after coming online (the `On since` line appears only when notifications are
armed — i.e. un-muted; a muted device shows just `Last seen`):

```
I'm alive
On since: 24.07 14:32
Last seen: 24.07 14:32 (updated every 30m)
```

After a few heartbeats (same message, edited in place):

```
I'm alive
On since: 24.07 14:32
Last seen: 24.07 16:02 (updated every 30m)
```

If the device goes silent after 16:02, the message stays at "Last seen: 16:02" —
the operator reads that as "the device (and therefore power) was present at least
until ~16:02".

> Note: the runtime wording and language of the Telegram messages is a product
> choice, not fixed here. Russian is the natural default since the operator reads
> them; the examples above are shown in English only to keep this doc consistent.

## 3. Lifecycle (state machine)

```
[POWER ON]
   │
   ▼
Connect to WiFi ──(failed)──► retry with backoff (router may still be booting)
   │ (success)
   ▼
Time sync over NTP
   │
   ▼
Load message_id / online_since / mute from NVS
   │
   ▼
if (reset reason == POWER-ON) and (not muted):
      sendMessage("I'm alive …", notify)    # new session; online_since = now
else:                                        # watchdog / OTA / muted power-off
      editMessageText(message_id, …)         # resume the existing message, silent
   │
   ▼
┌─► wait N minutes
│      │
│      ▼
│  editMessageText(message_id, "… Last seen: HH:MM")
│      │  (edit error — §7 fallback: a fresh silent message)
└──────┘
   ⋮
[DEVICE STOPS] → device goes silent, sends nothing
```

The boot is classified via `esp_reset_reason()` (ADR-0008): only a genuine
`ESP_RST_POWERON` while un-muted starts a fresh, **notifying** session and resets
"online since". Every other cause — watchdog, OTA, software restart — or a muted
power-off **resumes** the persisted message in place, keeping "online since". The
`message_id`, "online since" and mute flag survive in NVS; this is what fixes the
spontaneous-reboot false positive once noted in §7.

## 4. Inferring power loss (from liveness)

The device never sends a "power lost" message — when power drops at that location
the router is usually unpowered too, so the device would not physically have time
to send anything (a supercapacitor / power reserve was deliberately **not** added).
Equally, the device never asserts "power is present"; it only asserts liveness.

Power loss is **inferred by the human** from the "Last seen" line no longer
updating. The **resolution** of that inference equals the heartbeat interval (N
minutes): with N = 30 min the device went silent somewhere in the window
[last "last seen"; last "last seen" + 30 min].

The last heartbeat time is **not** persisted, so the device does not compute a
silent-duration on the next start — only `message_id`, "online since" and the mute
flag are kept (ADR-0008). On a real, un-muted power-on it posts a fresh message and
the previous one freezes, so the human still estimates the gap from that frozen
message in the chat history.

## 5. Timings

- **Online (liveness) detection latency**: practically instant — ESP boot time +
  WiFi connect + NTP ≈ 10–30 s.
- **Heartbeat interval (N)**: default **30 minutes** (candidates: 30 / 60),
  exposed in config. Tradeoff: smaller N → fresher "last seen" and a tighter
  silence estimate, but more Telegram API calls.

## 6. Technical stack and hardware

- **Board**: **ESP32-C3 SuperMini**, 2.4 GHz WiFi. Has an onboard blue LED
  (GPIO8, active LOW) that can be used for status indication.
- **Power**: from USB / a 5V adapter plugged into the monitored socket.
- **Firmware**: C++ on the **Arduino framework** via **PlatformIO**.
- **Telegram link**: HTTPS to `api.telegram.org` (TLS required —
  `WiFiClientSecure`; either a root certificate or `setInsecure()`).
  Two Bot API methods are enough: `sendMessage` and `editMessageText`.
  A full bot library (e.g. UniversalTelegramBot) is optional — for a one-way
  scenario a couple of HTTPS requests plus parsing `message_id` suffice.
- **Time**: NTP (`pool.ntp.org`), DST-aware via a POSIX TZ string from config
  (see "Time source" below).

### Expected project layout (PlatformIO)

```
imalive/
├── platformio.ini
├── include/
│   ├── config.h            # secrets — not committed (.gitignore)
│   └── config.example.h    # template without secrets — committed
├── src/
│   └── main.cpp
└── CONCEPT.md
```

### config.h (draft fields)

```cpp
#define WIFI_SSID        "..."
#define WIFI_PASSWORD    "..."
#define TG_BOT_TOKEN     "123456:ABC..."
#define TG_CHAT_ID       "-1001234567890"   // personal chat (positive id) OR channel (-100…)
#define HEARTBEAT_MIN    30                  // heartbeat interval, minutes
#define TZ_STRING        "ICT-7"                          // Asia/Bangkok (UTC+7, no DST); POSIX TZ — see "Time source"
#define DEVICE_NAME      "Cottage"           // caption in the message (optional)
```

Recipient (`TG_CHAT_ID`) — both variants are supported:

- **Personal chat**: `chat_id` = the user's numeric id (obtained once, e.g. by
  messaging the bot and reading `getUpdates`). The user must start the dialog with
  the bot first.
- **Channel**: `chat_id` of the form `-100…` (or `@username` for a public
  channel). The bot must be **added to the channel as an administrator** with post
  permission.

Timezone and heartbeat interval are fully controlled from `config.h`
(`TZ_STRING`, `HEARTBEAT_MIN`); changing them means editing the config and
rebuilding (no over-the-air provisioning in the current concept).

### Time source

The SuperMini has no battery-backed RTC, so the device knows nothing about the
wall-clock time at boot. Time comes from the network on every start:

1. Connect WiFi.
2. Sync time over SNTP from `pool.ntp.org` (UTC), applying the POSIX `TZ_STRING`
   so local time and DST transitions are handled automatically
   (`configTzTime(TZ_STRING, "pool.ntp.org")` in Arduino).
3. Only **after** a successful sync send the first "I'm alive" — never emit a
   message with unsynced time. If SNTP has not answered yet, retry with backoff.

During a session the internal timer keeps time between syncs (small drift); SNTP
re-syncs periodically to correct it. After a power loss the device reboots and
simply re-syncs — there is no RTC battery, so the wall clock always comes from the
network. (Message state *is* kept in NVS — ADR-0008 — but the clock is not.)

`TZ_STRING` is a POSIX TZ specification, e.g.:

- `ICT-7` — Asia/Bangkok (UTC+7, no DST) — the value shipped in this project.
- `EET-2EEST,M3.5.0/3,M10.5.0/4` — Eastern European (UTC+2, DST → UTC+3).
- `CET-1CEST,M3.5.0,M10.5.0/3` — Central European (UTC+1, DST → UTC+2).
- `MSK-3` — Moscow, fixed UTC+3, no DST.

Dependency note: NTP needs the network, but the device also needs the network to
reach Telegram — so there is no case where it can report but cannot get the time.

### Multiple devices and channels

Several imalive devices can safely share one chat or channel. A device edits only
the message it sent, addressed by the `message_id` returned from its own
`sendMessage` — it never scans the chat or looks at other messages. So multiple
imalive devices (and any unrelated bots posting to the same channel) never contend
for the same message: `message_id` is unique per chat and each device holds its
own. There is nothing to "figure out" at heartbeat time.

Practical notes:

- Give each device a distinct `DEVICE_NAME`. The firmware never relies on it — it
  is purely for the human reader — but it turns a channel into a readable board:
  `I'm alive — Cottage`, `I'm alive — Office`, ...
- A device resumes its own message across reboots (`message_id` in NVS, ADR-0008)
  and posts a new one only on a real, un-muted power cycle; earlier messages freeze
  in place. In a channel this accumulates one frozen message per power cycle per
  device — the intended history.
- If a device's message is deleted or is past its edit window, the next edit fails
  and the fallback (ADR-0004) posts a fresh **silent** message to continue from.

## 7. Edge cases and limitations (deliberate tradeoffs)

1. **Device liveness ≠ power.** The device asserts only that it is alive. That can
   mean "power present AND internet up AND firmware healthy" all at once — it
   **cannot distinguish** "no power", "no internet/router" and "device hung". All
   three read as "the heartbeat stopped updating". Reporting only liveness (rather
   than "power is back") is the honest framing of exactly this limitation.

2. **Spontaneous reboot (crash / watchdog), not a real outage.** *Resolved
   (ADR-0008).* The boot is classified via `esp_reset_reason()`: a watchdog / OTA /
   software restart is **not** `ESP_RST_POWERON`, so the device resumes its message
   (`message_id` from NVS) silently — no false "power is back". Only a genuine
   power-on notifies.

3. **Router boots slower than the ESP.** After power returns the router may take
   1–2 minutes to come up. WiFi connection must run in a retry loop (backoff)
   rather than failing on the first attempt.

4. **Long session and message-edit limits.** *Verified (ADR-0009):* the Bot API
   lets a bot edit its own message for **48 hours** in a private chat or group, but
   a **channel** post has **no** such limit. So in a private chat the heartbeat's
   edits start failing ~48 h after a message was posted; in a channel the same post
   is edited indefinitely.
   **Fallback:** if `editMessageText` returns an error (cannot edit) — send a fresh
   message **silently** (`disable_notification`) and keep editing that one. Net
   effect: a private-chat recipient accumulates roughly one silent message every
   ~2 days; a channel recipient does not.

5. **Telegram rate limits.** Edits every 30–60 minutes are far within the limits;
   no issues expected.

6. **WiFi drop without power loss.** Auto-reconnect within a session is needed so
   the ability to heartbeat is not lost.

7. **Always-on reliability.** Enable the hardware watchdog (auto-reboot on hang).
   The onboard LED (GPIO8) indicates WiFi status: blinking while connecting,
   solid once connected.

## 8. Out of MVP scope (possible extensions)

- **Further inbound commands** (e.g. `/ping`) beyond the implemented `/status`,
  `/mute` and `/unmute` (ADR-0007, ADR-0008). These poll `getUpdates` and reply;
  the main reporting flow is otherwise one-way (device → chat).
- Storing silence history/durations (NVS currently holds only message state, not a
  log of outages — ADR-0008).
- An in-the-moment "power lost" notification (would require a supercapacitor +
  brownout detection).
- Multiple recipients / a dedicated channel.
- Web-based setup (WiFi provisioning) instead of `config.h`.

## 9. Agreed decisions and open parameters

Decided:

- Board — **ESP32-C3 SuperMini**.
- The device reports **liveness only** ("I'm alive"), never "power is back"; the
  operator infers power presence (ADR-0005).
- **Persistence & notification** — `message_id`, "online since" and a mute flag are
  kept in NVS; the boot is classified via `esp_reset_reason()`, so only a genuine,
  un-muted power-on notifies while soft reboots resume silently. `/mute` / `/unmute`
  toggle it; the device ships muted (ADR-0008).
- Timezone and heartbeat interval — set in **`config.h`** (`TZ_STRING`,
  `HEARTBEAT_MIN`); default interval — **30 minutes**. Timezone uses a DST-aware
  POSIX TZ string (see §6 "Time source").
- Recipient — **personal chat OR channel** (see §6, both supported).
- Onboard LED (GPIO8, active-low) is a **WiFi status indicator**: it blinks while
  connecting and stays solid once connected.

Still open (can be decided during implementation):

- The time-string format in the message (`24.07 14:32` — fine or otherwise).

Related ADRs: `docs/adr/` — 0001 (Arduino/PlatformIO), 0002 (heartbeat inference),
0003 (stateless / no NVS — **superseded by 0008**), 0004 (Telegram messaging
model), 0005 (report liveness, not power), 0006 (time source: NTP + POSIX TZ),
0007 (inbound `/status` command), 0008 (NVS persistence, reset-reason classifier,
mute toggle), 0009 (edit window: 48 h chats / unlimited channels), 0010 (preserve
NVS across repartition).
