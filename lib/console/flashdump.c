#include "flashdump.h"

static const char HEX[] = "0123456789abcdef";

uint32_t flashdump_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static size_t put_hex32(char *d, uint32_t v)
{
    for (int i = 7; i >= 0; i--) { d[i] = HEX[v & 0xFu]; v >>= 4; }
    return 8;
}

void flashdump_emit(const uint8_t *mem, uint32_t addr, uint32_t len,
                    flashdump_out_t out, void *ctx)
{
    char line[8 + 1 + 2 * FLASHDUMP_LINE + 2];
    uint32_t crc = 0;

    for (uint32_t off = 0; off < len; off += FLASHDUMP_LINE) {
        uint32_t n = len - off < FLASHDUMP_LINE ? len - off : FLASHDUMP_LINE;
        size_t k = put_hex32(line, addr + off);
        line[k++] = ' ';
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = mem[off + i];
            line[k++] = HEX[b >> 4];
            line[k++] = HEX[b & 0xFu];
        }
        line[k++] = '\r';
        line[k++] = '\n';
        out(ctx, line, k);
        crc = flashdump_crc32(crc, mem + off, n);
    }

    char end[4 + 3 * 9 + 2];
    size_t k = 0;
    end[k++] = 'E'; end[k++] = 'N'; end[k++] = 'D'; end[k++] = ' ';
    k += put_hex32(end + k, addr);  end[k++] = ' ';
    k += put_hex32(end + k, len);   end[k++] = ' ';
    k += put_hex32(end + k, crc);
    end[k++] = '\r';
    end[k++] = '\n';
    out(ctx, end, k);
}
