# Host tests for the pure-arithmetic parts of the firmware.
#
# Nothing here needs Zephyr, a board, or the NCS toolchain: vitals.c is integer
# maths over a buffer, and the one Zephyr header it includes is shimmed. That is
# the point -- a test you can run in a second is a test that gets run, and these
# exist to settle constants (BPM_MIN, the filter corner) that nobody can settle
# by argument.
#
# flash_store.c needs more than a header shim -- a kernel, a logger and a flash
# part -- and it earns them: the ring arithmetic is the riskiest thing in the
# tree and a lap on hardware is 77 minutes of unbroken finger contact.
#
# Usage:
#   make            run everything
#   make vitals     the rate estimator only
#   make flash      the vitals log only
#   make sweep      run the candidate floors side by side
#   make BPM_MIN=25 HP_CORNER=40 vitals

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
INCLUDES := -Ishim -I../../src
LDLIBS := -lm

SRC := test_vitals.c ../../src/vitals.c
FLASH_SRC := test_flash_store.c shim/shim.c ../../src/flash_store.c
OUT := build

# Empty means "whatever vitals.h says", which is what the firmware compiles.
BPM_MIN ?=
HP_CORNER ?=
DEFS :=
ifneq ($(BPM_MIN),)
DEFS += -DVITALS_BPM_MIN=$(BPM_MIN)
endif
ifneq ($(HP_CORNER),)
DEFS += -DVITALS_HP_CORNER_BPM=$(HP_CORNER)
endif
ifneq ($(CONF),)
DEFS += -DVITALS_BPM_CONFIDENCE_MIN=$(CONF)
endif

.PHONY: all run vitals flash sweep clean

all: vitals flash

# `run` is what the vitals tests were always invoked as; kept so anything that
# calls it still works.
run: vitals

vitals: $(OUT)/test_vitals
	@$(OUT)/test_vitals

flash: $(OUT)/test_flash_store
	@$(OUT)/test_flash_store

$(OUT)/test_vitals: $(SRC) | $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEFS) $(SRC) $(LDLIBS) -o $@

$(OUT)/test_flash_store: $(FLASH_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) $(FLASH_SRC) -o $@

$(OUT):
	@mkdir -p $(OUT)

# Does lowering the floor cost accuracy, and does it start inventing pulses out
# of breathing? Run the candidates rather than reasoning about them.
sweep:
	@for floor in 40 35 30 25; do \
		for corner in 40 $$floor; do \
			$(MAKE) -s clean >/dev/null; \
			$(MAKE) -s BPM_MIN=$$floor HP_CORNER=$$corner run \
				| sed -n '4,6p;$$!{/totals\|below the floor\|false pulses\|no pulse/p}'; \
			echo "  ---"; \
		done; \
	done

clean:
	@rm -rf $(OUT)
