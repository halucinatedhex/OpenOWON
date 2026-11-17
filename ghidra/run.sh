#!/usr/bin/env bash
# Headless Ghidra analysis of the stock firmware.
#   ghidra/run.sh [image.bin]
# Produces ghidra/out/xdm_decomp.c and ghidra/out/xdm_index.txt
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
# Ghidra 12 needs JDK 21+. Homebrew installs openjdk@25 as a ghidra dependency;
# the system JDK here is 17, which analyzeHeadless rejects and then tries to
# prompt for - which fails with no TTY.
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:$PATH"
HEADLESS=/opt/homebrew/Cellar/ghidra/12.2/libexec/support/analyzeHeadless
IMG="${1:-firmware/XDM1041_OS_V2.1.2_20210525.bin}"
PROJ="$PWD/ghidra/proj"
OUT="$PWD/ghidra/out"
mkdir -p "$PROJ" "$OUT"
rm -rf "$PROJ"/xdm.rep "$PROJ"/xdm.gpr 2>/dev/null || true

"$HEADLESS" "$PROJ" xdm \
  -import "$IMG" \
  -processor ARM:LE:32:Cortex \
  -loader BinaryLoader -loader-baseAddr 0x08002800 \
  -scriptPath "$PWD/ghidra/scripts" \
  -preScript  XdmSetup.java  "$PWD/symbols/xdm1041_v2.1.2.sym" \
  -postScript XdmDeepen.java \
  -postScript XdmExport.java "$OUT/xdm_decomp.c" "$OUT/xdm_index.txt" \
  -deleteProject
