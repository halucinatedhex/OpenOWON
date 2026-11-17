#!/usr/bin/env bash
# Capture the firmware's RTT console (lib/hal/rtt.c) for N seconds. Read-only:
# attaches to the running core without halting or resetting it.
#   rtt-capture.sh [seconds=6] [out=build/rtt.log]
# J-Link: SEGGER's JLinkRTTLogger. Other probes (ST-Link, CMSIS-DAP): OpenOCD
# finds the "SEGGER RTT" control block in RAM and serves channel 0 on a local
# TCP port, which this reads.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
SECS="${1:-6}"
OUT="${2:-$ROOT/build/rtt.log}"
PROBE="$(detect_probe)"
[[ "$PROBE" == none ]] && die "No debug probe detected (set PROBE=jlink|stlink|cmsis-dap)."
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
log "RTT channel 0 for ${SECS}s -> $OUT (probe: $PROBE)"

if [[ "$PROBE" == jlink ]]; then
  have JLinkRTTLogger || die "JLinkRTTLogger not found. Install: brew install --cask segger-jlink"
  JLinkRTTLogger -Device "$JLINK_DEVICE" -If SWD -Speed "$SWD_SPEED" -RTTChannel 0 "$OUT" >/dev/null 2>&1 &
  pid=$!
  sleep "$SECS"
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
else
  oocd="$(find_openocd)" || die "OpenOCD not found. brew install open-ocd"
  iface="$(oocd_interface "$PROBE")"
  RTT_PORT="${RTT_PORT:-19021}"
  # shellcheck disable=SC2086
  "$oocd" -f $iface -f "$OOCD_CFG" -c "adapter speed $SWD_SPEED" -c "init" \
      -c "rtt setup 0x20000000 0xC000 {SEGGER RTT}" -c "rtt start" \
      -c "rtt server start $RTT_PORT 0" >"$OUT.openocd" 2>&1 &
  pid=$!
  python3 - "$RTT_PORT" "$SECS" "$OUT" <<'PY'
import socket, sys, time
port, secs, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
deadline = time.time() + secs + 3
s = None
while time.time() < deadline and s is None:            # wait for OpenOCD's server
    try: s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
    except OSError: time.sleep(0.2)
if s is None: sys.exit("could not reach OpenOCD's RTT server")
s.settimeout(0.2)
end = time.time() + secs; data = b""
while time.time() < end:
    try:
        d = s.recv(4096)
        if not d: break
        data += d
    except socket.timeout: pass
open(out, "wb").write(data)
PY
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
  [[ -s "$OUT" ]] || { tail -5 "$OUT.openocd" >&2; }
fi
[[ -s "$OUT" ]] || die "nothing captured: is the firmware running, and RTT enabled?"
log "$(wc -l < "$OUT" | tr -d ' ') lines captured"
