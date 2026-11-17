#!/usr/bin/env bash
# Disassemble a raw Cortex-M image with the PlatformIO ARM toolchain.
# Usage: fw-disasm.sh <image.bin> [start_addr] [length]
#        fw-disasm.sh fw.bin                 -> whole image to stdout
#        fw-disasm.sh fw.bin 0x080047D6 0x80 -> one function
set -euo pipefail

OBJDUMP="$(ls ~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-objdump 2>/dev/null | head -1)"
[[ -x "$OBJDUMP" ]] || { echo "arm-none-eabi-objdump not found (run 'pio run' once to fetch the toolchain)" >&2; exit 1; }

IMG="${1:?usage: fw-disasm.sh <image.bin> [start] [len]}"
BASE=0x08002800
START="${2:-}"
LEN="${3:-}"

ARGS=(-D -b binary -m arm_any -M force-thumb --adjust-vma="$BASE")
if [[ -n "$START" ]]; then
  off=$(( START - BASE ))
  ARGS+=(--start-address="$START")
  [[ -n "$LEN" ]] && ARGS+=(--stop-address=$(( START + LEN )))
fi
"$OBJDUMP" "${ARGS[@]}" "$IMG"
