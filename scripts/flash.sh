#!/usr/bin/env bash
# Write firmware to the chip. THIS IS THE DESTRUCTIVE ONE.
#
#   flash.sh [image.bin] [--i-accept-no-backup]
#       Program an application image into the app slot at 0x08002800, behind
#       OWON's bootloader (docs/boot.md). The default image is the pin-free
#       build, .pio/build/xdm1241/firmware.bin.
#
#   flash.sh <image.bin> --restore
#       Used by scripts/restore.sh only: program a bootloader+app image at
#       0x08000000. Still stops short of the protected storage.
#
# Guards, in order (none can be skipped by flags except where noted):
#   1. the image never reaches 0x0803D800 (log, prefs, factory calibration)
#   2. app images are written at 0x08002800, never over the bootloader
#   3. the image's own vector table must be linked for the address it is
#      going to - catches a 0x08000000-linked build headed for the app slot
#   4. a verified backup must exist (override: --i-accept-no-backup, which
#      also asks you to type ERASE)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

APP_BASE=0x08002800
BOOT_BASE=0x08000000
PROTECT_BASE=0x0803D800

OVERRIDE=no
RESTORE=no
IMG=""
for a in "$@"; do
  case "$a" in
    --i-accept-no-backup) OVERRIDE=yes ;;
    --restore)            RESTORE=yes ;;
    -*)                   die "Unknown option $a" ;;
    *)                    IMG="$a" ;;
  esac
done
IMG="${IMG:-$BUILD_DIR/firmware.bin}"
[[ -f "$IMG" ]] || die "No image at $IMG. Run 'pio run' first, or pass a .bin path."

if [[ "$RESTORE" == yes ]]; then ADDR=$BOOT_BASE; else ADDR=$APP_BASE; fi
SIZE=$(wc -c < "$IMG" | tr -d ' ')

# ---- 1. the storage boundary --------------------------------------------------
# Programming erases every page the image touches, so an image that reaches
# 0x0803D800 would destroy calibration. Applies even with every override.
IMG_END=$(( ADDR + SIZE ))
if (( ADDR >= PROTECT_BASE || IMG_END > PROTECT_BASE )); then
  die "$(printf 'Image spans 0x%08X-0x%08X, which reaches protected storage at 0x%08X. Refusing.' \
        "$ADDR" "$IMG_END" "$PROTECT_BASE")"
fi

# ---- 2 + 3. the image must be linked for where it is going ---------------------
# Word 0 is the initial SP (must be in SRAM), word 1 the reset vector (Thumb,
# inside the image as loaded at ADDR).
read -r SP RST < <(python3 - "$IMG" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read(8)
print(*struct.unpack("<II", d)) if len(d) == 8 else print(0, 0)
PY
)
if (( SP <= 0x20000000 || SP > 0x2000C000 )); then
  die "$(printf 'Vector table: initial SP 0x%08X is not in SRAM - not a firmware image?' "$SP")"
fi
if (( (RST & 1) == 0 || (RST & ~1) < ADDR || (RST & ~1) >= IMG_END )); then
  die "$(printf 'Vector table: reset vector 0x%08X is outside 0x%08X-0x%08X. The image was linked for a different address. Refusing.' \
        "$RST" "$ADDR" "$IMG_END")"
fi

# ---- 4. the backup guard ---------------------------------------------------------
if ! backup_is_verified; then
  if [[ "$OVERRIDE" != yes ]]; then
    cat >&2 <<MSG

  REFUSING TO FLASH: no verified backup of the stock firmware exists.

  Writing now would permanently destroy OWON's firmware and any calibration
  constants held in MCU flash. That cannot be undone.

  Do this first:   ./scripts/backup.sh

  If the backup failed because readout protection is enabled, read
  docs/readout-protection.md, record your reference measurements, and then
  deliberately override:

      ./scripts/flash.sh "$IMG" --i-accept-no-backup

MSG
    exit 4
  fi
  warn "Overriding the backup guard. The stock firmware will be unrecoverable."
  read -r -p "Type the word ERASE to continue: " confirm
  [[ "$confirm" == "ERASE" ]] || die "Aborted."
else
  log "Verified backup present: $(latest_backup)"
fi

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected."
ADDR_HEX=$(printf '0x%08X' "$ADDR")
log "Flashing $IMG ($SIZE bytes) to $ADDR_HEX via $PROBE"

case "$PROBE" in
  jlink)
    jlink_run <<JL
halt
loadbin $IMG, $ADDR_HEX
verifybin $IMG, $ADDR_HEX
r
go
JL
    ;;
  *)
    oocd="$(find_openocd)" || die "OpenOCD not found."
    iface="$(oocd_interface "$PROBE")"
    # shellcheck disable=SC2086
    "$oocd" -f $iface -f "$OOCD_CFG" \
      -c "program $IMG verify reset exit $ADDR_HEX"
    ;;
esac
log "Flash complete and verified."
