# SPDX-License-Identifier: BSD-3-Clause

project_dir := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
TOP := $(abspath $(project_dir)/../../..)
OUT_DIR ?= $(TOP)/out
CROSS_COMPILE ?= $(TOP)/sdk/toolchain/bin/arm-uclinuxfdpiceabi-
CC := $(CROSS_COMPILE)gcc

CFLAGS ?= -Os -Wall -Wextra -Werror
LDFLAGS += -Wl,--gc-sections -Wl,-rpath,/system/lib

BIN := $(OUT_DIR)/input-events/input-events

.PHONY: all clean
all: $(BIN)

$(BIN): $(project_dir)/input-events.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $@
	@$(CROSS_COMPILE)size $@

clean:
	rm -f $(BIN)
