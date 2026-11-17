/* lib/scpi: the parser on its own, with a recording handler table. */
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "scpi.h"

static scpi_t s;
static char out[256];
static char last_cmd[32];
static scpi_call_t last;
static char args[SCPI_MAX_ARGS][32];

static void wr(void *ctx, const char *t, size_t n)
{
    (void)ctx;
    strncat(out, t, n);
}

static void rec(scpi_t *sc, const scpi_call_t *c, const char *name)
{
    (void)sc;
    snprintf(last_cmd, sizeof last_cmd, "%s", name);
    last = *c;
    for (int i = 0; i < c->argc; i++) {
        snprintf(args[i], sizeof args[i], "%s", c->argv[i]);
        last.argv[i] = args[i];
    }
}
#define H(n) static void h_##n(scpi_t *sc, const scpi_call_t *c) { rec(sc, c, #n); }
H(idn) H(vdc) H(vac) H(meas) H(rate)

static const scpi_cmd_t cmds[] = {
    { "*IDN", h_idn },
    { "CONFigure:VOLTage[:DC]", h_vdc },
    { "CONFigure:VOLTage:AC", h_vac },
    { "MEASure[:SHOW]", h_meas },
    { "RATE", h_rate },
};

void setUp(void)
{
    scpi_init(&s, cmds, 5, NULL, wr);
    out[0] = 0;
    last_cmd[0] = 0;
    memset(&last, 0, sizeof last);
}
void tearDown(void) {}

static void run(const char *line)
{
    last_cmd[0] = 0;
    scpi_input(&s, line, strlen(line));
}

static void test_short_and_long_forms(void)
{
    const char *forms[] = { "CONF:VOLT\n", "configure:voltage\n", "Conf:Volt:DC\n",
                            ":CONFIGURE:VOLTAGE:DC\n" };
    for (size_t i = 0; i < 4; i++) { run(forms[i]); TEST_ASSERT_EQUAL_STRING("vdc", last_cmd); }
    run("CONF:VOLT:AC\n");
    TEST_ASSERT_EQUAL_STRING("vac", last_cmd);
    run("CONFI:VOLT\n");                       /* neither short nor long */
    TEST_ASSERT_EQUAL_STRING("", last_cmd);
    TEST_ASSERT_EQUAL_INT(SCPI_E_HEADER, scpi_error_pop(&s));
}

static void test_query_suffix_and_params(void)
{
    run("MEAS2?\n");
    TEST_ASSERT_EQUAL_STRING("meas", last_cmd);
    TEST_ASSERT_TRUE(last.query);
    TEST_ASSERT_EQUAL_INT(2, last.suffix);
    run("MEAS?\n");
    TEST_ASSERT_EQUAL_INT(-1, last.suffix);
    run("CONF:VOLT 5, \"AUTO\" \r\n");
    TEST_ASSERT_FALSE(last.query);
    TEST_ASSERT_EQUAL_INT(2, last.argc);
    TEST_ASSERT_EQUAL_STRING("5", last.argv[0]);
    TEST_ASSERT_EQUAL_STRING("AUTO", last.argv[1]);
    run("*idn?\n");
    TEST_ASSERT_EQUAL_STRING("idn", last_cmd);
}

static void test_semicolons_and_partial_lines(void)
{
    run("RATE F;CONF:VOLT:AC");
    TEST_ASSERT_EQUAL_STRING("", last_cmd);    /* nothing until the newline */
    run("\n");
    TEST_ASSERT_EQUAL_STRING("vac", last_cmd);
}

/* B45: terminal mode: CR ends a line, typing is echoed, backspace edits. */
static void test_terminal_mode(void)
{
    s.terminal = true;
    out[0] = 0;
    run("RATE X\bF\r");
    TEST_ASSERT_EQUAL_STRING("rate", last_cmd);
    TEST_ASSERT_EQUAL_STRING("F", last.argv[0]);
    TEST_ASSERT_EQUAL_STRING("RATE X\b \bF\r\n", out);
    last_cmd[0] = 0;
    run("\n");                                     /* the LF of a CRLF: nothing */
    TEST_ASSERT_EQUAL_STRING("", last_cmd);
    s.terminal = false;
}

static void test_overlong_line_is_an_error(void)
{
    char big[SCPI_LINE_MAX + 20];
    memset(big, 'A', sizeof big - 2);
    big[sizeof big - 2] = '\n';
    big[sizeof big - 1] = 0;
    run(big);
    TEST_ASSERT_EQUAL_INT(SCPI_E_COMMAND, scpi_error_pop(&s));
    run("RATE S\n");                           /* and recovers */
    TEST_ASSERT_EQUAL_STRING("rate", last_cmd);
}

static void test_error_queue_and_helpers(void)
{
    TEST_ASSERT_EQUAL_INT(0, scpi_error_pop(&s));
    for (int i = 0; i < SCPI_ERRQ + 3; i++) scpi_error(&s, SCPI_E_PARAM);
    for (int i = 0; i < SCPI_ERRQ - 1; i++) TEST_ASSERT_EQUAL_INT(SCPI_E_PARAM, scpi_error_pop(&s));
    TEST_ASSERT_EQUAL_INT(-350, scpi_error_pop(&s));       /* overflow marker */
    bool b;
    TEST_ASSERT_TRUE(scpi_parse_bool("on", &b) && b);
    TEST_ASSERT_TRUE(scpi_parse_bool("0", &b) && !b);
    TEST_ASSERT_FALSE(scpi_parse_bool("maybe", &b));
    double v;
    TEST_ASSERT_TRUE(scpi_parse_number("500E-3", &v));
    TEST_ASSERT_EQUAL_DOUBLE(0.5, v);
    TEST_ASSERT_FALSE(scpi_parse_number("5V", &v));
    scpi_reply(&s, "hello");
    TEST_ASSERT_EQUAL_STRING("hello\r\n", out);        /* stock's line ending */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_and_long_forms);
    RUN_TEST(test_query_suffix_and_params);
    RUN_TEST(test_semicolons_and_partial_lines);
    RUN_TEST(test_terminal_mode);
    RUN_TEST(test_overlong_line_is_an_error);
    RUN_TEST(test_error_queue_and_helpers);
    return UNITY_END();
}
