# 0015. Tag releases carry no firmware binary

- Status: Accepted
- Date: 2026-07-25

## Context

ADR-0014 attached a CI-built `imalive-<tag>.bin` to every version-tag release.
That binary is built from `config.example.h`, so it cannot work on any real
device: credentials are compile-time constants, and there is no device it
could connect for. Despite the warning in the release notes, a downloadable
`.bin` right next to the OTA instructions invites flashing it onto a remote
device, which would strand the device offline until a USB reflash (the
ADR-0013 rollback does not trigger — the image boots fine). A reference
binary nobody can use turned out to be pure confusion.

## Decision

Keep the tag-triggered release workflow, but publish only the release page
with auto-generated notes — no firmware asset. Firmware continues to be built
locally with the operator's `config.h` and delivered over Telegram OTA
(ADR-0013).

Supersedes ADR-0014.

## Consequences

- Releases remain browsable version milestones with nothing misleading to
  download.
- No canonical binary per version: reproducing an old build means checking out
  the tag and building locally (`build.yml` still proves every push compiles).
- The release job no longer needs Python or PlatformIO and finishes in
  seconds.
