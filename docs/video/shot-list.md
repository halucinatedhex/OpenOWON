# Shot checklist: filming the highlight reel

The bench session for [the video plan](../video-plan.md), in **filming
order**: grouped by setup so each wiring is done once, not in the order
the beats appear in the edit. Tick as you go. About 60–90 minutes.

Every caption is a measured number, so where a shot has a **Check**,
look at it before moving on: a re-shoot later costs a whole setup.

## Before you start

- [ ] Meter on the **ui build**, charged (the battery gauge shows on screen).
- [ ] Clock set, so the recording browse shows real times:
      `SYST:DATE yyyy,mm,dd` and `SYST:TIME hh,mm,ss` over serial.
- [ ] **UTILITY → Theme: dark.** Beeper on, volume up, style CLASSIC.
- [ ] Screen wiped; a plain dark mat under the meter.
- [ ] Phone on a stand above the meter, tilted ~15° against glare, **9:16**.
      Frame the screen in the upper middle, left of the app buttons
      ([frame guide](frame-guide.png)).
- [ ] Lock focus and exposure on the screen. Shutter **1/50 or 1/60 s**
      (no banding); for 240 fps use the phone's slow-motion mode.
- [ ] Sound: quiet room, no music playing; the beeps are part of the edit.
- [ ] Parts to hand: 10 kΩ resistor, red LED, 9 V battery, 10 kΩ pot wired
      across the 9 V battery (wiper and one end to the probes), the 9 VAC
      transformer with clip leads, a Li-ion cell (protected, in a holder)
      and the bench supply, the laptop on the meter's USB.

Keys used below: the softkeys are **F1–F5** beside the screen, top to
bottom; F3 is **VIEW** on the measurement screen.

## Setup A: probes in VΩ and COM

### Beat 1, the hook: ohms (240 fps)

- [ ] **OHM** (resistance, AUTO in the footer).
- [ ] Probes on the 10 kΩ resistor. Lift them, wait 2 s, touch again.
      Three or four takes.
- [ ] **Check:** the reading is back in under a second (count frames at
      240 fps: under 240 frames). The caption claims "Stock: 9 s".

### Beat 2: continuity (240 fps)

- [ ] **OHM** again until the footer says CONT.
- [ ] Tap the probe tips together quickly, 5–6 taps in 3 s.
- [ ] **Check:** a beep for every tap, none missed (listen back).

### Beat 3: beep styles

- [ ] **UTILITY**, UP/DOWN to **BEEP STYLE**.
- [ ] Step with **F3/F4** through DROP, DOUBLE, TWO-TONE, WARBLE, one every
      ~1 s: each step plays its sound. Film the screen close enough to
      read the style name.
- [ ] Back to **CLASSIC**, then **UTILITY** to close.

### Beat 4: diode

- [ ] **OHM** until DIODE.
- [ ] Red LED on the probes, the right way round: it glows, one short beep.
- [ ] **Check:** the LED visibly lights in frame. Dim the room a little if not.

### Beat 5: auto-hold

- [ ] **V** (DC volts). **RUN/STOP** twice: the top bar says **A-HOLD**.
- [ ] Probe the 9 V battery, wait for the beep, lift the probes. Hold the
      shot 2 s on the held reading.

### Beats 6 and 7: mini trend, graph, histogram

- [ ] **RUN/STOP** once to get back to Run. Probes on the pot's wiper and
      one end; **F4** clears the history.
- [ ] **F3** once: number + mini trend. Turn the pot slowly up and back
      over ~5 s. (Beat 6.)
- [ ] **F3** twice more: the full graph. Turn the pot again. **F3** once
      more: the histogram. (Beat 7.)
- [ ] **F3** to get back to the number.

### Beat 10: recording at 15 ms

- [ ] **RECORD**. **F3** until the interval reads **15 ms**. **F1** starts.
- [ ] Turn the pot through a sweep or two over ~10 s. **F1** stops.
- [ ] ▲ / ▼ move the cursor along the trace: film 3 s of browsing.
- [ ] **Check:** the reading count shows a few hundred readings
      (574 in ~10 s last time). The caption says "every 15 ms".
- [ ] **F5** back to the measurement.

## Setup B: the transformer

### Beat 9: AC volts and frequency

- [ ] Transformer's 9 VAC output on VΩ and COM (clip leads; mains side
      fully insulated).
- [ ] **V** until the footer says AC volts, then **DUAL**, **F1 FREQ**.
- [ ] **Check:** ~9–10 V AC and 60.0 Hz (50 Hz if that's your mains) under it.
- [ ] Unplug the transformer before touching the leads.

## Setup C: a Li-ion cell charging, through the A jack

### Beat 8: power, then beats 11 and 13 on the same screen

Wiring (low side, as `features.md` says):

- **Supply −** → meter **COM**
- meter **A** jack → **cell −**
- **cell +** → **supply +**
- meter **VΩ** lead → **cell +**

- [ ] Supply set to **4.20 V**, current limit **0.5 A**, output off.
      Cell between 3.0 and 4.1 V (check it on DC volts first).
- [ ] Red lead moved to the **A** jack. **I** (DC current), **F5** until the
      footer shows the A jack's range. **DUAL → F3 POWER**.
- [ ] Supply on. **F4 CLEAR Wh** so the totals start from zero.
- [ ] **Check:** watts **positive**, current a few hundred mA, Wh / mAh /
      time ticking. Negative watts means a lead is reversed: supply off,
      fix it.
- [ ] Film 5 s of the totals ticking. (Beat 8.)
- [ ] **Beat 11, themes:** UTILITY → Theme, then for each of stock, dark,
      amber, paper, synthwave close the menu and film 2 s of the power
      screen. The edit jump-cuts them on the beat.
- [ ] **Beat 13, end card:** on **synthwave**, hold 6 s untouched.
- [ ] Supply off. Never leave the cell charging unwatched.
- [ ] Theme back to dark.

## Setup D: the laptop

### Beat 12: installing over USB

- [ ] Optional, and **it rewrites the app** (the same ui build, so no
      change): film the laptop screen with the installer's pack-by-pack
      progress, then the meter restarting into OpenOWON. Command in
      `install.md`. Or record the screen while I run it, on your OK.
- [ ] Or skip the real install: film the terminal and the meter's boot
      separately (power it off and on).

## After

- [ ] Copy the clips off the phone; name them by beat (`b01-ohms-1.mov`, …).
- [ ] Note any reading that differs from its caption (beats 1, 8, 9, 10),
      so the caption can be changed before the edit.
- [ ] Spares, if there's time: SPEED high (16 readings a second); statistics
      with REL; 18 mF capacitance; AC current on the 10 A range.
