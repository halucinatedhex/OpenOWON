#!/usr/bin/env bash
# Identify the probe and the target. Read-only: safe to run any time.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PROBE="$(detect_probe)"
echo "probe detected : $PROBE"
echo "openocd        : $(find_openocd 2>/dev/null || echo 'not installed')"
echo "JLinkExe       : $(command -v JLinkExe 2>/dev/null || echo 'not installed')"
echo "target device  : $JLINK_DEVICE  ($FLASH_SIZE bytes @ $FLASH_BASE)"
echo "backup status  : $(backup_is_verified && echo "VERIFIED -> $(latest_backup)" || echo 'NONE — flashing is blocked')"
echo

[[ "$PROBE" == none ]] && { warn "No probe on USB. Connect one to read chip identity."; exit 0; }

log "Reading chip identity (no writes)"
case "$PROBE" in
  jlink)
    # DBGMCU_IDCODE at 0xE0042000; flash size (KB) at 0x1FFFF7E0 on F1-compatible parts
    jlink_run <<'JL'
mem32 0xE0042000, 1
mem16 0x1FFFF7E0, 1
mem32 0x1FFFF7E8, 3
JL
    ;;
  *)
    oocd="$(find_openocd)" || die "OpenOCD not found."
    iface="$(oocd_interface "$PROBE")"
    # One attempt only. Stock V4.3.0 ignores SWD while running and reacts to
    # traffic on PA13/PA14 (relays chatter), so never retry in a loop.
    # shellcheck disable=SC2086
    if ! "$oocd" -f $iface -f "$OOCD_CFG" -c "adapter speed $SWD_SPEED" \
        -c "init; halt; xdm_identify; exit"; then
      warn "No answer from the chip. If the wiring is right (docs/bench.md),"
      warn "stock firmware has switched SWD off: use ./scripts/connect-reset.sh,"
      warn "which connects while you hold the chip in reset."
      exit 1
    fi
    ;;
esac
echo
echo "Reference: DBGMCU_IDCODE low 12 bits = device ID; 0x414 = F1 high-density class (GD32F303 reports compatible)."
echo "FlashSize low half-word should read 256 (0x100) for a GD32F303CC."
echo "PROTECTION ON means no SWD backup is possible: see docs/readout-protection.md."
