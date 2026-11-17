---
description: Identify the debug probe and GD32 target; show backup status
---
Run `./scripts/probe-info.sh` and interpret the output for me.

Confirm specifically:
- Which probe was detected, and whether the required tooling is installed
- Whether the DBGMCU IDCODE and reported flash size match a GD32F303CC (256 KB)
- Whether a verified backup exists (and therefore whether flashing is unblocked)

If no probe is detected, say what to check: USB connection, and the four wires
to the 5-pin header (PA14=SWCLK, PA13=SWDIO, GND, RST).
If identity reads fail but the probe is present, suggest `SWD_SPEED=1000`.
