# Build the hot-swap prototype for QNX 8.0 x86_64.
# Needs the SDP environment:  source ~/qnx800/qnxsdp-env.sh
#
#   make            build everything into build/
#   make deploy     copy the binaries to the VM (ssh alias "qnxvm") in /tmp/hotswap
#   make clean

CC      := qcc -Vgcc_ntox86_64
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
OUT     := build
BINS    := $(OUT)/shell $(OUT)/core_v1 $(OUT)/core_v2 $(OUT)/client \
           $(OUT)/jitter $(OUT)/hello
HDRS    := src/hotswap_proto.h
TARGET  ?= qnxvm
REMOTE  ?= /tmp/hotswap

all: $(BINS)

$(OUT):
	mkdir -p $@

$(OUT)/shell: src/shell.c $(HDRS) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/core_v%: src/core.c $(HDRS) | $(OUT)
	$(CC) $(CFLAGS) -DCORE_VERSION=$* -o $@ $<

$(OUT)/client: src/client.c $(HDRS) | $(OUT)
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
	for f in $(BINS) scripts/demo-swap.sh; do \
	    b=$(REMOTE)/$$(basename $$f); \
	    ssh $(TARGET) "cat > $$b.new && chmod +x $$b.new && mv -f $$b.new $$b" < $$f || exit 1; \
	done

clean:
	rm -rf $(OUT)

.PHONY: all deploy clean
