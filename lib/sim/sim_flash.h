/* HOST ONLY. A NOR-flash model of our storage: the data log
 * (0x08030000-0x0803B7FF, lib/datalog) and settings (0x0803B800-0x0803D7FF).
 *
 * Behaves like the GD32 FMC: erase sets a 2 KB page to 0xFF; programming a
 * word that is not erased fails (the FMC reports PGERR); reads outside the
 * region return 0xFF. Any erase or program outside the region is refused and
 * counted, so tests can assert nothing ever strayed near the stock storage.
 *
 * Power cuts: sim_flash_cut_after(n) lets n more erase/program operations
 * through, then drops the rest. An erase that is cut leaves the page half
 * erased, which is worse than a real part and so a stricter test.
 */
#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include <stdint.h>

#include "store.h"

#define SIM_FLASH_BASE 0x08030000u
#define SIM_FLASH_END  0x0803D800u

extern const store_io_t sim_flash_io;

void     sim_flash_reset(void);                 /* all erased */
void     sim_flash_fill(uint8_t v);             /* simulate leftover data */
void     sim_flash_cut_after(int n);            /* -1: never */
uint32_t sim_flash_violations(void);            /* ops outside the region */
uint32_t sim_flash_erases(uint32_t page);
/* Change a byte behind the FMC's back: bit rot, for corruption tests. */
void     sim_flash_poke(uint32_t addr, uint8_t v);

#endif
