# Capacitance sweep, 2026-11-13 (panel build, `DIAG:CAPX <r> 24=01`)

Owner's parts, values from an LCR meter (not precision parts or meter:
electrolytics especially are only good to several percent at DC).
One file per part, raw `DIAG:CAPX` log lines; `open.txt` is leads open.
`cap.py` took the readings, `fit.py` tabulates them (`fit-output.txt`).
Findings: `docs/afe-table.md`, session 2026-11-13.

Parts: 7.55 nF, 48.36 nF, 114.4 nF, 431.4 nF, 1.036 uF, 3.68 uF, 10.4 uF,
22.7 uF, 45.9 uF, 96.68 uF, 263.8 uF.
