/* HOST ONLY. See sim_flash.h. */
#include "sim_flash.h"

#include <string.h>

#define SIZE (SIM_FLASH_END - SIM_FLASH_BASE)
#define NPAGE (SIZE / STORE_PAGE_SIZE)

static uint8_t  mem[SIZE];
static int      budget = -1;
static uint32_t violations;
static uint32_t erases[NPAGE];

static int spend(void)
{
    if (budget < 0) return 1;
    if (budget == 0) return 0;
    budget--;
    return 1;
}

static void f_read(uint32_t addr, void *dst, size_t n)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        uint32_t a = addr + (uint32_t)i;
        d[i] = (a >= SIM_FLASH_BASE && a < SIM_FLASH_END) ? mem[a - SIM_FLASH_BASE] : 0xFF;
    }
}

static int f_erase(uint32_t addr)
{
    if (addr < SIM_FLASH_BASE || addr >= SIM_FLASH_END || (addr - SIM_FLASH_BASE) % STORE_PAGE_SIZE) {
        violations++;
        return -1;
    }
    uint8_t *p = mem + (addr - SIM_FLASH_BASE);
    if (!spend()) {                         /* cut mid-erase: half done */
        memset(p, 0xFF, STORE_PAGE_SIZE / 2);
        return -1;
    }
    memset(p, 0xFF, STORE_PAGE_SIZE);
    erases[(addr - SIM_FLASH_BASE) / STORE_PAGE_SIZE]++;
    return 0;
}

static int f_program(uint32_t addr, uint32_t v)
{
    if (addr < SIM_FLASH_BASE || addr + 4 > SIM_FLASH_END || (addr & 3u)) {
        violations++;
        return -1;
    }
    if (!spend()) return -1;
    uint8_t *p = mem + (addr - SIM_FLASH_BASE);
    uint32_t old;
    memcpy(&old, p, 4);
    if (old != 0xFFFFFFFFu) return -1;      /* FMC: PGERR on a non-erased word */
    memcpy(p, &v, 4);
    return 0;
}

const store_io_t sim_flash_io = { .read = f_read, .erase_page = f_erase, .program_word = f_program };

void sim_flash_reset(void)
{
    memset(mem, 0xFF, sizeof mem);
    budget = -1;
    violations = 0;
    memset(erases, 0, sizeof erases);
}

void sim_flash_fill(uint8_t v) { memset(mem, v, sizeof mem); }
void sim_flash_poke(uint32_t a, uint8_t v)
{
    if (a >= SIM_FLASH_BASE && a < SIM_FLASH_END) mem[a - SIM_FLASH_BASE] = v;
}
void sim_flash_cut_after(int n) { budget = n; }
uint32_t sim_flash_violations(void) { return violations; }
uint32_t sim_flash_erases(uint32_t page)
{
    uint32_t i = (page - SIM_FLASH_BASE) / STORE_PAGE_SIZE;
    return i < NPAGE ? erases[i] : 0;
}
