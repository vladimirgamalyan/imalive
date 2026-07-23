# 0003. Operate statelessly — do not persist state to NVS

- Status: Accepted
- Date: 2026-07-24

## Context

Across reboots the device could persist data in NVS (non-volatile storage), for
example the current Telegram `message_id` or the last heartbeat time. Persisting
`message_id` would let the device keep editing the same message after a reboot;
persisting the last heartbeat time would let it compute outage duration on the
next start. Both add complexity and flash-wear considerations.

## Decision

Keep the device **stateless across reboots**: store nothing in NVS. `message_id`
lives only in RAM. Every power-on starts fresh: sync time, send a new "power is
back" message, then heartbeat-edit that message during the session.

## Consequences

- Easier: no NVS layout, no flash-wear management, simpler code; a power-on maps
  cleanly to a new chat message, matching the intended real power-cycle behavior.
- Limitation (false positive): a spontaneous reboot (crash / watchdog) is
  indistinguishable from a real power cycle and produces a new "power is back"
  message even though power was never cut. Accepted (see CONCEPT.md §7).
- The device cannot compute outage duration on the next start; the human reads it
  from the previous chat message.
- Constrains related decisions: because `message_id` is not persisted, the
  messaging model is necessarily one new message per power-on (see ADR-0004).
