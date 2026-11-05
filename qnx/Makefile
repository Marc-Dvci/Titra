# QNX 8.0 build of Titra's processes for x86_64 (QEMU), with the SDP's qcc.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make -C qnx            build/x86_64/{spo2d,valved,titractl,titra-launch,station}
#   make -C qnx ARCH=aarch64le
#
# The deterministic core in ../core is compiled into each process from the same sources the host tests cover.

ARCH ?= x86_64
QCC_TARGET_x86_64 = gcc_ntox86_64
QCC_TARGET_aarch64le = gcc_ntoaarch64le
CC = qcc -V$(QCC_TARGET_$(ARCH))
BUILD = build/$(ARCH)
WARN = -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS = -std=gnu11 -O2 -g $(WARN)
CPPFLAGS = -I../core/include -Icommon
CORE = $(filter-out ../core/src/sim.c,$(wildcard ../core/src/*.c))
COMMON = common/link.c common/rm.c common/util.c
PROGRAMS = spo2d valved titractl titra-launch station titra-burn

.PHONY: all clean
all: $(addprefix $(BUILD)/,$(PROGRAMS))

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/spo2d: spo2d/spo2d.c $(COMMON) $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lsocket

$(BUILD)/valved: valved/valved.c $(COMMON) $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lsocket

$(BUILD)/titractl: titractl/titractl.c $(COMMON) $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lsocket -lham

$(BUILD)/titra-launch: launch/titra-launch.c common/util.c $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lham

$(BUILD)/station: station/station.c station/page.h common/util.c $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ station/station.c common/util.c $(CORE) -lm -lsocket

$(BUILD)/titra-burn: burn/titra-burn.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

station/page.h: station/index.html station/embed.py
	python3 station/embed.py station/index.html $@

clean:
	rm -rf build
