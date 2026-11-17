---
description: Verify chip contents against a .bin (read-only)
---
Run `./scripts/verify.sh $ARGUMENTS` (no argument = current build, else latest backup).

Report clearly whether the chip matches the image. If it does not, say what that
implies: either the flash didn't take, the wrong image was compared, or the
chip holds different firmware than expected. Do not re-flash to "fix" a failed
verify without asking me first.
