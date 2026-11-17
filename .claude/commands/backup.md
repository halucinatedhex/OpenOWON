---
description: Back up the stock firmware over SWD (read-only, two-pass verified)
---
Run `./scripts/backup.sh` to dump the stock firmware.

This is read-only — it never writes to the chip. It reads all 256 KB twice and
only marks the result verified if both passes are byte-identical.

After it runs, report:
- Whether the backup is VERIFIED, and its sha256 and path
- If it exited 3, the dump was all 0x00 or 0xFF, meaning readout protection is
  enabled. Do NOT suggest ways around that. Instead walk me through
  `docs/readout-protection.md`: the next step is dumping the I2C EEPROM to find
  out whether calibration is recoverable, before any decision about flashing.
- If the two passes differed, suggest retrying with `SWD_SPEED=1000`.

Never offer to proceed to flashing in the same breath as a failed backup.
