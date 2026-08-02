# 0018. Fall back to a fresh message only when Telegram rejects the edit

- Status: Accepted
- Date: 2026-08-02

## Context

ADR-0004 defined the fallback as "on an edit error, send a fresh message and
continue editing that one", and the firmware took that literally: `tgEditMessage`
returned a single `bool`, so a stalled TLS handshake, a read timeout, a 5xx or a
429 flood wait were indistinguishable from Telegram answering "this message
cannot be edited". Any one of them cost a new message.

This was observed in production. The channel recipient
`Electricity - The Fine Place Resort` had been editing the same post for nine
days when, at 09:10 on 2 Aug 2026, the heartbeat posted a fresh one. The
admin chat holds no boot notice for that moment, so there was no reboot; the new
message was created 12 s after the tick, which is the 10 s `TLS_HANDSHAKE_S` cap
plus a reconnect — a network hiccup, not a rejection. The rotation ADR-0009
predicts for private chats had happened in a channel, where no edit window
applies.

The two failures need opposite responses. A rejected edit means the message is
gone or frozen, so retrying it forever would leave the device silent. A transient
failure means the message is intact and reachable a moment later, so replacing it
throws away a working message — and, on a flaky link, does so repeatedly.

## Decision

Classify the outcome of `editMessageText` instead of collapsing it to a bool:

- **HTTP 400** — Telegram parsed the request and refused it (message deleted, past
  the 48 h chat window, ...). *Rejected*: post a fresh **silent** message and
  track that one, exactly as ADR-0004 and ADR-0008 describe.
- **Anything else** — no HTTP status at all, a 5xx, a 429. *Transient*: retry the
  edit up to `EDIT_ATTEMPTS` (3) times, `EDIT_RETRY_MS` (2 s) apart. If every
  attempt fails, keep the tracked `message_id` and skip this update; the next
  heartbeat resumes it.
- `"message is not modified"` still counts as success, unchanged.

The same classification governs the resume path in `setup()`: a boot that cannot
reach Telegram no longer duplicates the message it was trying to resume.

The worst case is 3 × 10 s of handshake plus 2 × 2 s of backoff ≈ 34 s. Every
request feeds the task watchdog on its way out, so the burst cannot trip the 60 s
`WDT_TIMEOUT_S`, and it fits inside even a one-minute heartbeat.

## Consequences

- A network hiccup costs a stale "Last seen" for one interval instead of a
  permanent extra message in the chat. Reading a slightly older timestamp is the
  cheaper failure: §7.1 already says the operator cannot distinguish "no power"
  from "no internet", and one missed edit does not change that judgement.
- A genuinely un-editable message is still replaced, so the 48 h private-chat
  rotation of ADR-0009 works as before — only now it is not triggered early.
- If Telegram itself is unreachable for longer than a heartbeat interval, the
  message simply stops updating, which is indistinguishable from the device being
  down. That is the intended semantics (ADR-0005: liveness, not power).
- 400 is treated as terminal wholesale. A 400 from a cause other than the message
  itself (say a malformed request after a future change) would post a fresh
  message per heartbeat — noisy, but self-evident in the chat and in the serial
  log, which prints the API description on every non-200.
- Refines the fallback of ADR-0004, ADR-0008 §2 and ADR-0009; the messaging model
  is otherwise unchanged.
