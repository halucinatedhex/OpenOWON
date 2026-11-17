#!/usr/bin/env bash
# First-flash smoke test: build the pin-free firmware, flash it into the app
# slot, capture its RTT console and check the boot log.
#
# WRITES THE CHIP (through scripts/flash.sh and all its guards). Requires a
# person at a terminal to confirm; it refuses to run without one, so it cannot
# be started by automation. Run it yourself:   ! ./scripts/hil-smoke.sh
# Put stock back afterwards with ./scripts/restore.sh.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[[ -t 0 ]] || die "hil-smoke writes the chip and needs a person at a terminal to confirm."
backup_is_verified || die "No verified backup. Run ./scripts/backup.sh first."

log "Building the pin-free image (drives no pins)"
pio run -e xdm1241 >/dev/null || die "build failed"
IMG="$ROOT/.pio/build/xdm1241/firmware.bin"

cat <<MSG

  About to replace the stock application (0x08002800) with the pin-free test
  firmware. The bootloader and calibration are not touched. Restore stock
  afterwards with ./scripts/restore.sh.

MSG
read -r -p "Type FLASH to continue: " ok
[[ "$ok" == "FLASH" ]] || die "Aborted."

"$ROOT/scripts/flash.sh" "$IMG"
"$ROOT/scripts/rtt-capture.sh" 6 "$ROOT/build/rtt-boot.log"
echo
python3 "$ROOT/scripts/check-boot-log.py" "$ROOT/build/rtt-boot.log"
