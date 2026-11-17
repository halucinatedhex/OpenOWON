#!/usr/bin/env bash
# Back up ONLY the storage region (top 10 KB): data log, preferences, calibration.
#
# Read-only. This is the region that makes the meter accurate, and it is
# independent of the application, so it can be restored after flashing custom
# firmware. See docs/settings-storage.md.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CAL_BASE="${CAL_BASE:-0x0803D800}"
CAL_SIZE="${CAL_SIZE:-0x2800}"          # 10 KB, through 0x08040000

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected."

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$BACKUP_DIR/calib-$STAMP"
mkdir -p "$OUT"
log "Reading $CAL_SIZE bytes from $CAL_BASE via $PROBE (settings + calibration)"

read_to() {
  case "$PROBE" in
    jlink) jlink_run <<JL
savebin $1, $CAL_BASE, $CAL_SIZE
JL
      ;;
    *)
      local oocd iface
      oocd="$(find_openocd)" || die "OpenOCD not found."
      iface="$(oocd_interface "$PROBE")"
      # shellcheck disable=SC2086
      "$oocd" -f $iface -f "$OOCD_CFG" \
        -c "init; halt; dump_image $1 $CAL_BASE $CAL_SIZE; exit"
      ;;
  esac
}

read_to "$OUT/calib.bin"        || die "read failed"
read_to "$OUT/calib.verify.bin" || die "second read failed"
cmp -s "$OUT/calib.bin" "$OUT/calib.verify.bin" \
  || die "reads differ - not trusted. Try SWD_SPEED=500."
rm -f "$OUT/calib.verify.bin"

SHA="$(shasum -a 256 "$OUT/calib.bin" | awk '{print $1}')"
cat > "$OUT/manifest.txt" <<MAN
XDM1241 settings + calibration region
date    : $(date -u +%Y-%m-%dT%H:%M:%SZ)
origin  : $CAL_BASE
size    : $CAL_SIZE
sha256  : $SHA
layout  : 0x0803D800 data log (6004 B), 0x0803F000 preferences (168 B), 0x0803F800 factory + calibration (1392 B)
MAN
# A blank read means protection or a wrong address, not an empty region.
if ! LC_ALL=C grep -qv $'\xff' < "$OUT/calib.bin"; then
  warn "Region reads as all 0xFF - either protected, or already erased."
  exit 3
fi
touch "$OUT/VERIFIED"
log "Saved $OUT/calib.bin"
cat "$OUT/manifest.txt"
