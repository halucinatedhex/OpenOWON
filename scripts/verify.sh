#!/usr/bin/env bash
# Compare what is on the chip against a .bin file. Read-only.
# Usage: verify.sh [image.bin]   (default: the current build, else the latest backup)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

IMG="${1:-}"
if [[ -z "$IMG" ]]; then
  if   [[ -f "$BUILD_DIR/firmware.bin" ]]; then IMG="$BUILD_DIR/firmware.bin"
  elif [[ -n "$(latest_backup)" ]];        then IMG="$(latest_backup)"
  else die "Nothing to verify against. Build first, or pass a .bin path."
  fi
fi
[[ -f "$IMG" ]] || die "No such file: $IMG"

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected."
log "Verifying chip against $IMG ($(wc -c < "$IMG" | tr -d ' ') bytes) via $PROBE"

case "$PROBE" in
  jlink)
    jlink_run <<JL
verifybin $IMG, $FLASH_BASE
JL
    ;;
  *)
    oocd="$(find_openocd)" || die "OpenOCD not found."
    iface="$(oocd_interface "$PROBE")"
    # shellcheck disable=SC2086
    "$oocd" -f $iface -f "$OOCD_CFG" \
      -c "init; halt; verify_image $IMG $FLASH_BASE bin; exit"
    ;;
esac
log "Verify passed: chip matches $IMG"
