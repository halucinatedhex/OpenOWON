---
description: Start a GDB server and attach for interactive debugging
---
Start the GDB server with `./scripts/debug.sh` in the background, then attach.

Useful first checks on the boot skeleton in `src/main.c`:
- `print boot_marker` should read 0xA11C0DE if the firmware is running
- `print heartbeat` twice, a moment apart, should show it incrementing
- `info registers` and `bt` if the target appears halted or faulted

Report what the target is doing. If the core will not halt, suggest connecting
RST and trying `SWD_SPEED=1000`.
Remember to stop the GDB server when we're done.
