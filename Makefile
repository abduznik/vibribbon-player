# Developer builds of the C test harness (build/vibrecomp-headless). The player itself is built with cargo.
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wno-unused-function -fno-strict-aliasing -std=gnu11 -Iruntime
GENFLAGS ?= -O2 -g0 -w -fno-strict-aliasing -std=gnu11 -Iruntime
RT_SRC  := runtime/mem.c runtime/cpu.c runtime/gte.c runtime/gpu.c runtime/irq.c runtime/timers.c runtime/dma.c \
           runtime/cdrom.c runtime/disc.c runtime/spu.c runtime/sio.c runtime/kernel.c runtime/guest.c runtime/biosnames.c runtime/audio_in.c runtime/autoplay.c runtime/ctrl.c runtime/runtime.c
GEN_SRC := $(wildcard gen/*.c)
BUILD   := build
RT_OBJ  := $(RT_SRC:%.c=$(BUILD)/obj/%.o)
GEN_OBJ := $(GEN_SRC:%.c=$(BUILD)/obj/%.o)
HDRS    := runtime/psx.h runtime/ops.h runtime/recomp_rt.h

# game executables recompiled by `make recomp` (needs the extracted disc files in work/disc)
RECOMP_EXE := MAIN_G.EXE

all: $(BUILD)/vibrecomp-headless

$(BUILD)/obj/gen/%.o: gen/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c -o $@ $<

$(BUILD)/obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/vibrecomp-headless: $(RT_OBJ) $(BUILD)/obj/runtime/main_headless.o $(GEN_OBJ)
	$(CC) -o $@ $^ -lm

# regenerate gen/*.c from the game executables, then run `make -j` again
recomp:
	rm -f gen/*.c
	python3 tools/recomp.py work/disc/MAIN_G.EXE --name MAIN_G.EXE --entries work/trace/entries_G.txt --patched work/trace/writes_G.txt -v

clean:
	rm -rf $(BUILD)

.PHONY: all clean recomp
