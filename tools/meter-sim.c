/* HOST ONLY. The whole meter on your Mac, driven by a script.
 *
 *   build/meter-sim sim/demo.sim
 *
 * Script lines (# starts a comment):
 *   set <quantity> <value>   dcv acv dci aci ohm diode cap freq duty temp cj noise
 *   key <name>               V I OHM CAP FREQ TEMP UP DOWN LEFT RIGHT RUNSTOP
 *                            MATH RECORD UTILITY DUAL F1..F6
 *   run <ms>                 advance virtual time
 *   shot <path.bmp>          save the panel as it is right now
 *   send <scpi line>         send a line to the meter's serial port
 *   expect <field> <text>    field: reading unit range func status sub, or
 *                            reply (the meter's next serial line)
 *                            (fails the run if different)
 *   print                    print the current view
 *
 * Exit status is the number of failed expectations, so a script is also a test.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "sim_board.h"

static const struct { const char *name; app_key_t k; } keys[] = {
    { "V", KEY_V }, { "I", KEY_I }, { "OHM", KEY_OHM }, { "CAP", KEY_CAP },
    { "FREQ", KEY_FREQ }, { "TEMP", KEY_TEMP }, { "UP", KEY_UP }, { "DOWN", KEY_DOWN },
    { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT }, { "RUNSTOP", KEY_RUNSTOP },
    { "MATH", KEY_MATH }, { "RECORD", KEY_RECORD }, { "UTILITY", KEY_UTILITY },
    { "DUAL", KEY_DUAL }, { "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 },
    { "F4", KEY_F4 }, { "F5", KEY_F5 }, { "F6", KEY_F6 },
};

static double *quantity(sim_world_t *w, const char *q)
{
    if (!strcmp(q, "dcv")) return &w->dcv;
    if (!strcmp(q, "acv")) return &w->acv;
    if (!strcmp(q, "dci")) return &w->dci;
    if (!strcmp(q, "aci")) return &w->aci;
    if (!strcmp(q, "ohm")) return &w->ohm;
    if (!strcmp(q, "diode")) return &w->diode;
    if (!strcmp(q, "cap")) return &w->cap;
    if (!strcmp(q, "freq")) return &w->freq;
    if (!strcmp(q, "duty")) return &w->duty;
    if (!strcmp(q, "temp")) return &w->temp_c;
    if (!strcmp(q, "cj")) return &w->cj_c;
    if (!strcmp(q, "noise")) return &w->noise_counts;
    if (!strcmp(q, "jack_a")) return &w->jack_a;
    return NULL;
}

static const char *field(const app_t *a, const char *f)
{
    const app_view_t *v = &a->view;
    if (!strncmp(f, "key", 3) && f[3] >= '1' && f[3] <= '6' && !f[4]) return v->key_value[f[3] - '1'];
    if (!strcmp(f, "menu")) return a->menu_open ? v->menu_value[a->menu_sel] : "(closed)";
    if (!strcmp(f, "reading")) return v->reading;
    if (!strcmp(f, "unit")) return v->unit;
    if (!strcmp(f, "range")) return v->range;
    if (!strcmp(f, "func")) return v->func;
    if (!strcmp(f, "status")) return v->status;
    if (!strcmp(f, "sub")) return v->sub;
    return NULL;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s script.sim\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) { perror(argv[1]); return 2; }

    static app_t app;
    sim_board_init(NULL);
    app_init(&app, sim_board_platform());
    sim_world_t *w = sim_hy3131_world();

    char line[256];
    int lineno = 0, failures = 0;
    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        trim(line);
        char cmd[32] = "", a1[128] = "";
        if (sscanf(line, "%31s %127[^\n]", cmd, a1) < 1) continue;

        if (!strcmp(cmd, "set")) {
            char q[32]; double v;
            if (sscanf(a1, "%31s %lf", q, &v) == 2 && !strcmp(q, "battery")) {   /* volts, -1 none */
                sim_board_set_battery_mv(v < 0 ? -1 : (int)(v * 1000.0 + 0.5));
                continue;
            }
            double *p = (sscanf(a1, "%31s %lf", q, &v) == 2) ? quantity(w, q) : NULL;
            if (!p) { fprintf(stderr, "%d: bad set '%s'\n", lineno, a1); return 2; }
            *p = v;
        } else if (!strcmp(cmd, "key")) {
            size_t i;
            for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
                if (!strcmp(keys[i].name, a1)) { sim_board_press(keys[i].k); break; }
            if (i == sizeof keys / sizeof keys[0]) { fprintf(stderr, "%d: no key '%s'\n", lineno, a1); return 2; }
        } else if (!strcmp(cmd, "menu")) {   /* menu <LABEL>: open the Utility menu on that row */
            int it = -1;
            for (int i = 0; i < MI_COUNT; i++) {
                const char *l = menu_labels[i];
                while (*l == ' ') l++;
                if (!strcmp(l, a1)) it = i;
            }
            if (it < 0) { fprintf(stderr, "%d: no menu item '%s'\n", lineno, a1); return 2; }
            if (!app.menu_open) app_key(&app, KEY_UTILITY);
            for (int n = 0; app.menu_sel != it && n < MI_COUNT; n++)
                app_key(&app, app.menu_sel < it ? KEY_DOWN : KEY_UP);
        } else if (!strcmp(cmd, "run")) {
            sim_board_run(&app, (uint32_t)strtoul(a1, NULL, 10));
        } else if (!strcmp(cmd, "shot")) {
            if (sim_lcd_save_bmp(a1)) { perror(a1); return 2; }
            printf("  %s\n", a1);
        } else if (!strcmp(cmd, "send")) {
            sim_board_serial_send(a1);
        } else if (!strcmp(cmd, "expect")) {
            char f[32], want[96] = "", line[128];
            sscanf(a1, "%31s %95[^\n]", f, want);
            if (!strcmp(f, "replynear")) {        /* expect replynear <value> <relative tolerance> */
                double v, tol, g;
                char *end;
                if (sscanf(want, "%lf %lf", &v, &tol) != 2) {
                    fprintf(stderr, "%d: replynear needs <value> <tolerance>\n", lineno);
                    return 2;
                }
                const char *r = sim_board_serial_take(line, sizeof line) ? line : "<no reply>";
                g = strtod(r, &end);
                if (end == r || fabs(g - v) > tol * fabs(v)) {
                    fprintf(stderr, "%s:%d: expected a reply within %g of %g, got '%s'\n",
                            argv[1], lineno, tol, v, r);
                    failures++;
                }
                continue;
            }
            const char *got = field(&app, f);
            if (!strcmp(f, "reply"))
                got = sim_board_serial_take(line, sizeof line) ? line : "<no reply>";
            if (!got) { fprintf(stderr, "%d: no field '%s'\n", lineno, f); return 2; }
            if (strcmp(got, want)) {
                fprintf(stderr, "%s:%d: expected %s '%s', got '%s'\n", argv[1], lineno, f, want, got);
                failures++;
            }
        } else if (!strcmp(cmd, "print")) {
            const app_view_t *v = &app.view;
            printf("  t=%ums  %s  [%s %s]  %s  %s\n", sim_board_millis(), v->reading, v->unit,
                   v->range, v->status, v->sub);
        } else {
            fprintf(stderr, "%d: unknown command '%s'\n", lineno, cmd);
            return 2;
        }
    }
    fclose(fp);
    if (failures) fprintf(stderr, "%d expectation(s) failed\n", failures);
    return failures;
}
