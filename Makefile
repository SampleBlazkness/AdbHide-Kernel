# adbng - SPDX-License-Identifier: MIT
#
# Local (host != target) convenience build.  The release artifact is built with
# the Android NDK / bionic in CI; this Makefile produces a static glibc binary
# that runs on Android as-is, which is what we use for in-device protocol tests.

CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -I src -D_GNU_SOURCE
LDFLAGS ?= -static

BUILD  := build
SRCS_D := src/daemon.c src/shell.c src/util.c src/rsa.c src/keys.c
SRCS_T := tests/host_tester.c src/util.c src/rsa.c

.PHONY: all clean test key

all: $(BUILD)/usbgwd $(BUILD)/host_tester

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/usbgwd: $(SRCS_D) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS_D) $(LDFLAGS)

$(BUILD)/host_tester: $(SRCS_T) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS_T) $(LDFLAGS)

key:
	python3 tests/genkey.py $(BUILD)

# protocol self test, no USB required
test: all key
	$(BUILD)/host_tester $(BUILD)/usbgwd $(BUILD)/adb_keys $(BUILD)/hostkey.txt
test-bad: all key
	$(BUILD)/host_tester $(BUILD)/usbgwd $(BUILD)/adb_keys $(BUILD)/hostkey.txt bad

clean:
	rm -rf $(BUILD)