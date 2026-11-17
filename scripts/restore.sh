#!/usr/bin/env bash
# Put the stock firmware back, from the newest verified backup. WRITES THE CHIP.
#
#   restore.sh [backups/<stamp>/flash.bin]
#
# Writes only 0x08000000-0x0803D7FF: the bootloader and the stock application.
# The storage at 0x0803D800 and above (data log, preferences, factory
# calibration) is never erased by anything in this project, so it still holds
# the original records and is left alone.
#
# Our own settings pages (0x0803B800-0x0803D7FF) are overwritten with whatever
# the backup holds there, which is what stock expects.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SRC="${1:-$(latest_backup)}"
[[ -n "$SRC" && -f "$SRC" ]] || die "No backup to restore from. Run ./scripts/backup.sh first."
[[ -f "$(dirname "$SRC")/VERIFIED" ]] || die "$SRC is not a verified backup."

FULL=$(wc -c < "$SRC" | tr -d ' ')
(( FULL == 0x40000 )) || die "$SRC is $FULL bytes; expected a full 256 KB dump."

KEEP=$(( 0x0803D800 - 0x08000000 ))
OUT="$ROOT/build/restore-$(basename "$(dirname "$SRC")").bin"
mkdir -p "$ROOT/build"
head -c "$KEEP" "$SRC" > "$OUT"
log "Restoring stock bootloader + application from $SRC ($KEEP bytes)"

exec "$ROOT/scripts/flash.sh" "$OUT" --restore
