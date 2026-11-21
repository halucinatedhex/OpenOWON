# Developer guide

How to build, test and extend OpenOWON. For what it does, see
[`features.md`](features.md). For the hardware, see [`hardware.md`](hardware.md)
and the bench runbook, [`bench.md`](bench.md).

## The idea that makes this work

The application never touches hardware directly. Everything it needs from
the board goes through one struct, `platform_t` (`lib/app/platform.h`):
- the front-end and panel buses;
- the clock, keys, beeper and backlight;
- the serial port;
- the calibration record;
- two flash regions.

On the meter, `lib/hal/` fills it in with GD32 registers. On your computer,
`lib/sim/` fills it in with models: a bit-level HY3131 that speaks the real
SPI protocol, a panel decoder that rebuilds the screen from the 8080 bus
traffic, a NOR-flash model that can cut power at any write, a virtual clock,
and a key queue.

So the same code, unchanged, runs in three places:
- the unit tests;
- the simulator, which runs the whole meter, takes screenshots and runs
  scripted scenarios;
- the meter.

Almost all feature work happens and is proven on the host.

```
  lib/app      application: functions, ranging, math, screens, menus, recording, SCPI
     |
  platform_t   the only way out of the application
     |
  +--------------------------+------------------------------+
  lib/hal (meter)                     lib/sim (host)
  GD32 registers, gated by            HY3131 model, panel decoder,
  hal_pins.h                          flash model, virtual clock

  shared by both: lib/measure  lib/afe  lib/display  lib/ui  lib/store  lib/datalog  lib/scpi
```

| Library | What it does | Tested by |
| --- | --- | --- |
| `lib/measure` | every function's arithmetic, calibration record, filter, temperature, dB | `test_meas`, `test_temp` (against the stock image) |
| `lib/afe` | HY3131 driver | `test_afe` |
| `lib/display` | panel driver, 8-bit 8080 bus | `test_display` |
| `lib/ui` | framebuffer, fonts, themes, screens | `test_ui` |
| `lib/store` | power-safe settings records | `test_store` (power cut at every write) |
| `lib/datalog` | the 10,668-point log | `test_datalog` (power cut at every write) |
| `lib/scpi` | SCPI parser | `test_scpi` |
| `lib/app` | the application | `test_app` (end to end), `test_ranging` (timing) |
| `lib/hal` | GD32 board support, RTT console | on the meter |

## Setting up

You need:
- [PlatformIO](https://platformio.org) Core (`pip install platformio`), which
  fetches the ARM toolchain and the GD32 framework itself;
- Python 3;
- `make`;
- `pyserial`, but only for the bench scripts.

```sh
git clone <repo> && cd <repo>
# put OWON's v2.1.2 image in firmware/ (firmware/README.md says where)
make setup      # checks the image, generates lib/ui/stock_fonts.h from it
make check      # should end with both target builds SUCCESS
```

`make` on its own lists every target.

## The everyday loop

| Command | What it runs | Time |
| --- | --- | --- |
| `make test` | all host unit tests (Unity, `pio test -e native`) | about 30 s |
| `pio test -e native -f test_app` | one suite | seconds |
| `make sim` | every scenario in `scenarios/`; screenshots in `build/sim-*.png` | seconds |
| `make screens` | `make sim`, then copies its shots to `docs/screens/`, and `scripts/readme-media.py` renders the hero shots, histograms and the two GIFs (the README's pictures). Run it when the UI changes | seconds |
| `make ui` | every screen in every theme, to `build/ui-*.png` | seconds |
| `make portable` | the on-meter layers built for Cortex-M4 with `-Werror` | seconds |
| `make check` | all of the above plus script tests and both firmware images | about a minute |

Run `make check` before every commit.

### Scenarios

A scenario is a script for the simulated meter, and each `expect` is a
check:

```
set dcv 0.0123          # the physical input: dcv acv dci aci ohm diode cap freq duty temp cj noise
key V                   # front-panel keys: V I OHM CAP FREQ TEMP UP DOWN LEFT RIGHT RUNSTOP MATH RECORD UTILITY F1..F6
run 2000                # advance virtual time, ms
expect reading 12.300   # reading unit range func status sub key1..key6 menu reply
send MEAS?              # a line into the serial port
expect replynear 0.0123 1e-3
shot build/sim-x.bmp    # save the panel
```

`./build/meter-sim scenarios/tour.sim` runs one scenario. The command exits
non-zero if any check fails.

## Adding a feature

A worked example: backlog item **B18, a diode beep threshold**. Today the
diode beep is fixed at 0.7 V (`sample()` in `lib/app/app.c`); stock lets you
set it, and an owner asked for it to be remembered. The same steps apply to
almost any feature.

### 1. Find it in the backlog

[`backlog.md`](backlog.md) gives each item its type (firmware, bench,
hardware mod), value, effort and priority. If the feature isn't there, add
it. When it's done, mark it done in the same commit.

### 2. State lives in `app_t`, defaults in one place

Add the field to `app_t` in `lib/app/app.h`, next to its neighbours:

```c
uint16_t  diode_threshold_mv;     /* beep at or below, default 700 */
```

Then give it its default in `prefs_defaults()` in `app.c`. That function is
the single source of defaults, and Load Defaults uses it too.

### 3. Behaviour

Change the logic where it lives. Here, one line in `sample()`:

```c
if (a->beeper && a->fn == MEAS_DIODE)
    want = s->value >= 0.0 && s->value * 1000.0 <= a->diode_threshold_mv;
```

Rules that keep the code portable:
- **No hardware access in `lib/app`.** If the feature needs something from
  the board, add a field to `platform_t`, give it a NULL-safe default, and
  implement it in `lib/hal` and `lib/sim`.
- **No floating-point surprises on the meter.** It has an FPU for floats;
  doubles are fine but slower.
- **Watch memory.** The chip has 48 KB of RAM, and `make fw-all` prints the
  RAM use; we are around 27 KB. Never hold a whole frame: draw through `fb_t`
  and let `app_render` band it.

### 4. Remember it across power cycles

Append it to the settings record in `lib/app/settings.c`. The record only
ever grows:
1. Add the byte(s) at the end.
2. Raise the length in `settings_pack`.
3. In `settings_apply`, read them only `if (len >= N)` and validate them.

```c
b[27] = (uint8_t)(a->diode_threshold_mv / 10);          /* 10 mV steps */
...
if (len >= 28 && b[27] >= 10 && b[27] <= 300) a->diode_threshold_mv = b[27] * 10u;
```

An older, shorter record then still loads, and the new field keeps its
default. Saving is debounced and power-safe; you don't call anything.

### 5. Put it on the screen

For a preference, add a row to the Utility menu (`lib/app/menu.c`):
1. a `MI_` entry in the enum in `menu.h`;
2. its label in `menu_labels`;
3. its text in `menu_value`;
4. its step in `menu_adjust`.

Labels are UTF-8 in the anti-aliased label font: write units in their real
case ("min", "mA", "°C"; `docs/ui-development.md`). Changes made
there apply immediately through `app_apply_prefs`.

### 6. Make it controllable remotely

Add a handler and one line to the `cmds[]` table in `lib/app/app_scpi.c`.
Follow the nearest existing command. `CONTinuity:THREshold` is the model
here:

```c
{ "DIODe:THREshold", h_diode_thr },
```

Long form in capitals marks the short form (`DIOD:THRE`). Queries get the
same pattern with `?`. Reply in stock's formats where stock has the command;
document it in [`scpi.md`](scpi.md) either way.

### 7. Test it on the host

In `test/test_app/test_app.c`, the sim board gives you real end-to-end
tests:

```c
static void test_diode_threshold(void)
{
    app_key(&app, KEY_OHM); app_key(&app, KEY_OHM); app_key(&app, KEY_OHM);   /* diode */
    w->diode = 0.55;
    app.diode_threshold_mv = 500;
    sim_board_run(&app, 500);
    TEST_ASSERT_FALSE(sim_board_beeping());
    menu_goto(MI_DIODE_THRESHOLD);
    app_key(&app, KEY_RIGHT);                      /* 600 mV */
    sim_board_run(&app, 500);
    TEST_ASSERT_TRUE(sim_board_beeping());
}
```

Add it to `main()` with `RUN_TEST`.

**Also cover:**
- a reboot: the `reboot()` helper keeps flash and restarts the app;
- an old record without the field;
- the SCPI command.

**Then check the tests catch something.** Break the feature on purpose and
confirm a test fails. Several bugs in this project were found only that way.

If it changes what's on screen, add a step to a scenario with a `shot`, and
look at the picture.

### 8. Document it

| Where | What |
| --- | --- |
| [`features.md`](features.md) | how to use it |
| [`scpi.md`](scpi.md) | any new command |
| [`backlog.md`](backlog.md) | mark it done |
| [`app.md`](app.md) | design notes, if the reasoning isn't obvious |
| `README.md` | only if it belongs in the feature list or the comparison with stock |

### 9. Check and commit

Run `make check`, then commit with a message that says what changed and
why. Commits end with any `Co-Authored-By` lines that apply.

## Features that need the meter

Some work can't be finished on the host:
- the front end's per-range register values (`afe_configure`);
- the key matrix;
- power hold;
- anything timing-related.

The pattern:
1. **Build it on the host** against the model, behind a `platform_t` field.
2. **Write down what the bench must confirm**, as a backlog item and in
   `app.md` ("Assumptions the bench must confirm").
3. **On the meter, gate it.** `lib/hal/hal_pins.h` has a flag per
   subsystem, off until confirmed on the v3 board.
   - The `xdm1241` build drives no pins at all, which makes it the safe
     first flash.
   - `xdm1241-bench` drives what is confirmed.

Hardware rules, which the scripts enforce:
- Never write the chip without a verified backup.
- Nothing may erase at or above 0x0803D800, where the stock log,
  preferences and calibration live.
- Flash with `scripts/flash.sh`, never with `pio run -t upload`.

[`bench.md`](bench.md) is the runbook.

## Conventions

- **Validate against something independent.** Recovered data and
  arithmetic are tested against the stock image or other owners' data,
  never only by inspection.
- **Don't ship unverified data.** A wrong table is worse than a missing one.
  When unsure, gate it off and say so.
- **Write down dead ends,** so they aren't retried.
- **Keep it honest in the docs.** Say what is proven on the meter and what
  only in the simulator.

## Where things are written up

[`docs/README.md`](README.md) indexes every document in three groups:
- **using** the firmware;
- **developing** it;
- **reference:** how the stock firmware and hardware were reverse-engineered.
