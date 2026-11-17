# OpenOWON documentation

## Using the firmware

| Document | What's in it |
| --- | --- |
| [install.md](install.md) | **installing over USB serial**, through OWON's bootloader; why there's very likely no going back |
| [features.md](features.md) | keys, screens, menus, recording, what the meter remembers |
| [video-plan.md](video-plan.md) | the highlight reel: beats, captions and the measurement behind each, storyboard, filming |
| [scpi.md](scpi.md) | remote-control commands and reply formats |
| [backlog.md](backlog.md) | what's coming, with feasibility and priority (hardware mods included) |

## Developing

| Document | What's in it |
| --- | --- |
| [developer-guide.md](developer-guide.md) | **start here**: setup, the build-and-test loop, architecture, adding a feature |
| [app.md](app.md) | the application's design: pipeline, autoranging, recording, persistence, the simulator |
| [boot.md](boot.md) | flash map, clock, boot, the HAL and its pin gating, the three target builds |
| [user-calibration.md](user-calibration.md) | proposal (B31): owner calibration on top of the factory record |
| [test-plan.md](test-plan.md) | **bench validation**: every function and feature against reference parts, pass limits, `bench-check.py` |
| [bench.md](bench.md) | the debug-probe runbook, and what it found: our meter is readout-protected |
| [ui-development.md](ui-development.md) | designing screens on the host |
| [power-controller.md](power-controller.md) | plan: our own ATtiny202 firmware (short-press power-on, battery, off-drain) |
| [replacement-plan.md](replacement-plan.md) | milestones and acceptance criteria |
| [wishlist.md](wishlist.md) | feasibility reasoning behind the backlog |

## Reference: how the stock meter works

How the original firmware and hardware were taken apart. Developers need
this when touching measurement, drivers or storage; users don't.

| Document | What's in it |
| --- | --- |
| [bootloader.md](bootloader.md) | OWON's bootloader, read off our meter: panel init, update erase range, start-up |
| [community-research.md](community-research.md) | what other owners found: hardware versions, bootloader protocol, hidden menus, prior art |
| [hardware.md](hardware.md) | the boards, pin map, open questions |
| [measurement.md](measurement.md) | every function, from register to displayed value |
| [calibration.md](calibration.md) | the calibration record and how it is applied |
| [analog-frontend.md](analog-frontend.md) | the HY3131: registers, bus, isolation |
| [afe-table.md](afe-table.md) | **the per-range HY3131 + relay table**, recovered by emulating v2.1.2, checked against an XDM2041 capture |
| [display.md](display.md) | the panel and its bus |
| [settings-storage.md](settings-storage.md) | stock's three flash records |
| [settings-dump.md](settings-dump.md) | reading the factory record over serial |
| [firmware-analysis.md](firmware-analysis.md) | the stock image: layout, compressed `.data`, SCPI, fonts |
| [data-tables.md](data-tables.md) | tables recovered from the image |
| [feasibility.md](feasibility.md) | what a replacement can and can't achieve |
| [resources.md](resources.md) | other projects and approaches |
| [modifying.md](modifying.md) | patching stock in place (not the route taken) |
| [readout-protection.md](readout-protection.md) | our meter is readout-protected: what that blocks, what is recoverable, why we install over serial |

Files not in the repository: `manual/` and `datasheets/` (their READMEs say
where to get them). `photos/` has the board photographs and `screens/` the
simulator screenshots.
