/* Flash read-out as text, for the serial console (DIAG:BOOT?).
 *
 * Readout protection stops a debugger reading flash, not code running on the
 * chip (docs/readout-protection.md). This is how we get OWON's bootloader,
 * which holds the panel init sequence and decides what an update erases.
 *
 * Format, one line per 32 bytes, then a trailer:
 *     08000000 0123456789abcdef...      (address, 64 lowercase hex digits)
 *     END 08000000 00002800 1a2b3c4d    (base, length, CRC-32/IEEE of the data)
 * Lines end in "\r\n". scripts/read-boot.py parses and checks it.
 *
 * Portable: takes a pointer, so it is host-tested against a buffer.
 */
#ifndef FLASHDUMP_H
#define FLASHDUMP_H

#include <stddef.h>
#include <stdint.h>

#define FLASHDUMP_LINE 32u

typedef void (*flashdump_out_t)(void *ctx, const char *s, size_t n);

/* CRC-32/IEEE (zlib's crc32), continuing from `crc` (start with 0). */
uint32_t flashdump_crc32(uint32_t crc, const uint8_t *p, size_t n);

/* Emit `len` bytes at `mem`, labelled with `addr`. */
void flashdump_emit(const uint8_t *mem, uint32_t addr, uint32_t len,
                    flashdump_out_t out, void *ctx);

#endif
