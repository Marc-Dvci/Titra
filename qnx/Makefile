# QNX 8.0 build of Titra's processes for x86_64 (QEMU), with the SDP's qcc.
#
#   source ~/qnx800/qnxsdp-env.sh
#   make -C qnx            build/x86_64/{spo2d,valved,titractl,titra-launch,station,advisor,llmstore}
#   make -C qnx llm        build/x86_64/{llm,llm_bench}: needs llama.cpp built for QNX first (qnx/llm/build.sh)
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
PROGRAMS = spo2d valved titractl titra-launch station titra-burn advisor llmstore
LLAMA_DIR ?= $(HOME)/llama-qnx
LLAMA_BUILD = $(LLAMA_DIR)/build-qnx
LLAMA_LIBS = $(LLAMA_BUILD)/src/libllama.a $(LLAMA_BUILD)/ggml/src/libggml.a $(LLAMA_BUILD)/ggml/src/libggml-cpu.a $(LLAMA_BUILD)/ggml/src/libggml-base.a
CXX = q++ -V$(QCC_TARGET_$(ARCH))_cxx
LLAMA_CXXFLAGS = -std=gnu++17 -O2 -g1 -Wall -I$(LLAMA_DIR)/include -I$(LLAMA_DIR)/ggml/include

.PHONY: all clean llm
all: $(addprefix $(BUILD)/,$(PROGRAMS))
llm: $(BUILD)/llm $(BUILD)/llm_bench

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

$(BUILD)/advisor: advisor/advisor.c $(COMMON) $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm -lsocket

$(BUILD)/llmstore: llm/llmstore.c common/util.c $(CORE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(BUILD)/llm: llm/llm.cpp llm/llm_shared.h ../core/src/advice.c ../core/src/facts.c ../core/src/loops.c ../core/src/risk.c $(LLAMA_LIBS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../core/src/advice.c -o $(BUILD)/llm_advice.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../core/src/facts.c -o $(BUILD)/llm_facts.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../core/src/loops.c -o $(BUILD)/llm_loops.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ../core/src/risk.c -o $(BUILD)/llm_risk.o
	$(CXX) $(CPPFLAGS) $(LLAMA_CXXFLAGS) llm/llm.cpp $(BUILD)/llm_advice.o $(BUILD)/llm_facts.o $(BUILD)/llm_loops.o $(BUILD)/llm_risk.o $(LLAMA_LIBS) -lm -lsocket -lham -o $@

$(BUILD)/llm_bench: llm/llm_bench.cpp $(LLAMA_LIBS) | $(BUILD)
	$(CXX) $(LLAMA_CXXFLAGS) llm/llm_bench.cpp $(LLAMA_LIBS) -lm -lsocket -o $@

$(BUILD)/titra-burn: burn/titra-burn.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

station/page.h: station/index.html station/embed.py
	python3 station/embed.py station/index.html $@

clean:
	rm -rf build
