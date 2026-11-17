#!/usr/bin/env bash
# Connect over SWD while you hold the chip in reset by hand, then report
# identity and readout protection; with --backup, also take a two-pass backup.
#
#   ./scripts/connect-reset.sh            # identity + protection status
#   ./scripts/connect-reset.sh --backup   # ...and backup if not protected
#
# For when ./scripts/probe-info.sh gets no answer: stock V4.3.0 switches SWD
# off once running, and ST-Link clones' RST pin does not drive NRST.
# Read-only: writes only the core's debug registers, never flash.
# Needs you at the meter; see docs/bench.md, "Connecting under reset".
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MODE=info
case "${1:-}" in
  --backup) MODE=backup ;;
  "")       ;;
  *)        die "usage: $0 [--backup]" ;;
esac
[[ -t 0 ]] || die "Interactive: you have to hold and release reset. Run it from a terminal (or as: ! $0)."

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected. Plug one in, or set PROBE=stlink|cmsis-dap|jlink"
oocd="$(find_openocd)" || die "OpenOCD not found. brew install open-ocd"
iface="$(oocd_interface "$PROBE")"

cat <<'TXT'

  1. Meter OFF. SWDIO, SWCLK and GND connected (docs/bench.md).
  2. Connect CO5's RST hole to GND: with an ST-Link clone, plug the RST lead
     onto the stick's spare GND pin (its own RST pin does nothing).
  3. Switch the meter on and KEEP HOLDING the power button.
     The screen stays blank: the chip is in reset.

TXT
read -r -p "  Press Enter when the meter is on with the screen blank... " _

OUT=""
if [[ "$MODE" == backup ]]; then
  OUT="$BACKUP_DIR/$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$OUT"
fi

set +e
# shellcheck disable=SC2086
"$oocd" -f $iface -f "$OOCD_CFG" -c "adapter speed $SWD_SPEED" \
  -c "set XDM_MODE $MODE" -c "set XDM_OUT {$OUT}" \
  -f "$ROOT/openocd/connect-reset.tcl"
rc=$?
set -e

echo
log "The core is left halted. Switch the meter off and on to run stock again."

if [[ $rc -ne 0 ]]; then
  [[ -n "$OUT" ]] && rm -rf "$OUT"
  die "Did not complete (see above). If it never connected: check that RST really is grounded (screen blank), and try SWDIO and SWCLK the other way round: clone labels can be swapped."
fi

if [[ "$MODE" == backup ]]; then
  finalize_backup "$OUT" "$PROBE (connect under manual reset)"
fi
