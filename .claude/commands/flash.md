---
description: Program the chip (destructive - requires a verified backup)
---
Flash firmware to the GD32F303 using `./scripts/flash.sh $ARGUMENTS`.

Before running it:
1. Confirm a verified backup exists (`./scripts/backup.sh` output, or
   `backups/latest/VERIFIED`). If none exists, STOP and tell me — do not pass
   `--i-accept-no-backup` on your own initiative. That flag is mine to choose.
2. Confirm the image exists and state its size and sha256.
3. Tell me what is about to be overwritten and wait for my go-ahead.

After flashing, the script verifies automatically. Report the verify result.
If it fails, stop and report — do not retry in a loop.
