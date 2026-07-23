# 0005. Report device liveness, not power presence, in messages

- Status: Accepted
- Date: 2026-07-24

## Context

The device cannot actually distinguish "mains power present" from "internet up"
or "firmware healthy" — all it truly knows is that it is running and online (see
CONCEPT.md §7 and ADR-0002). Earlier example wording ("power is back") asserted
more than the device can know and could mislead the reader, e.g. during an
internet-only outage or a hang. The project is named *imalive* / "I'm alive",
which already frames the honest primitive.

## Decision

Messages assert **only that the device is alive/online**, never "power is back"
or that power is present. The device sends an "I'm alive" message on start and a
heartbeat that updates a "last seen" line. Inferring mains-power presence from
that liveness is left to the human operator.

## Consequences

- Honest: the device never claims knowledge it lacks; no misleading signal during
  an internet-only outage or a hang.
- The operator interprets liveness as power presence themselves, consistent with
  the limitation in CONCEPT.md §7.
- Product wording is aligned with the project name.
- Refines the message wording used in ADR-0002 and ADR-0004; the detection
  mechanism (0002) and the message lifecycle (0004) are unchanged.
