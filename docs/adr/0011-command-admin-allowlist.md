# 0011. Restrict commands to an admin allowlist

- Status: Accepted
- Date: 2026-07-24

## Context

Inbound commands (`/status`, `/mute`, `/unmute`) arrive via `getUpdates` from
anyone who direct-messages the bot (ADR-0007). Two problems follow: `/status` now
reveals device configuration (WiFi SSID, chat id, timezone, the admin list), and
`/mute` / `/unmute` change behaviour — so answering arbitrary senders leaks config
and lets any stranger who finds the bot toggle notifications.

## Decision

Add a config allowlist `TG_ADMIN_IDS` (a brace-list of Telegram **user** IDs). A
command is processed only if `message.from.id` is in the list; other senders are
acknowledged (the update offset still advances, so they are not re-processed) but
receive no reply. The effective allowlist is shown in `/status`.

`TG_ADMIN_IDS` lives in `config.h` (untracked); `config.example.h` ships a
placeholder.

## Consequences

- Only listed admins can query status or toggle mute; a stranger messaging the bot
  gets silence.
- An empty list means **no one** is allowed — fail-safe, not fail-open. At least
  one id must be configured for commands to work.
- The id checked is the sender (`from.id`), which for a private chat with the bot
  equals that user's id — independent of where heartbeats are posted (a channel).
- Relates to ADR-0007 (command polling).
