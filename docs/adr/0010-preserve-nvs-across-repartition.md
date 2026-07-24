# 0010. Preserve the NVS partition across partition-table changes

- Status: Accepted
- Date: 2026-07-24

## Context

ADR-0008 stores `message_id`, `online_since`, and the mute flag in NVS. A future
firmware may outgrow the app partition (currently ~70 % of the 1.25 MB app slot in
the default table) and need a new partition layout. Changing the layout must not
silently wipe the saved NVS state.

NVS content is just bytes at a fixed flash offset (`0x9000`, size `0x5000` in the
default layout); the partition table only *maps* names to offsets. The app slot
starts at `0x10000`, above both `nvs` and `otadata`, and grows toward higher
addresses — so enlarging the app does not physically reach `nvs`.

## Decision

When changing the partition table, keep the `nvs` entry **byte-identical** (same
offset and size) and reclaim app space from the OTA / SPIFFS / coredump regions,
which sit at higher offsets. Do **not** run a full `erase_flash` when flashing the
new table.

The standard Arduino "bigger app" schemes (`huge_app.csv`, `min_spiffs.csv`,
`no_ota.csv`) already keep `nvs` at `0x9000 / 0x5000`, so switching to one of them
preserves the stored data.

## Consequences

- NVS data survives a partition-map change as long as the `nvs` entry is unchanged
  and no full erase is performed.
- NVS is lost by a full `erase_flash` / `--erase-all`, or by moving or resizing the
  `nvs` entry. This is **non-fatal by design**: on empty NVS the device creates a
  new *silent* message, resets `online_since`, and reverts to muted (ADR-0008) —
  only message continuity and the mute setting are lost.
- Refines ADR-0008.
