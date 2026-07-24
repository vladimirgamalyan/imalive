# 0008. Persist message state in NVS; classify boot by reset reason; mute toggle

- Status: Accepted
- Date: 2026-07-24

> Refined by ADR-0010: how to preserve this NVS state across a partition-table
> change.

## Context

ADR-0003 kept the device stateless: `message_id` lived only in RAM, so *any*
reboot started fresh and sent a new, notifying message. That has two problems:

- **False "power is back" pings.** A watchdog crash or an OTA reboot — where
  power was never cut — produced a new notification (ADR-0003 §7.2), as did a
  planned power-off for reflashing or moving the device to another socket.
- **The tracked message was lost** on every reboot, so it could not be resumed.

We want a notification only on a genuine mains power-on; silence for soft reboots
(watchdog / OTA) and for planned power-offs; and the tracked message to survive
reboots so it is resumed in place.

Two hardware facts constrain the solution: the board has no RTC battery and no
supercapacitor, so **only NVS survives a power loss** (RTC memory does not); and
a Telegram bot cannot read chat history, so `message_id` cannot be recovered from
the chat. Therefore the message state must be persisted to NVS.

## Decision

1. **Persist `{ message_id, online_since_epoch, muted }` in NVS.** Writes happen
   only when a new message is created or the mute flag is toggled — both rare —
   so flash wear is negligible. The heartbeat's `Last seen` is still never
   persisted. This *narrows* ADR-0003 (no frequent NVS writes), not abandons it.

2. **Classify each boot with `esp_reset_reason()`:**
   - `ESP_RST_POWERON` **and** not muted → *announce*: send a new message **with**
     a notification and set `online_since = now`. The previous message stays
     frozen as the outage record.
   - Any other reason (software / watchdog / brownout / …) **or** muted →
     *continuation*: silently **edit** the persisted message and keep the
     persistent `online_since`. If it can no longer be edited (deleted or past
     Telegram's edit window), post a fresh **silent** message.

3. **Add `/mute` and `/unmute` chat commands** — a persistent toggle, **muted by
   default** (the device ships silent; `/unmute` enables power-on pings). `/mute`
   is sent before a planned power-off; its acknowledgement confirms the flag is
   persisted, so it is then safe to cut power. `/unmute` arms. `/status` shows
   `Mute: on/off`. The message still only asserts liveness (ADR-0005 unchanged);
   the notification merely alerts on a real return.

`online_since` is thus continuous across soft reboots and planned restarts and
resets only on a real armed power-on. Uptime since *this* boot is tracked
separately and shown in `/status`.

## Consequences

- No false ping on watchdog / OTA / software reboots (fixes ADR-0003 §7.2) or on
  a planned muted restart. A genuine mains return still pings.
- The tracked message survives reboots and is resumed in place.
- The device **starts muted**, so the first power-on is silent and pings stay off
  until the first `/unmute` — handy for the reboot-heavy setup/testing phase.
- **Footgun:** the mute toggle is persistent — if left muted, the next real
  power-on will not ping. The state is visible in `/status`.
- **Edge — NTP retry:** a real power-on whose first NTP sync fails triggers
  `ESP.restart()` (ADR-0006); its reset reason is software, so that return
  resumes silently instead of pinging. Rare (NTP normally syncs once WiFi is up).
- **Edge — brief full outage:** a full outage of only a few seconds still reports
  `ESP_RST_POWERON` and pings. Short-outage filtering was intentionally not added
  (it would need outage-duration measurement, i.e. frequent NVS writes).
- **USB reflash:** whatever reset reason `esptool` triggers, sending `/mute`
  first guarantees a silent resume; an unmuted bench reflash may ping (harmless at
  the bench).
- Supersedes ADR-0003; refines the messaging model of ADR-0004.
