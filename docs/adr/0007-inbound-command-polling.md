# 0007. Optional inbound command polling for /status

- Status: Accepted
- Date: 2026-07-24

## Context

The device is otherwise a one-way reporter (CONCEPT.md §8). It is useful to query
its state on demand — uptime, WiFi signal, IP, when it came online — without
waiting for the next heartbeat. Telegram delivers inbound messages via `getUpdates`
(polling) or a webhook; a webhook needs a public HTTPS endpoint the device does
not have.

## Decision

Add short-poll `getUpdates` on a timer (default every few seconds), restricted to
message updates, and handle a single command — `/status` — by replying to the
requesting chat with a status summary. Track the update offset in RAM (no NVS) and
drain any pre-boot updates at startup so stale commands are not answered.

## Consequences

- The device is no longer strictly one-way; a user can DM the bot `/status`.
- Cost: continuous polling adds a few TLS requests per minute even when idle —
  acceptable for a mains-powered device.
- The offset is not persisted (consistent with ADR-0003); on reboot pre-boot
  updates are drained once so old commands are ignored.
- Only `/status` is handled; more commands can follow the same pattern.
- Chat ids are handled as 64-bit to support large Telegram ids.
