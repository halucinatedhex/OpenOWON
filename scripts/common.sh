#!/usr/bin/env bash
# Shared config + helpers for XDM1241 firmware workflow.
# Source this; do not run it directly.

set -euo pipefail

# ---- target -----------------------------------------------------------------
JLINK_DEVICE="${JLINK_DEVICE:-GD32F303CC}"
FLASH_BASE="${FLASH_BASE:-0x08000000}"
FLASH_SIZE="${FLASH_SIZE:-0x40000}"   # 256 KB
SWD_SPEED="${SWD_SPEED:-1000}"        # kHz; 1 MHz is safe on jumper wires
GDB_PORT="${GDB_PORT:-2331}"

# ---- paths ------------------------------------------------------------------
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUP_DIR="$ROOT/backups"
BUILD_DIR="$ROOT/.pio/build/xdm1241"
OOCD_CFG="$ROOT/openocd/gd32f303cc.cfg"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mERR\033[0m %s\n' "$*" >&2; exit 1; }

# ---- probe detection --------------------------------------------------------
# PROBE may be forced: jlink | stlink | cmsis-dap | olimex
detect_probe() {
  if [[ -n "${PROBE:-}" ]]; then echo "$PROBE"; return; fi
  local usb
  # system_profiler misses some devices (an ST-Link clone on macOS 27 was
  # absent from it while ioreg listed it), so read both.
  usb="$(system_profiler SPUSBDataType 2>/dev/null || true)
$(ioreg -p IOUSB -w0 2>/dev/null || true)"
  if   grep -qi "J-Link"            <<<"$usb"; then echo jlink
  elif grep -qi "ST-LINK\|STLINK"   <<<"$usb"; then echo stlink
  elif grep -qi "Picoprobe\|CMSIS-DAP\|DAPLink" <<<"$usb"; then echo cmsis-dap
  elif grep -qi "Olimex"            <<<"$usb"; then echo olimex
  else echo none
  fi
}

have() { command -v "$1" >/dev/null 2>&1; }

# Locate OpenOCD: system install, else PlatformIO's bundled copy.
find_openocd() {
  if have openocd; then command -v openocd; return; fi
  local pio_oocd
  pio_oocd="$(ls -d "$HOME"/.platformio/packages/tool-openocd/bin/openocd 2>/dev/null | head -1 || true)"
  [[ -n "$pio_oocd" ]] && { echo "$pio_oocd"; return; }
  return 1
}

# OpenOCD interface config for the detected probe.
oocd_interface() {
  case "$1" in
    jlink)     echo "interface/jlink.cfg" ;;
    stlink)    echo "interface/stlink.cfg" ;;
    cmsis-dap) echo "interface/cmsis-dap.cfg" ;;
    olimex)    echo "interface/ftdi/olimex-arm-usb-tiny-h.cfg -f interface/ftdi/olimex-arm-jtag-swd.cfg" ;;
    *)         return 1 ;;
  esac
}

# Run a J-Link Commander script from stdin.
jlink_run() {
  have JLinkExe || die "JLinkExe not found. Install: brew install --cask segger-jlink"
  local tmp; tmp="$(mktemp -t xdm1241.XXXXXX.jlink)"
  { echo "device $JLINK_DEVICE"; echo "si SWD"; echo "speed $SWD_SPEED"; echo "connect"; cat; echo "qc"; } > "$tmp"
  log "J-Link: $JLINK_DEVICE @ ${SWD_SPEED}kHz SWD"
  JLinkExe -NoGui 1 -ExitOnError 1 -CommanderScript "$tmp"
  local rc=$?; rm -f "$tmp"; return $rc
}

# Newest verified backup, or empty string.
latest_backup() {
  ls -1t "$BACKUP_DIR"/*/flash.bin 2>/dev/null | head -1 || true
}

backup_is_verified() {
  local b; b="$(latest_backup)"
  [[ -n "$b" && -f "$(dirname "$b")/VERIFIED" ]]
}

# Check a two-pass dump in $1 (flash.bin + flash.verify.bin) and, if it is
# sound, write manifest.txt and VERIFIED. $2 describes how it was read.
# Exit 3 for a blank (protection-blocked) dump; die on a mismatch.
finalize_backup() {
  local out="$1" method="$2" stamp sha size blank=no
  stamp="$(basename "$out")"
  [[ -s "$out/flash.bin" ]] || die "Dump is empty"
  if ! cmp -s "$out/flash.bin" "$out/flash.verify.bin"; then
    die "The two reads differ. Backup NOT trusted. Check wiring/speed (try SWD_SPEED=500), then retry."
  fi
  rm -f "$out/flash.verify.bin"

  sha="$(shasum -a 256 "$out/flash.bin" | awk '{print $1}')"
  size="$(wc -c < "$out/flash.bin" | tr -d ' ')"

  # Sanity: a blank or protection-blocked read is all 0x00 or all 0xFF.
  # (Not `grep -v $'\x00'`: bash cannot pass a NUL, so that greps for ''
  # and called every dump all-zero.)
  if [[ "$(LC_ALL=C tr -d '\000' < "$out/flash.bin" | head -c 1 | wc -c | tr -d ' ')" == 0 ]]; then blank="all-zero"; fi
  if [[ "$(LC_ALL=C tr -d '\377' < "$out/flash.bin" | head -c 1 | wc -c | tr -d ' ')" == 0 ]]; then blank="all-ones"; fi

  cat > "$out/manifest.txt" <<MAN
XDM1241 / XDM1041_dig_v1.7 stock firmware backup
date        : $(date -u +%Y-%m-%dT%H:%M:%SZ)
probe       : $method
device      : $JLINK_DEVICE
origin      : $FLASH_BASE
size        : $size bytes
sha256      : $sha
two-pass    : identical (verified)
suspicious  : $blank
MAN

  if [[ "$blank" != no ]]; then
    warn "Dump is entirely $blank — readout protection is probably enabled."
    warn "This is NOT a usable backup. Not marking it verified."
    warn "See docs/readout-protection.md before going further."
    exit 3
  fi

  touch "$out/VERIFIED"
  ln -sfn "$stamp" "$BACKUP_DIR/latest"
  log "Backup verified: $out/flash.bin"
  log "sha256: $sha"
  cat "$out/manifest.txt"
}
