# Host build of the deterministic core: a shared library for the bench and the C unit tests.
# The QNX processes compile the same sources with qcc (see qnx/).
#
#   make            library and tests
#   make test       run the C unit tests
#   make coverage   run them instrumented and print line and branch coverage

CC ?= gcc
BUILD ?= build
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wformat=2 -Werror
CFLAGS ?= -std=c11 -O2 -g $(WARN)
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
SRCS = $(wildcard src/*.c)
TESTS = $(wildcard tests/test_*.c)

.PHONY: all test coverage clean

all: $(BUILD)/libtitra_core.so $(BUILD)/test_core

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/libtitra_core.so: $(SRCS) $(wildcard include/titra/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -o $@ $(SRCS) -lm

$(BUILD)/test_core: $(SRCS) $(TESTS) tests/check.h tests/main.c $(wildcard include/titra/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests -o $@ $(SRCS) $(TESTS) tests/main.c -lm

test: $(BUILD)/test_core
	$(BUILD)/test_core

coverage: | $(BUILD)
	rm -rf $(BUILD)/cov && mkdir -p $(BUILD)/cov
	for f in $(SRCS) $(TESTS) tests/main.c; do \
	  $(CC) $(CPPFLAGS) -std=c11 -O0 -g --coverage $(WARN) -Itests -c $$f -o $(BUILD)/cov/$$(basename $$f .c).o || exit 1; \
	done
	$(CC) --coverage -o $(BUILD)/cov/test_core $(BUILD)/cov/*.o -lm
	$(BUILD)/cov/test_core > $(BUILD)/cov/tests.txt
	@tail -n 1 $(BUILD)/cov/tests.txt
	@gcov -b -n -o $(BUILD)/cov $(SRCS) | grep -A4 "^File 'src/" | grep -E "^File|Lines executed|Taken at least once"

clean:
	rm -rf $(BUILD)
