---
description: Run host-native unit tests (no hardware needed)
---
Run `pio test -e native` and report results.

These compile for the Mac and test the pure logic in `lib/measure/` — no probe,
no chip. If a test fails, show the assertion and fix the logic, not the test,
unless the test's expectation is itself wrong.

If I've added hardware-touching code to `lib/`, point out that it breaks the
host build and should move behind the HAL seam instead.
