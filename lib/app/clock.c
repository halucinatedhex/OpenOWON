/* See clock.h. Days from the civil date after Howard Hinnant's algorithm. */
#include "clock.h"

#include <stdio.h>

static int32_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    int32_t yoe = y - era * 400;
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int days_in_month(int y, int m)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dm[m - 1];
}

uint32_t clock_from_civil(const clock_civil_t *c)
{
    if (c->year < 2000 || c->year > 2105 || c->month < 1 || c->month > 12 || c->day < 1 ||
        c->day > days_in_month(c->year, c->month) || c->hour < 0 || c->hour > 23 ||
        c->min < 0 || c->min > 59 || c->sec < 0 || c->sec > 59)
        return 0;
    int32_t days = days_from_civil(c->year, c->month, c->day);
    return (uint32_t)days * 86400u + (uint32_t)(c->hour * 3600 + c->min * 60 + c->sec);
}

void clock_to_civil(uint32_t t, clock_civil_t *c)
{
    int32_t z = (int32_t)(t / 86400u) + 719468;
    uint32_t s = t % 86400u;
    int32_t era = z / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = (int)(yoe + era * 400 + (c->month <= 2));
    c->hour = (int)(s / 3600u);
    c->min = (int)(s / 60u % 60u);
    c->sec = (int)(s % 60u);
}

void clock_fmt(char *out, size_t cap, uint32_t t, char sep)
{
    clock_civil_t c;
    clock_to_civil(t, &c);
    snprintf(out, cap, "%04d-%02d-%02d%c%02d:%02d:%02d", c.year, c.month, c.day, sep, c.hour, c.min, c.sec);
}
