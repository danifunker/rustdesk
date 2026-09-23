# Linux builds of the portable core: tests and the host agent.
#   make -C host          build everything into host/build
#   make -C host test     run the libvpx comparison
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -g -Wall -Wextra -Wno-missing-field-initializers
VPXINC  ?= ../../rustdesk-ppc-agent/vpx-include
LIBVPX  ?= /usr/lib/x86_64-linux-gnu/libvpx.so.9
CORE    := ../src/core
CORESRC := $(CORE)/vp8enc.c $(CORE)/pb.c $(CORE)/sha256.c $(CORE)/session.c $(CORE)/yuv.c
B       := build

all: $(B)/test_vp8 $(B)/hostagent $(B)/test_core

$(B):
	mkdir -p $(B)

$(B)/test_vp8: test_vp8.c $(CORESRC) | $(B)
	$(CC) $(CFLAGS) -I$(VPXINC) -o $@ test_vp8.c $(CORE)/vp8enc.c $(LIBVPX) -lm

$(B)/hostagent: hostagent.c $(CORESRC) | $(B)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -o $@ hostagent.c $(CORESRC)

$(B)/test_core: test_core.c $(CORESRC) | $(B)
	$(CC) $(CFLAGS) -o $@ test_core.c $(CORESRC)

test: all
	$(B)/test_core
	$(B)/test_vp8 16 | tail -1
	$(B)/test_vp8 0 | tail -1
	SLICE=3 $(B)/test_vp8 16 | tail -1

clean:
	rm -rf $(B)
.PHONY: all test clean
