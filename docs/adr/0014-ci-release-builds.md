# 0014. CI release builds from version tags

- Status: Superseded by ADR-0015
- Date: 2026-07-25

## Context

Firmware is normally built locally: credentials live in the git-ignored
`include/config.h` and are baked in at compile time. CI (`build.yml`) already
proves every push compiles by substituting `config.example.h`, but no binary is
tied to a version number — OTA (ADR-0013) flashes whatever the operator last
built, and old versions are not reconstructible without checking out and
rebuilding the exact commit.

## Decision

A `release` workflow triggers on `v*` tags, builds exactly like CI (template
config), and publishes `imalive-<tag>.bin` as a GitHub release asset with
auto-generated notes plus a fixed warning about the placeholder credentials.

## Consequences

- Every version tag yields a canonical reference binary and a release-notes
  page for free; the version history becomes browsable outside git.
- The asset is a **reference artifact, not an OTA payload**: it carries
  `config.example.h` placeholders, so flashed onto a configured device it
  would boot but never reach WiFi, leaving the device unreachable until a USB
  reflash. The ADR-0013 boot-count rollback would NOT catch this - it counts
  only boots that fail, and this image boots successfully. Devices must keep
  receiving locally built binaries; the release notes state this.
- Nothing enforces that `FW_VERSION` in `src/main.cpp` matches the tag; keeping
  them in sync stays an operator responsibility.
