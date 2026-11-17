#!/usr/bin/env bash
# Query the decompiled output. Usage: ghidra/find.sh <what>
#   display   candidate display/blit routines
#   afe       HY3131 register writers and their callers
#   bus <hex> functions touching a peripheral or RAM address
#   fn <name> print one decompiled function
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
C=ghidra/out/xdm_decomp.c
I=ghidra/out/xdm_index.txt
[[ -s $C ]] || { echo "no decompilation yet - run ghidra/run.sh" >&2; exit 1; }

case "${1:-}" in
  display)
    # A panel driver writes many 16-bit values in a loop and takes x/y/w/h.
    awk '/^\/\* ===== /{name=$0; body=""} {body=body"\n"$0}
         /^\}/{ if (body ~ /0x2[abc]|window|Window|pixel|Pixel|lcd|Lcd|LCD|tft|Tft|TFT/) print name; }' "$C" | sort -u
    echo "--- functions with tight 16-bit output loops ---"
    grep -B40 "<< 8" "$C" | grep "^/\* =====" | sort -u | head -20
    ;;
  afe)
    grep -n "stock_hy_write_reg\|stock_hy_read_regs\|stock_hy_write_regs" "$C" | head -40
    ;;
  bus)
    grep -n "${2:?need an address}" "$C" | head -30
    ;;
  fn)
    awk -v n="${2:?need a name}" '
      $0 ~ "^/\\* ===== "n" " {p=1}
      p {print}
      p && /^\}$/ {exit}' "$C"
    ;;
  *) sed -n '2,20p' "$0" ;;
esac
