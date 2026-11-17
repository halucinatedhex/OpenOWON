#!/usr/bin/env bash
# Start a GDB server for interactive debugging. Read-only until you flash from GDB.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected."

ELF="$BUILD_DIR/firmware.elf"
[[ -f "$ELF" ]] || warn "No ELF at $ELF — symbols unavailable. Run 'pio run' first."

log "GDB server on localhost:$GDB_PORT (probe: $PROBE). Ctrl-C to stop."
log "Connect with:  arm-none-eabi-gdb $ELF -ex 'target extended-remote :$GDB_PORT'"
log "Or simply:     pio debug"

case "$PROBE" in
  jlink)
    have JLinkGDBServer || die "JLinkGDBServer not found. brew install --cask segger-jlink"
    exec JLinkGDBServer -device "$JLINK_DEVICE" -if SWD -speed "$SWD_SPEED" -port "$GDB_PORT" -nogui
    ;;
  *)
    oocd="$(find_openocd)" || die "OpenOCD not found."
    iface="$(oocd_interface "$PROBE")"
    # shellcheck disable=SC2086
    exec "$oocd" -f $iface -f "$OOCD_CFG" -c "gdb_port $GDB_PORT"
    ;;
esac
