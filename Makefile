# One entry point. `make` on its own lists the targets.
.DEFAULT_GOAL := help

# Third-party files are not in the repo (README.md, "Files you supply").
STOCK_IMAGE  = firmware/XDM1041_OS_V2.1.2_20210525.bin
STOCK_SHA256 = 16e2706a0f3160d5efd742969f1defe2131c2886c779f17a996111568da67224
FONTS_H      = lib/ui/stock_fonts.h

help:                ## show this list
	@grep -hE '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed 's/:.*##/\t/' | expand -t22

setup: $(FONTS_H)     ## once after cloning: check the stock image, generate what derives from it
	@echo "setup done: $(FONTS_H) generated from $(STOCK_IMAGE)"

$(FONTS_H): $(STOCK_IMAGE)
	@echo "$(STOCK_SHA256)  $(STOCK_IMAGE)" | shasum -a 256 -c --quiet - \
	 || { echo "$(STOCK_IMAGE) is not the expected v2.1.2 image (firmware/README.md)"; exit 1; }
	@python3 scripts/extract-fonts.py $(STOCK_IMAGE) --emit $@ >/dev/null
	@echo "generated $@"

$(STOCK_IMAGE):
	@echo "Missing $@ - download it as described in firmware/README.md, then run make setup"; exit 1

ui: $(FONTS_H)                  ## render every screen x theme to build/*.bmp, then open them
	@$(MAKE) -s -C tools
	@cd build && for f in ui-*.bmp; do sips -s format png "$$f" --out "$${f%.bmp}.png" >/dev/null 2>&1; done
	@echo "PNGs in build/ - open with: open build/ui-*.png"

sim: $(FONTS_H)                 ## run the whole meter on the host: scenarios/*.sim, screenshots in build/
	@$(MAKE) -s -C tools sim
	@fail=0; for f in scenarios/*.sim; do echo "$$f"; ./build/meter-sim $$f || fail=1; done; \
	 (cd build && for f in sim-*.bmp; do sips -s format png "$$f" --out "$${f%.bmp}.png" >/dev/null 2>&1; done); \
	 exit $$fail

# The README's screenshots, from the simulator run (sim-NN -> docs/screens).
SCREENS = 01-dcv:measure 12-graph:graph 15-record:record 11-stats:stats 14-utility:utility 16-dual:dual \
          30-bargraph-dcv:bargraph 33-autohold:autohold 34-beep-settings:beeper \
          35-browse-recording:log-browse 36-histogram:histogram 10-thermocouple:thermocouple \
          reel-04-diode-led:diode reel-13-theme-dark:theme-dark reel-14-theme-amber:theme-amber \
          reel-15-theme-paper:theme-paper reel-15s-theme-synthwave:theme-synthwave reel-16-theme-stock:theme-stock \
          reel-17-mini-trend:mini-trend reel-18-mini-hist:mini-hist 39-power:power
screens: sim                    ## refresh docs/screens/*.png (the README's pictures) from the simulator
	@for s in $(SCREENS); do cp "build/sim-$${s%%:*}.png" "docs/screens/$${s##*:}.png"; done
	@echo "docs/screens/ refreshed: $(foreach s,$(SCREENS),$(lastword $(subst :, ,$(s))))"

storyboard: sim                 ## the highlight reel's storyboard and 9:16 frame guide (docs/video/)
	@python3 scripts/storyboard.py

test: $(FONTS_H)                ## host unit tests, no hardware
	pio test -e native

# The layers that will run on the meter, compiled for the Cortex-M4 with
# warnings as errors. Catches host-only code creeping into portable layers.
ARM_CC  = $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc
PORTABLE = lib/app/app.c lib/app/acquire.c lib/app/sound.c lib/app/ucal.c lib/app/readout.c lib/app/settings.c lib/app/menu.c lib/datalog/datalog.c \
           lib/app/app_scpi.c lib/app/clock.c lib/scpi/scpi.c \
           lib/store/store.c lib/measure/meas.c \
           lib/measure/meas_temp.c lib/ui/ui.c lib/afe/hy3131.c lib/display/lcd.c \
           lib/console/flashdump.c lib/keypad/keypad.c
portable: $(FONTS_H)            ## compile the on-meter layers for Cortex-M4, -Werror
	@test -x $(ARM_CC) || { echo "fetching the ARM toolchain (first run on this machine)"; pio pkg install -e xdm1241 >/dev/null; }
	@mkdir -p build/portable
	@for f in $(PORTABLE); do \
	  $(ARM_CC) -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -std=c11 \
	    -Wall -Wextra -Werror -Ilib/app -Ilib/store -Ilib/datalog -Ilib/scpi -Ilib/measure -Ilib/ui -Ilib/afe -Ilib/display -Ilib/console -Ilib/keypad \
	    -c $$f -o build/portable/$$(basename $$f .c).o || exit 1; done
	@echo "portable layers build clean for Cortex-M4"

fw-all: $(FONTS_H)              ## build the target images (pin-free, serial console, panel test, UI with simulated AFE, dumper, bench)
	pio run -e xdm1241 -e xdm1241-serial -e xdm1241-panel -e xdm1241-ui -e xdm1241-dumper -e xdm1241-bench

test-scripts:        ## tests for the analysis and bench scripts
	@for t in scripts/test_*.py; do python3 $$t || exit 1; done

check: test fw-all test-scripts sim portable  ## everything that runs without hardware

fw: $(FONTS_H)                  ## build the target firmware
	pio run

triage:              ## analyse the stock firmware image
	python3 scripts/fw-triage.py firmware/XDM1041_OS_V2.1.2_20210525.bin

probe:               ## identify probe + target, show backup status
	./scripts/probe-info.sh

clean:               ## remove build artefacts
	rm -rf build .pio

.PHONY: help ui sim screens storyboard test test-scripts portable fw-all check fw triage probe clean
