# Test data from outside this project

| File | What | Source | Licence |
| --- | --- | --- | --- |
| `xdm2041_calibration.bin` | A real 2 KB factory record (last flash page) from an OWON XDM2041, identity `XDM2041` / `V1.7.2`. Same HY3131 platform and record layout; the EEVblog thread reports restoring an XDM1041 with it. | [PetteriAimonen/owon-xdm2041-info](https://github.com/PetteriAimonen/owon-xdm2041-info), `bootloader/XDM2041_calibration.bin` | CC0-1.0 |

It is the first real record our calibration parser has seen: `meas_cal_validate()`
and `serial-dump.py` both find 0 implausible slots, and the cold-junction gain
is 1701 (our default, from the v2.1.2 image, is 1694).
