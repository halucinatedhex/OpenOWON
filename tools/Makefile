# Host-side UI preview. No hardware, no PlatformIO.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
all: ../build/uidump
	@mkdir -p ../build && cd .. && ./build/uidump
../build/uidump: uidump.c ../lib/ui/ui.c ../lib/ui/ui.h
	@mkdir -p ../build
	$(CC) $(CFLAGS) uidump.c ../lib/ui/ui.c -lm -o $@
# The whole meter on the host: app + simulated HY3131 + simulated panel.
SIM_SRC = meter-sim.c ../lib/app/app.c ../lib/app/acquire.c ../lib/app/readout.c ../lib/app/sound.c ../lib/app/ucal.c \
          ../lib/app/settings.c ../lib/app/menu.c ../lib/app/app_scpi.c ../lib/app/clock.c ../lib/scpi/scpi.c \
          ../lib/store/store.c ../lib/datalog/datalog.c ../lib/sim/sim_flash.c \
          ../lib/sim/sim_board.c ../lib/sim/sim_hy3131.c ../lib/sim/sim_lcd.c \
          ../lib/measure/meas.c ../lib/measure/meas_temp.c ../lib/ui/ui.c \
          ../lib/afe/hy3131.c ../lib/display/lcd.c
SIM_INC = -I../lib/app -I../lib/sim -I../lib/store -I../lib/datalog -I../lib/scpi -I../lib/measure -I../lib/ui -I../lib/afe -I../lib/display
../build/meter-sim: $(SIM_SRC) $(wildcard ../lib/*/*.h)
	@mkdir -p ../build
	$(CC) $(CFLAGS) $(SIM_INC) $(SIM_SRC) -lm -o $@
sim: ../build/meter-sim
clean:
	rm -f ../build/uidump ../build/meter-sim ../build/ui-*.bmp ../build/sim-*.bmp
.PHONY: all sim clean
