# 0006. Time source: NTP with a POSIX TZ string (DST-aware)

- Status: Accepted
- Date: 2026-07-24

## Context

The ESP32-C3 SuperMini has no battery-backed RTC, so it holds no wall-clock time
at boot. Messages need local timestamps ("online since", "last seen"). Two
sub-choices: how to obtain the time, and how to represent the timezone — a fixed
numeric offset versus a POSIX TZ string that encodes daylight-saving rules.

## Decision

Obtain time from NTP (SNTP, `pool.ntp.org`) on every boot; keep it with the
internal timer plus periodic re-sync during a session. Represent the timezone as
a POSIX TZ string in config (`TZ_STRING`, applied via `configTzTime`), not a
fixed offset, so DST transitions are handled automatically. Send the first
message only after a successful sync. This project ships with `ICT-7`
(Asia/Bangkok, UTC+7, no DST).

## Consequences

- No RTC battery and no NVS needed; consistent with the stateless model
  (ADR-0003) — each boot simply re-syncs.
- DST is correct without firmware changes in zones that observe it; changing
  location is a one-line config edit.
- Dependency: time requires the network — acceptable, because the device also
  needs the network to reach Telegram, so it can never report yet lack the time.
- Failure handling: if NTP does not sync within a timeout, the device restarts
  rather than sending a wrong timestamp.
- Refines the time note in CONCEPT.md §6 ("Time source").
