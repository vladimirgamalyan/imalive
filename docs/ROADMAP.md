# Roadmap

Planned work. Once a feature is picked up, its design decisions get recorded
as [ADRs](adr/) as usual.

## Captive-portal provisioning (WiFi + Telegram bot)

Let a pre-flashed device be configured without touching the code: on first
boot (or after a config reset) the device raises a SoftAP with a setup portal
where the user picks a WiFi network, enters the password, and pastes a bot
token from [@BotFather](https://t.me/BotFather). The firmware validates both
live and discovers the `chat_id` automatically via a `t.me/<bot>?start=...`
deep link — no manual `getUpdates` spelunking. Credentials move from the
compile-time `include/config.h` into NVS; the `config.h` path stays for
development builds.

Follow-up once this lands: the firmware binary becomes configuration-free, so
a CI-built image works on any device — reinstate the `firmware.bin` asset in
tag releases (superseding ADR-0015), making OTA (ADR-0013) possible straight
from a GitHub release.
