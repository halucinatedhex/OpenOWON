#!/usr/bin/env bash
# Build the extension firmware and splice it into a stock dump.
#   attic/extension/build-ext.sh <stock.bin> [out.bin]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
TC="$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi"
STOCK="${1:?usage: build-ext.sh <stock.bin> [out.bin]}"
OUT="${2:-build/patched.bin}"
mkdir -p build

python3 attic/extension/gen-symbols.py
"$TC-gcc" -mcpu=cortex-m4 -mthumb -Os -ffreestanding -nostdlib -fno-builtin \
          -Wall -Wextra -c attic/extension/ext_main.c -o build/ext_main.o
"$TC-gcc" -mcpu=cortex-m4 -mthumb -nostdlib -Wl,-L,build \
          -Wl,-T,attic/extension/extension.ld -Wl,-Map,build/ext.map \
          build/ext_main.o -o build/ext.elf
"$TC-objcopy" -O binary build/ext.elf build/ext.bin
echo "extension: $(wc -c < build/ext.bin | tr -d ' ') bytes at 0x08020000"

python3 attic/extension/mkpatch.py --stock "$STOCK" --base 0x08002800 \
        --place 0x08020000=build/ext.bin \
        ${PATCHES:+--patch "$PATCHES"} -o "$OUT"
