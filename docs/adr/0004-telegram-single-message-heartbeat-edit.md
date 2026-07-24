# 0004. One Telegram message per power-on, updated in place via editMessageText

- Status: Accepted
- Date: 2026-07-24

> Message wording refined by ADR-0005: the "power is back" message is an
> "I'm alive" message. The one-message-per-power-on lifecycle below is unchanged.
>
> Refined by ADR-0008: the `message_id` now persists in NVS, so a soft reboot or
> a muted planned restart resumes the same message instead of posting a new one;
> the power-on notification is conditional (only a genuine, unmuted power-on).
>
> Edit-window risk (below) resolved by ADR-0009: editing is limited to 48 h in a
> chat or group, but unlimited in a channel.

## Context

The device must signal "power is back" and then prove it is still alive over
time, without spamming the chat with repeated notifications. Telegram raises a
notification for a new message (`sendMessage`) but not for an edit
(`editMessageText`). State is not persisted across reboots (see ADR-0003), so a
`message_id` is only valid within a single power session.

## Decision

Per power-on, send exactly **one** new message ("power is back") and keep its
`message_id` in RAM. Emit the heartbeat by **editing that same message**
in place (updating a "last seen" line) every N minutes. Do not send a new
message per heartbeat.

## Consequences

- Only one sound-notification per power-on; heartbeats are silent edits — the
  chat is not spammed.
- The chat history naturally records one message per power-on event; a frozen
  "last seen" marks how long power was present.
- Edit-limit risk: a session lasting days keeps editing an old message, which
  Telegram may refuse to edit (exact limit to be verified during
  implementation). Fallback: on an edit error, send a fresh message and continue
  editing that one; optionally rotate the message on a schedule.
- Both a personal chat and a channel are valid recipients; for a channel the bot
  must be an administrator with post permission.
