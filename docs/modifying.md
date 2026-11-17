# Changing the firmware

> **Not the chosen route.** The project is doing a full replacement
> (`replacement-plan.md`). The code-cave tooling described here lives in
> `attic/extension/` and still builds byte-identically.

## Can we regenerate the stock binary byte-for-byte?

**No, and it is not worth attempting.**

The image was built with **Keil MDK (armcc)**, almost certainly microlib. The
proof is the startup sequence, which is verbatim Keil's `startup_stm32f10x.s`
plus ARM's `__main`:

```
Reset   0x08002900 : ldr r0,=SystemInit ; blx r0 ; ldr r0,=__main ; bx r0
__main  0x080028EC : ldr sp,=0x20005260 ; bl scatterload ; bx __rt_entry
```

GCC's CMSIS startup looks nothing like this (it inlines the .data copy and
calls `main` directly). The binary is also fully stripped — no `__main`,
`scatterload`, `GCC` or `ARM` strings survive.

Byte-matching would need the exact armcc version and flags, then function-level
matching across 116 KB of optimised Thumb-2. Comparable decompilation projects
(SM64, Pokémon) take dozens of contributors several years, *with* freely
available compilers. armcc is proprietary and version-pinned. The cost is not
justified when the goal is modification rather than provenance.

## Three paths that do work

| Path | Produces | Needs your own dump? |
| --- | --- | --- |
| 1. Clean reimplementation | Wholly new firmware from the recovered spec | No |
| 2. Code-cave extension | Stock image + your code in free flash | **Yes** |
| 3. Ship of Theseus | Starts as stock, functions replaced one at a time | **Yes** |

**Paths 2 and 3 need a dump of *your* v3.x GD32 board.** The v2.1.2 image here
is STM32F103 and will not run on it. If readout protection blocks the backup,
only path 1 remains — see `readout-protection.md`.

## The Ship-of-Theseus build (paths 2 and 3)

Already scaffolded and working end to end. It exploits the free flash above the stock application. Note the usable
window is ~120 KB, not the 129.7 KB quoted earlier: the top 10 KB holds
settings and calibration (see `settings-storage.md`) and must never be
erased.

```
0x08002800  stock application        (116.3 KB, untouched)
0x08020000  your extension code      (linked here)
0x08040000  end of flash
0x2000A000  your RAM                 (8 KB, clear of stock's 0x20005260 stack)
```

### Pieces

| File | Role |
| --- | --- |
| `symbols/*.sym` | recovered stock addresses, `<addr> <name>` per line |
| `attic/extension/gen-symbols.py` | turns those into `build/stock_symbols.ld` |
| `attic/extension/extension.ld` | places your code at 0x08020000, RAM at 0x2000A000 |
| `attic/extension/ext_main.c` | your code; calls stock functions by name |
| `attic/extension/patches.txt` | which stock call sites to divert |
| `attic/extension/mkpatch.py` | splices the extension in and applies hooks |
| `attic/extension/build-ext.sh` | runs the whole chain |

### Build

```sh
PATCHES=attic/extension/patches.txt ./attic/extension/build-ext.sh backups/latest/flash.bin build/patched.bin
./scripts/flash.sh build/patched.bin
```

### How a hook works

Declare the stock function, then call it:

```c
extern void stock_scpi_enqueue(const char *cmd, uint8_t source);

void ext_scpi_enqueue_hook(const char *cmd, uint8_t source)
{
    if (streq(cmd, "EXT:PING")) { ext_custom_hits++; return; }
    stock_scpi_enqueue(cmd, source);   /* everything else unchanged */
}
```

and divert one call site in `attic/extension/patches.txt`:

```
b 0x08004834 0x0802003C
```

`mkpatch.py` encodes the Thumb-2 branch (verified against objdump) and writes
it in. Stock behaviour is preserved except where you intervene.

Directives: `vector <idx> <addr>`, `bl <at> <target>`, `b <at> <target>`,
`bytes <at> <hex>`.

### Verified working

Building against the v2.1.2 image produces a 120,956-byte image ending at
0x0802007C, with `0x08004834` reading `b.w 0x802003c` and the extension present
and correctly disassembling. The mechanism is sound; only the *addresses* need
redoing against a v3.x dump.

## Migrating to your own dump

1. `./scripts/backup.sh` — get `backups/latest/flash.bin`.
2. Re-run the analysis against it: `python3 scripts/fw-triage.py backups/latest/flash.bin`.
   Confirm the base (it may not be 0x08002800 on v3.x) and the vector table.
3. Diff the string tables against v2.1.2 to see what moved.
4. Write a new `symbols/xdm1241_v3.sym` with the re-derived addresses.
5. Rebuild. Nothing else in the chain changes.

**Do not reuse the v2.1.2 addresses on a v3.x dump.** They will be wrong, and a
branch into the middle of an unrelated function is how you brick a meter.
