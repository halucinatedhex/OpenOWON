/* A small SCPI command parser. Portable, no allocation.
 *
 *   - lines end in \n (a preceding \r is ignored); ';' separates commands
 *   - keywords match in short or long form, case-insensitively: a pattern
 *     word "CONFigure" accepts CONF or CONFIGURE
 *   - [optional] nodes: "CONFigure:VOLTage[:DC]" matches CONF:VOLT and CONF:VOLT:DC
 *   - a trailing digit on the last keyword is a suffix: MEAS2? -> suffix 2
 *   - '?' after the header makes it a query
 *   - parameters follow whitespace, comma-separated, outer quotes stripped
 *   - errors go to a small queue, read with SYST:ERR? (scpi_error_pop)
 */
#ifndef SCPI_H
#define SCPI_H

#include <stdbool.h>
#include <stddef.h>

#define SCPI_LINE_MAX   128
#define SCPI_MAX_ARGS     4
#define SCPI_ERRQ         8

/* SCPI-99 standard error codes used here */
#define SCPI_E_COMMAND        (-100)
#define SCPI_E_HEADER        (-113)    /* undefined header */
#define SCPI_E_PARAM         (-224)    /* illegal parameter value */
#define SCPI_E_MISSING_PARAM (-109)
#define SCPI_E_SETTINGS      (-221)    /* settings conflict */
#define SCPI_E_QUERY         (-420)    /* query unterminated / not allowed */

typedef struct scpi scpi_t;

typedef struct {
    bool        query;
    int         suffix;        /* numeric suffix on the last keyword, or -1 */
    int         argc;
    const char *argv[SCPI_MAX_ARGS];
} scpi_call_t;

typedef void (*scpi_handler_t)(scpi_t *s, const scpi_call_t *c);

typedef struct {
    const char    *pattern;    /* e.g. "CONFigure:VOLTage[:DC]" or "*IDN" */
    scpi_handler_t fn;
} scpi_cmd_t;

struct scpi {
    const scpi_cmd_t *cmds;
    int               ncmds;
    void             *ctx;                         /* for handlers */
    void            (*write)(void *ctx, const char *s, size_t n);
    char              line[SCPI_LINE_MAX];
    size_t            len;
    bool              overflow;
    /* Terminal mode (B45), for typing by hand: CR ends a line (screen and
     * most terminals send CR for Enter), typed characters are echoed,
     * backspace/DEL edit, Ctrl-C/Ctrl-U clear the line. Off: stock's
     * behaviour, LF ends a line and nothing is echoed. */
    bool              terminal;
    bool              last_cr;                     /* swallow the LF of a CRLF */
    int               errq[SCPI_ERRQ];
    int               nerr;
};

void scpi_init(scpi_t *s, const scpi_cmd_t *cmds, int ncmds, void *ctx,
               void (*write)(void *ctx, const char *s, size_t n));

/* Feed received bytes; complete lines are executed. */
void scpi_input(scpi_t *s, const char *data, size_t n);

/* Execute one line now (no terminator needed). */
void scpi_execute(scpi_t *s, char *line);

/* Send a reply line; "\r\n" is appended, as stock does. */
void scpi_reply(scpi_t *s, const char *text);
void scpi_replyf(scpi_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void scpi_error(scpi_t *s, int code);
int  scpi_error_pop(scpi_t *s);             /* 0 when empty */
const char *scpi_error_text(int code);

/* Helpers for handlers. */
bool scpi_match_word(const char *token, const char *pattern_word);
bool scpi_parse_bool(const char *a, bool *out);         /* ON/OFF/1/0 */
bool scpi_parse_number(const char *a, double *out);     /* 1.5, 500E-3, 5m? no */

#endif
