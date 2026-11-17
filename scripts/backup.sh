#!/usr/bin/env bash
# Dump the stock firmware twice, compare, and archive it as a verified backup.
# Read-only with respect to the target: never writes or erases.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected. Plug one in, or set PROBE=jlink|stlink|cmsis-dap|olimex"
log "Probe: $PROBE"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$BACKUP_DIR/$STAMP"
mkdir -p "$OUT"

dump_to() { # $1 = destination file
  case "$PROBE" in
    jlink)
      jlink_run <<JL
savebin $1, $FLASH_BASE, $FLASH_SIZE
JL
      ;;
    *)
      local oocd iface
      oocd="$(find_openocd)" || die "OpenOCD not found. brew install open-ocd"
      iface="$(oocd_interface "$PROBE")"
      # shellcheck disable=SC2086
      "$oocd" -f $iface -f "$OOCD_CFG" \
        -c "init; halt; dump_image $1 $FLASH_BASE $FLASH_SIZE; exit"
      ;;
  esac
}

log "Pass 1 of 2: reading ${FLASH_SIZE} bytes from $FLASH_BASE"
dump_to "$OUT/flash.bin"       || die "First read failed"
log "Pass 2 of 2: re-reading to verify the dump is stable"
dump_to "$OUT/flash.verify.bin" || die "Second read failed"

finalize_backup "$OUT" "$PROBE (hot attach)"
