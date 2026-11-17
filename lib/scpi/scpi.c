/* See scpi.h. */
#include "scpi.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void scpi_init(scpi_t *s, const scpi_cmd_t *cmds, int ncmds, void *ctx,
               void (*write)(void *ctx, const char *s, size_t n))
{
    memset(s, 0, sizeof *s);
    s->cmds = cmds;
    s->ncmds = ncmds;
    s->ctx = ctx;
    s->write = write;
}

/* ---- output and errors ---------------------------------------------------- */

void scpi_reply(scpi_t *s, const char *text)
{
    if (!s->write) return;
    s->write(s->ctx, text, strlen(text));
    s->write(s->ctx, "\r\n", 2);            /* as stock (V4.3.0, observed) */
}

void scpi_replyf(scpi_t *s, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    scpi_reply(s, b);
}

void scpi_error(scpi_t *s, int code)
{
    if (s->nerr < SCPI_ERRQ) s->errq[s->nerr++] = code;
    else s->errq[SCPI_ERRQ - 1] = -350;           /* queue overflow */
}

int scpi_error_pop(scpi_t *s)
{
    if (!s->nerr) return 0;
    int e = s->errq[0];
    memmove(s->errq, s->errq + 1, (size_t)(s->nerr - 1) * sizeof s->errq[0]);
    s->nerr--;
    return e;
}

const char *scpi_error_text(int code)
{
    switch (code) {
    case 0:                    return "No error";
    case SCPI_E_COMMAND:       return "Command error";
    case SCPI_E_MISSING_PARAM: return "Missing parameter";
    case SCPI_E_HEADER:        return "Undefined header";
    case SCPI_E_SETTINGS:      return "Settings conflict";
    case SCPI_E_PARAM:         return "Illegal parameter value";
    case SCPI_E_QUERY:         return "Query UNTERMINATED";
    case -350:                 return "Queue overflow";
    default:                   return "Error";
    }
}

/* ---- matching ------------------------------------------------------------------ */

/* Token matches a pattern word in short form (its uppercase letters, plus
 * any digits) or long form. Case-insensitive. */
bool scpi_match_word(const char *t, const char *p)
{
    char shortf[24], longf[24];
    size_t ns = 0, nl = 0;
    for (const char *q = p; *q && nl < sizeof longf - 1; q++) {
        longf[nl++] = (char)toupper((unsigned char)*q);
        if ((isupper((unsigned char)*q) || isdigit((unsigned char)*q) || *q == '*')
            && ns < sizeof shortf - 1)
            shortf[ns++] = *q;
    }
    shortf[ns] = 0;
    longf[nl] = 0;
    char up[24];
    size_t i;
    for (i = 0; t[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)t[i]);
    up[i] = 0;
    return !strcmp(up, shortf) || !strcmp(up, longf);
}

/* Split a pattern into words, noting which are optional. */
typedef struct { char w[24]; bool opt; } pword_t;

static int pattern_words(const char *p, pword_t *out, int max)
{
    int n = 0;
    while (*p && n < max) {
        bool opt = false;
        if (*p == '[') { opt = true; p++; }
        if (*p == ':') p++;
        size_t k = 0;
        while (*p && *p != ':' && *p != '[' && *p != ']' && k < sizeof out[n].w - 1)
            out[n].w[k++] = *p++;
        out[n].w[k] = 0;
        if (*p == ']') p++;
        out[n].opt = opt;
        if (k) n++;
    }
    return n;
}

static bool match(const pword_t *pw, int np, char tok[][24], int nt)
{
    if (np == 0) return nt == 0;
    if (nt > 0 && scpi_match_word(tok[0], pw[0].w) && match(pw + 1, np - 1, tok + 1, nt - 1))
        return true;
    if (pw[0].opt) return match(pw + 1, np - 1, tok, nt);
    return false;
}

/* ---- execution --------------------------------------------------------------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void run_one(scpi_t *s, char *cmd)
{
    cmd = trim(cmd);
    if (!*cmd) return;

    /* header | params */
    char *params = cmd;
    while (*params && !isspace((unsigned char)*params)) params++;
    if (*params) *params++ = 0;

    scpi_call_t c = { .suffix = -1 };
    size_t hl = strlen(cmd);
    if (hl && cmd[hl - 1] == '?') { c.query = true; cmd[--hl] = 0; }
    if (*cmd == ':') cmd++;

    char tok[8][24];
    int nt = 0;
    for (char *t = strtok(cmd, ":"); t && nt < 8; t = strtok(NULL, ":")) {
        snprintf(tok[nt], sizeof tok[nt], "%s", t);
        nt++;
    }
    if (nt == 0) { scpi_error(s, SCPI_E_COMMAND); return; }

    /* numeric suffix on the last keyword (not on common commands) */
    char *last = tok[nt - 1];
    size_t ll = strlen(last);
    if (last[0] != '*' && ll > 1 && isdigit((unsigned char)last[ll - 1])) {
        c.suffix = last[ll - 1] - '0';
        last[ll - 1] = 0;
    }

    params = trim(params);
    if (*params) {
        for (char *a = strtok(params, ","); a && c.argc < SCPI_MAX_ARGS; a = strtok(NULL, ",")) {
            a = trim(a);
            size_t al = strlen(a);
            if (al >= 2 && (a[0] == '"' || a[0] == '\'') && a[al - 1] == a[0]) {
                a[al - 1] = 0;
                a++;
            }
            c.argv[c.argc++] = a;
        }
    }

    for (int i = 0; i < s->ncmds; i++) {
        pword_t pw[8];
        int np = pattern_words(s->cmds[i].pattern, pw, 8);
        if (match(pw, np, tok, nt)) { s->cmds[i].fn(s, &c); return; }
    }
    scpi_error(s, SCPI_E_HEADER);
}

void scpi_execute(scpi_t *s, char *line)
{
    /* split on ';' outside quotes */
    char *start = line;
    bool q = false;
    for (char *p = line; ; p++) {
        if (*p == '"') q = !q;
        if (*p == 0 || (*p == ';' && !q)) {
            char end = *p;
            *p = 0;
            run_one(s, start);
            if (!end) break;
            start = p + 1;
        }
    }
}

static void echo(scpi_t *s, const char *t, size_t n)
{
    if (s->terminal && s->write) s->write(s->ctx, t, n);
}

void scpi_input(scpi_t *s, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        if (s->terminal) {
            bool after_cr = s->last_cr;
            s->last_cr = ch == '\r';
            if (ch == '\n' && after_cr) continue;           /* CRLF: the CR did it */
            if (ch == '\r') ch = '\n';
            if (ch == '\b' || ch == 0x7F) {                 /* backspace / DEL */
                if (s->len) { s->len--; echo(s, "\b \b", 3); }
                continue;
            }
            if (ch == 0x03 || ch == 0x15) {                 /* Ctrl-C, Ctrl-U */
                s->len = 0;
                s->overflow = false;
                echo(s, "^C\r\n", 4);
                continue;
            }
            if (ch == '\n') echo(s, "\r\n", 2);
            else echo(s, &ch, 1);
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (s->overflow) scpi_error(s, SCPI_E_COMMAND);
            else { s->line[s->len] = 0; scpi_execute(s, s->line); }
            s->len = 0;
            s->overflow = false;
            continue;
        }
        if (s->len < SCPI_LINE_MAX - 1) s->line[s->len++] = ch;
        else s->overflow = true;
    }
}

/* ---- parameter helpers --------------------------------------------------------- */

bool scpi_parse_bool(const char *a, bool *out)
{
    if (!a) return false;
    if (scpi_match_word(a, "ON") || !strcmp(a, "1")) { *out = true; return true; }
    if (scpi_match_word(a, "OFF") || !strcmp(a, "0")) { *out = false; return true; }
    return false;
}

bool scpi_parse_number(const char *a, double *out)
{
    if (!a || !*a) return false;
    char *end;
    double v = strtod(a, &end);
    if (end == a) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = v;
    return true;
}
