# Stock firmware image (not in the repo)

The analysis, several tests and `make setup` need OWON's v2.1.2 application
image. It is OWON's code, so it is not committed. Put it here as:

    firmware/XDM1041_OS_V2.1.2_20210525.bin

SHA-256 `16e2706a0f3160d5efd742969f1defe2131c2886c779f17a996111568da67224`
(`make setup` checks it).

Where to get it:
- [TheHWcave/OWON-XDM1041](https://github.com/TheHWcave/OWON-XDM1041),
  which publishes OWON's updater with this firmware ("V2.1.2", shown by the
  meter as 20210525), split into chunks;
- or OWON support.

Then run `make setup`, which generates `lib/ui/stock_fonts.h` from it.
Without the image, the tests that read it are skipped and the UI does not
build.
