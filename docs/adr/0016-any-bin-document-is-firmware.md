# 0016. Any .bin document from an admin is a firmware image

- Status: Accepted
- Date: 2026-07-30

## Context

ADR-0013 §1 gates OTA updates behind a caption: a firmware document is only
flashed when it arrives captioned `/update`. The intent was to make flashing an
explicit operator action rather than an accident.

In practice the gate costs more than it buys:

- Adding a caption is extra manual work on every update, and on mobile it is
  the step most easily forgotten — the document then lands in the chat and is
  silently ignored, which reads as a device failure rather than a missing
  caption.
- The device has exactly one operator, who is on the admin allowlist
  (ADR-0011). Nobody else can reach the OTA path at all.
- The accident the gate guards against — an admin dropping an unrelated `.bin`
  into the bot's private chat — is not a realistic event for this device. That
  chat exists solely to talk to the device.

## Decision

Treat **any document whose file name ends in `.bin`** (case-insensitive) from an
admin as a firmware image. The name is the only gate; captions are ignored, so a
document still captioned `/update` keeps working.

A bare `/update` **text** message continues to reply with usage instructions,
now telling the operator to just send the `.bin`.

This amends ADR-0013 §1 only. Everything else in ADR-0013 stands unchanged:
`getFile` streaming into the free slot, the confirm-or-rollback boot counter,
and the reset-reason model.

## Consequences

- The update cycle is `pio run` → drop the `.bin` into the chat, with nothing to
  remember.
- The `.bin` extension keeps ordinary attachments (photos, documents, logs) from
  entering the OTA path, so a mistaken flash requires sending a file that is
  already named like firmware.
- The protection ADR-0013 attributed to the caption is gone: an admin who sends
  *any* `.bin` now flashes it. The remaining safety net is the one that actually
  covers the dangerous case — a valid-but-wrong image — namely the boot-count
  rollback, which restores the previous slot when the new image fails to
  confirm. A foreign image that boots but lacks the confirmation code is
  therefore rolled back rather than kept.
- `setMyCommands` and the `/update` usage reply change wording; no protocol or
  NVS change, so nothing about ADR-0010 is affected.
