# 0017. Drop the `/update` command

- Status: Accepted
- Date: 2026-07-31

## Context

ADR-0016 made any `.bin` document from an admin a firmware image, so nothing
about an update is triggered by a command any more. What remained of `/update`
was vestigial: an entry in the `setMyCommands` menu and a text handler that only
replied "just send the `.bin`".

Keeping it costs clarity. A command in the `/` menu reads as a step the operator
must perform, which is exactly the misunderstanding ADR-0016 set out to remove.

## Decision

Remove `/update` entirely — from the `setMyCommands` menu, from the command
handler, and from the README. The bot's menu is `/status`, `/mute`, `/unmute`.

Sending `/update` as text is now ignored like any other unrecognised text.

This amends ADR-0016's "a bare `/update` text message continues to reply with
usage instructions". The OTA mechanism itself (ADR-0013, as amended by
ADR-0016) is unchanged.

## Consequences

- The `/` menu lists only commands that do something.
- An operator used to the old flow gets no hint back when typing `/update`; the
  README documents the document-drop flow under its own section instead.
- Captions were already ignored (ADR-0016), so old habits — sending the `.bin`
  captioned `/update` — keep working unchanged.
