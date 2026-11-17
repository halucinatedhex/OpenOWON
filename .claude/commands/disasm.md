---
description: Disassemble / analyse the stock firmware image (offline, no hardware)
---
Analyse the stock firmware in `firmware/`. Read `docs/firmware-analysis.md` first —
it records what is already established, so don't re-derive it.

Key facts to apply:
- Load base is **0x08002800**, not 0x08000000. A 10 KB bootloader sits below and
  is NOT in the file. Both scripts already default to this base.
- The image targets the STM32F103 (v2.x hardware); our board is GD32F303 (v3.x).
  Logic, strings and SCPI grammar transfer; register-level setup may not.

Tools:
- `python3 scripts/fw-triage.py <image>` — vector table, strings, tables
- `./scripts/fw-disasm.sh <image> [addr] [len]` — objdump disassembly

For $ARGUMENTS, disassemble the relevant region and explain what it does in terms
of the hardware: which peripheral, which register, what the control flow means.
Resolve literal-pool loads (`ldr rN, [pc, #x]`) to their target values, and if a
target lands in the 0x0801B000+ region, show the string it points at.

Append anything newly established to `docs/firmware-analysis.md`. Flag inferences
as inferences — do not state a guess as a finding.
