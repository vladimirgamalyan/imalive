# 0002. Detect power state by presence inference, not by an active power-loss signal

- Status: Accepted
- Date: 2026-07-24

> Message wording refined by ADR-0005: the device reports liveness ("I'm alive"),
> not "power is back". The detection mechanism below is unchanged.

## Context

The goal is to know whether mains power is present at a location. Because the
device is powered by that same mains supply, its own power state already carries
the signal: online means power is present.

Reporting a power *loss* in the moment is hard: when power drops, the local
router is usually unpowered too, so the device has neither time nor a network
path to send a final "power lost" message. Sending one would require extra
hardware (a supercapacitor / power reserve plus brownout detection).

## Decision

Treat device presence as the power signal. On power-on the device sends a
"power is back" message; while powered it periodically emits a heartbeat. Power
loss is **inferred** from the heartbeat no longer updating. Do **not** add a
supercapacitor/brownout path and do **not** attempt an in-the-moment "power
lost" notification.

## Consequences

- Easier: no extra hardware; the sensor is the power supply itself.
- The time resolution of a detected outage equals the heartbeat interval N
  (the outage lies within [last heartbeat; last heartbeat + N]).
- Limitation: "no power", "no internet/router", and "device hung" are
  indistinguishable — all appear as a stopped heartbeat. Accepted (see
  CONCEPT.md §7).
- "Power lost" is never an explicit event; the observer reads it from a stale
  "last seen" value.
- Reversible later by adding a supercapacitor + brownout ADR if an in-the-moment
  loss notification becomes required.
