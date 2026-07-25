# 0013. OTA updates via a Telegram firmware document, with boot-count rollback

- Status: Accepted
- Date: 2026-07-25

## Context

The device is deployed at a remote location; reflashing over USB requires a
physical visit. Telegram is the device's only channel (ADR-0004, ADR-0007), and
the default partition table already provides two OTA app slots (`app0`/`app1`,
1280 KB each) plus `otadata` — the image currently occupies ~72 % of a slot.

Two forces shape the design:

- **No new infrastructure.** A separate update server or public firmware
  hosting would add a second moving part to an otherwise single-channel device.
- **A bad update must not brick a remote device.** The prebuilt Arduino
  bootloader ships with automatic rollback disabled, so any safety net must
  live at the application level. `Update.end()` verifies image integrity
  (magic byte / checksum), which catches corrupt transfers — but not a build
  that crashes at runtime.

## Decision

1. **Deliver firmware over Telegram itself.** An admin (ADR-0011) sends the
   built `firmware.bin` as a document with caption `/update`. The command
   poller resolves it via `getFile`, streams it from `api.telegram.org` into
   the free OTA slot (`Update.h`), and reboots. Bot API downloads are capped
   at 20 MB — an order of magnitude above the image size. A bare `/update`
   text message replies with usage instructions.

2. **Confirm-or-rollback watchdog** on NVS keys `ota_state`, `ota_boots`,
   `ota_chat`:
   - Before rebooting into the new image, the state is set to *pending* and
     the initiating chat is recorded.
   - Each boot while pending increments `ota_boots` — before networking, so
     even a firmware that never gets online is counted.
   - The first successful update of the tracked message confirms the image:
     the state is cleared and "Update confirmed" is reported to the
     initiating chat.
   - If a boot starts with the counter already exhausted (`> 3`),
     `Update.rollBack()` restores the previous slot and marks the state
     *rolled back*; the previous (OTA-aware) firmware reports the failure
     once online.

3. **Reset-reason model unchanged.** The post-update reboot is a software
   reset, so the ADR-0008 logic resumes the tracked message silently — an
   update never produces a false "power is back" ping. `/status` shows the
   running slot and version for verification.

## Consequences

- Remote updates with zero extra infrastructure; authorization is the existing
  admin allowlist; the whole cycle is `pio run` → drop the `.bin` into the chat.
- Rollback is safe by construction: the spare slot always holds the image that
  was previously running (and confirmed) — a failed update lands back on
  working firmware.
- A hang in the new firmware becomes a WDT reset, which the boot counter also
  catches.
- The watchdog counts *any* reboot while pending — e.g. three consecutive NTP
  failures (ADR-0006 restarts) would roll back a perfectly good image. This is
  accepted: the outcome is a working firmware either way, plus a report.
- The very first rollback target (a pre-OTA firmware) cannot report the
  failure; the operator sees it via the `/status` version instead. From the
  next release on, both slots are OTA-aware.
- TLS to Telegram remains unauthenticated (`setInsecure()`, as elsewhere in
  the firmware): in principle an MITM could substitute a *valid* foreign
  image. Accepted for a personal device — consistent with the existing
  tradeoff noted in the code.
- Any valid ESP32-C3 image is accepted, including one from a different
  project; a wrong-but-bootable image would neither confirm *nor* roll back
  (it lacks the watchdog code). The caption gate makes flashing an explicit
  operator action rather than an accident.
- The firmware must keep fitting a 1280 KB slot; `Update.begin()` enforces
  this and the failure is reported to the chat.
- Relies on ADR-0010 trivially: the partition table is untouched and the new
  NVS keys are additive.
