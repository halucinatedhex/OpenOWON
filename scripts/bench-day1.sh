#!/usr/bin/env bash
# Bench day 1, everything read-only, in order. Stops at the first failure.
#
#   bench-day1.sh [/dev/cu.usbserial-XXXX]
#
#   1. probe-info      chip identity, readout protection, backup status
#   2. backup          full flash, two-pass verified
#   3. backup-calib    the storage region on its own
#   4. serial-dump     factory record over SCPI (only with a serial port given)
#   5. port-symbols    v2.1.2 symbols onto this meter's dump
#   6. bootloader      carve 0x08000000-0x080027FF out for analysis
#   7. regions         are our data-log and settings pages free on this board?
#
# Nothing here writes the chip. Afterwards commit backups/ and push it, so the
# dumps exist off this machine (the repo is private; they hold OWON's code).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
SERIAL="${1:-}"

step() { echo; log "[$1] $2"; }

step 1 "probe and target";   "$ROOT/scripts/probe-info.sh"
step 2 "full backup";        "$ROOT/scripts/backup.sh"
step 3 "storage backup";     "$ROOT/scripts/backup-calib.sh"
DUMP="$(latest_backup)"
[[ -n "$DUMP" ]] || die "no backup found after step 2"
DIR="$(dirname "$DUMP")"

if [[ -n "$SERIAL" ]]; then
  step 4 "factory record over serial"
  python3 "$ROOT/scripts/serial-dump.py" "$SERIAL" || warn "serial dump reported problems (see output)"
else
  step 4 "skipped: no serial port given"
fi

step 5 "port symbols to this dump"
python3 "$ROOT/scripts/port-symbols.py" "$DUMP" -o "$ROOT/symbols/xdm1241_v3.sym"

step 6 "carve out the bootloader"
head -c $((0x2800)) "$DUMP" > "$DIR/bootloader.bin"
tail -c +$((0x2800 + 1)) "$DUMP" | head -c $((0x3D800 - 0x2800)) > "$DIR/app.bin"
log "bootloader: $DIR/bootloader.bin   app: $DIR/app.bin"

step 7 "our flash regions"
python3 "$ROOT/scripts/check-regions.py" "$DUMP" || warn "a region we write is in use: leave its flag off (above)"

echo
log "Done. Next: git add backups symbols/xdm1241_v3.sym && commit, then push."
log "Then ! ./scripts/hil-smoke.sh for the first flash (docs/bench.md)."
