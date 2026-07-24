# 0012. Phase-lock heartbeat edits to round wall-clock times

- Status: Accepted
- Date: 2026-07-24

## Context

The heartbeat was scheduled off a relative `millis()` timer: the first edit fired
`HEARTBEAT_MIN` minutes after boot and every interval thereafter, so its phase was
whatever the power-on moment happened to be. Powering on at 13:11 with a 5-minute
interval produced "Last seen" values of 13:16, 13:21, 13:26 — arbitrary digits.

The operator reads these values by eye to judge freshness and estimate the silence
window; off-grid digits make that arithmetic harder, and several devices sharing a
channel each drift to their own phase, so their timestamps cannot be compared at a
glance. Aligning the edits to round wall-clock times (13:15, 13:20, …) is a
readability win, not a functional one.

## Decision

Add a compile-time flag `HEARTBEAT_ALIGN` (config.h, default on). When it is set
**and** the interval divides an hour evenly (`60 % HEARTBEAT_MIN == 0`, i.e.
5/10/15/20/30/60), each heartbeat is scheduled at the next **local** minute that is
a multiple of `HEARTBEAT_MIN` with seconds zeroed. The target epoch is recomputed
from `getLocalTime` + `mktime` after every edit, not stepped from a fixed origin,
so it tracks NTP re-syncs and never drifts off the displayed minute.

A guard skips any boundary falling within `HEARTBEAT_GUARD_S` (30 s) of the
previous update, so a power-on landing just before a boundary does not fire two
edits seconds apart. 30 s is a wide margin under Telegram's ~1 request/second
per-chat limit (ADR-0004 already keeps heartbeats silent, so this cap is about the
boot collision, not notification spam).

Intervals that do not divide 60, or `HEARTBEAT_ALIGN 0`, keep the original
relative-`millis()` behaviour unchanged. Only the heartbeat scheduler in
`setup()`/`loop()` changed; the notification, mute, edit-fallback and NVS logic are
untouched.

## Consequences

- "Last seen" values land on a tidy grid (13:15, 13:20, …); the inferred silence
  window gets round bounds too. Multiple devices in one channel become visually
  synchronized and directly comparable.
- The **first** interval after boot is shorter than `HEARTBEAT_MIN` by design (a
  13:11 power-on edits at 13:15), and the boot message's own "Last seen" stays
  off-grid until the first aligned edit — accepted.
- Alignment depends on the wall clock, which the design already requires (ADR-0006,
  NTP mandatory before any send). Recomputing per cycle avoids the `millis()`-drift
  case where an edit fires at 13:19:59 and displays the wrong minute.
- Only divisors of 60 are aligned; this is exactly the condition under which the
  grid tiles an hour cleanly. DST transitions can make `mktime` fire one edit an
  hour early/late twice a year — negligible, and moot for the shipped no-DST TZ
  (`ICT-7`).
- Relates to ADR-0004 (single-message heartbeat-edit model) and ADR-0006 (time
  source: NTP + POSIX TZ).
