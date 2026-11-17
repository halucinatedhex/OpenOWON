# Connect-under-reset by hand, for boards whose firmware switches SWD off
# (stock V4.3.0 does) and probes whose RST pin does not work (ST-Link clones).
# Driven by scripts/connect-reset.sh; source after gd32f303cc.cfg.
#
#   set XDM_MODE info|backup     (backup also needs XDM_OUT)
#
# Sequence: the user holds NRST low; we connect (the debug port works in
# reset), arm reset-vector catch, ask for release, and the core halts on its
# first instruction, before firmware can touch PA13/PA14.
#
# Writes only the core's debug registers DEMCR and DHCSR. Never flash.

proc xdm_stamp {msg} { echo "[clock format [clock seconds] -format %H:%M:%S] $msg" }

init
xdm_stamp "Connected while the chip is held in reset."

# DEMCR: TRCENA | VC_CORERESET -> halt on the first instruction after reset.
mww 0xE000EDFC 0x01000001
# DHCSR: DBGKEY | C_HALT | C_DEBUGEN
mww 0xE000EDF0 0xA05F0003

echo ""
echo ">>> RELEASE RESET NOW: take the RST lead off ground (keep holding power) <<<"
echo ""

set halted 0
for {set i 0} {$i < 1800} {incr i} {
    catch { poll }
    if {[gd32f303cc.cpu curstate] eq "halted"} { set halted 1; break }
    sleep 100
}
if {!$halted} {
    xdm_stamp "Timed out after 3 minutes: the core never halted."
    shutdown error
}
xdm_stamp "Halted at the reset vector. Stock firmware has not run."

set prot [xdm_identify]

if {$XDM_MODE ne "backup"} { shutdown }
if {$prot eq "on"} {
    echo "Not dumping: readout protection blocks every flash read."
    shutdown error
}

xdm_stamp "Pass 1 of 2: reading 256 KB"
if {[catch {dump_image $XDM_OUT/flash.bin 0x08000000 0x40000} e]} {
    xdm_stamp "Pass 1 failed: $e"; shutdown error
}
xdm_stamp "Pass 2 of 2: re-reading to verify"
if {[catch {dump_image $XDM_OUT/flash.verify.bin 0x08000000 0x40000} e]} {
    xdm_stamp "Pass 2 failed: $e"; shutdown error
}
xdm_stamp "Both passes read."
shutdown
