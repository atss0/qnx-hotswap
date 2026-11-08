# Build the hot-swap prototype for QNX 8.0 x86_64.
# Needs the SDP environment:  source ~/qnx800/qnxsdp-env.sh
#
#   make            build everything into build/
#   make deploy     copy the binaries to the VM (ssh alias "qnxvm") in /tmp/hotswap
#   make clean
#
# Every driver (counter.c, accel.c) is built in several variants from the
# same source:
#   <drv>_v1, <drv>_v2   swappable cores (core_main.c), versions 1 and 2
#   <drv>_v2_bad         v2 core that crashes on its first read (rollback demo)
#   <drv>_v2_late        v2 core that crashes on its 500th read
#   <drv>_v2_hang        v2 core that spins forever on its first read
#   <drv>_mono_v1, _v2   the same driver as a conventional single-process
#                        resource manager (mono_main.c): the restart baseline

CC      := qcc -Vgcc_ntox86_64
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
LDLIBS  := -lm
OUT     := build
DRIVERS := counter accel
VARIANTS:= _v1 _v2 _v2_bad _v2_late _v2_hang _mono_v1 _mono_v2

BINS    := $(OUT)/shell \
           $(foreach d,$(DRIVERS),$(foreach v,$(VARIANTS),$(OUT)/$(d)$(v))) \
           $(OUT)/counter_client $(OUT)/accel_client $(OUT)/rogue $(OUT)/ctl_test \
           $(OUT)/jitter $(OUT)/hello
CORE_HDRS := src/hotswap_proto.h src/driver.h src/counter.h src/accel.h
TARGET  ?= qnxvm
REMOTE  ?= /tmp/hotswap

all: $(BINS)

$(OUT):
	mkdir -p $@

$(OUT)/shell: src/shell.c src/hotswap_proto.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

# Driver variants. $(1) = driver name.
define driver_rules
$(OUT)/$(1)_v1: src/core_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=1 -o $$@ src/core_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_v2: src/core_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=2 -o $$@ src/core_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_v2_bad: src/core_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=2 -DCRASH_AFTER=1 -o $$@ src/core_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_v2_late: src/core_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=2 -DCRASH_AFTER=500 -o $$@ src/core_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_v2_hang: src/core_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=2 -DHANG_ON_READ -o $$@ src/core_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_mono_v1: src/mono_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=1 -o $$@ src/mono_main.c src/$(1).c $(LDLIBS)
$(OUT)/$(1)_mono_v2: src/mono_main.c src/$(1).c $(CORE_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DDRV_VERSION=2 -o $$@ src/mono_main.c src/$(1).c $(LDLIBS)
endef
$(foreach d,$(DRIVERS),$(eval $(call driver_rules,$(d))))

$(OUT)/counter_client: src/client.c src/counter.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/accel_client: src/accel_client.c src/accel.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/rogue: src/rogue.c src/hotswap_proto.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/ctl_test: src/ctl_test.c src/counter.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/jitter: examples/jitter/jitter.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/hello: examples/hello/hello.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

# scp hangs against the quickstart image, so stream each file through ssh.
# Write to a temporary name and rename: a running binary cannot be
# overwritten in place ("Resource busy"), but it can be replaced.
deploy: all
	ssh $(TARGET) 'mkdir -p $(REMOTE)'
	for f in $(BINS) scripts/demo-swap.sh scripts/run-series.sh; do \
	    b=$(REMOTE)/$$(basename $$f); \
	    ssh $(TARGET) "cat > $$b.new && chmod +x $$b.new && mv -f $$b.new $$b" < $$f || exit 1; \
	done

clean:
	rm -rf $(OUT)

.PHONY: all deploy clean
