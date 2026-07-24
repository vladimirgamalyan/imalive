# 0009. Telegram message-edit window: 48 h in chats, unlimited in channels

- Status: Accepted
- Date: 2026-07-24

## Context

The heartbeat keeps editing one message via `editMessageText` (ADR-0004). How
long an old message stays editable was left open in ADR-0004 and CONCEPT §7.4
("the exact limit **must be verified**").

## Decision

Per Telegram's published limits (confirmable once the device has run past 48 h):

- **Private chat / group:** a bot may edit its own message for **48 hours** after
  it was sent; after that `editMessageText` fails.
- **Channel:** channel posts are **not** subject to the 48 h limit — the bot, as a
  channel administrator, can edit the same post indefinitely.

We accept this instead of working around it. The fallback from ADR-0008 already
covers the chat case: on an edit failure the firmware posts a fresh **silent**
message (`disable_notification`) and continues editing that one.

## Consequences

- **Private-chat recipient:** the tracked message rotates roughly **every ~2
  days** — a new *silent* message (no notification) carrying the same persistent
  `online_since`. The chat slowly accumulates about one message per two days.
- **Channel recipient:** no rotation; a single post is edited forever.
- To avoid accumulation entirely, use a **channel** as the recipient
  (`TG_CHAT_ID` = `-100…`, bot added as administrator). CONCEPT §6 already supports
  both recipient types.
- Resolves the open edit-limit item in ADR-0004 and CONCEPT §7.4.

## Sources

- Telegram — "edit any message within 48 hours": <https://x.com/telegram/status/1168523951695437824>
- Telegram Limits: <https://limits.tginfo.me/en>
- Telegram Bots FAQ: <https://core.telegram.org/bots/faq>
