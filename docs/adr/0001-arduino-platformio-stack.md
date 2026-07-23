# 0001. Use Arduino framework with PlatformIO for firmware

- Status: Accepted
- Date: 2026-07-24

## Context

The device is an always-on ESP32-C3 that must connect to WiFi, sync time over
NTP, and talk to the Telegram Bot API over HTTPS. Three stacks were considered:
ESP-IDF (native C), the Arduino framework (C++), and MicroPython.

Priorities are reliability for continuous operation and low implementation
friction for a small, mostly one-way feature. The functionality needed is
narrow: WiFi, TLS HTTP client, NTP, and two Bot API calls.

## Decision

Write the firmware in C++ on the **Arduino framework**, built with **PlatformIO**.

## Consequences

- Easier: mature, ready-made libraries for WiFi, TLS (`WiFiClientSecure`), NTP,
  and Telegram reduce the code we own; faster to a working device.
- Easier: PlatformIO gives reproducible builds and dependency management.
- Harder / traded away: less low-level control than ESP-IDF (task scheduling,
  power management, fine-grained watchdog control) — acceptable for this scope.
- Rejected MicroPython: convenient to iterate, but considered less robust for an
  unattended always-on device.
- Rejected ESP-IDF: maximum control, but more effort than this narrow feature
  warrants.
