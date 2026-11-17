#!/usr/bin/env bash
# PreToolUse/Bash guard: block chip-writing commands unless a verified backup exists.
#
# Reading the GD32 is safe and always allowed. Writing is irreversible and can
# destroy OWON's factory calibration, so it is gated on backups/*/VERIFIED.
# Exit 2 = block, and stderr is shown to Claude.

ROOT="${CLAUDE_PROJECT_DIR:-$PWD}"
# Read the command. jq if present, else python3; if neither can parse it,
# check the raw input instead, so a missing tool fails closed, not open.
INPUT="$(cat)"
CMD="$(printf '%s' "$INPUT" | jq -r '.tool_input.command // empty' 2>/dev/null)" \
  || CMD="$(printf '%s' "$INPUT" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("tool_input",{}).get("command",""))' 2>/dev/null)" \
  || CMD="$INPUT"
[[ -z "$CMD" ]] && exit 0

# Commands that write or erase the target's flash.
DESTRUCTIVE='(openocd.*(program |flash +write|flash +erase|stm32f1x +(mass_)?erase))'
DESTRUCTIVE+='|(JLink(Exe|GDBServer)?.*(loadbin|loadfile|erase))'
DESTRUCTIVE+='|(st-flash +(write|erase))'
DESTRUCTIVE+='|(pio|platformio).* -t +upload'
DESTRUCTIVE+='|(pio|platformio).* run .*--target +upload'
DESTRUCTIVE+='|(probe-rs +(download|erase))'
DESTRUCTIVE+='|(pyocd +(flash|erase))'
DESTRUCTIVE+='|(dfu-util.* -D )'

echo "$CMD" | grep -qiE "$DESTRUCTIVE" || exit 0

# scripts/flash.sh carries its own guard and interactive confirmation.
echo "$CMD" | grep -q 'scripts/flash\.sh' && exit 0

if compgen -G "$ROOT/backups/*/VERIFIED" > /dev/null 2>&1; then
  exit 0
fi

cat >&2 <<'MSG'
BLOCKED: this command writes or erases the GD32F303 flash, and no verified
backup of the stock firmware exists (backups/*/VERIFIED is absent).

Writing now would permanently destroy OWON's firmware and any calibration
constants held in MCU flash. That cannot be undone.

Run ./scripts/backup.sh first. It is read-only and takes two passes to confirm
the dump is good.

If the backup already failed because readout protection is enabled, do not work
around this hook. Read docs/readout-protection.md, then use the deliberate
override, which asks the user to confirm in person:

    ./scripts/flash.sh <image.bin> --i-accept-no-backup
MSG
exit 2
