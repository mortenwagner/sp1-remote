# Host unit tests for the Zephyr-free logic modules. Each test binary links
# only the pure sources it needs; nothing here may pull in a Zephyr header.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -g -I../../firmware/src -I.
BUILD   := build
SRC     := ../../firmware/src

SRC_smoke    :=
SRC_controls := $(SRC)/controls.c
SRC_txqueue  := $(SRC)/txqueue.c
SRC_profile  := $(SRC)/profile.c
SRC_buttons  := $(SRC)/buttons.c $(SRC)/controls.c $(SRC)/profile.c
SRC_presets  := $(SRC)/presets.c $(SRC)/buttons.c $(SRC)/controls.c $(SRC)/profile.c

TESTS := smoke controls txqueue profile buttons presets

.PHONY: all test clean
all: test

test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do echo "== $$t"; $(BUILD)/$$t || exit 1; done

$(BUILD)/%: test_%.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SRC_$*)

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
