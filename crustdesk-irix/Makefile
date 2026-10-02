# C-Desk-Vint for IRIX: cross-compiled on Linux with GCC for MIPS I / o32,
# against IRIX 5.3's own headers; linked on an IRIX machine by IRIX's ld
# (scripts/indigo.py link), because only SGI's linker makes IRIX executables.
#
#   make                 objects into build/obj (needs the toolchain and sysroot)
#   make toolchain       fetch Ubuntu's mips-linux-gnu GCC 13 into build/toolchain
#   make sysroot         IRIX 5.3 headers from a disk image (IRIX53_IMAGE=...)
#
# See BUILD.md.
TOOLCHAIN ?= $(CURDIR)/build/toolchain
SYSROOT   ?= $(CURDIR)/build/irix53-sysroot
export IRIX_SYSROOT := $(SYSROOT)
export PATH := $(TOOLCHAIN)/usr/bin:$(PATH)
export LD_LIBRARY_PATH := $(TOOLCHAIN)/usr/lib/x86_64-linux-gnu
# IRIX_LDFLAGS=-p links for prof(1)'s pc sampling (mon.out at exit).
IRIX_LDFLAGS ?=
BUILD_ID  ?= $(shell date -u +%Y%m%d)-$(shell git rev-parse --short=9 HEAD 2>/dev/null || echo dev)

CC    := tools/irix-gcc
CORE  := ../crustdesk-macos-classic/src/core
TP    := ../crustdesk-macos-classic/third_party
NACL  := $(TP)/libsodium-min
O     := build/obj
INC   := -Isrc -I$(CORE) -I../crustdesk-macos-classic/host -I$(NACL)/include -I$(NACL)/include/sodium -I$(TP)/zstd-decoder \
         -I$(SYSROOT)/usr/include/X11
CFLAGS := -std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-unused-function \
          -include src/compat.h $(INC) -DCONFIGURED=1 $(EXTRA_CFLAGS)

IRIX_SRC := main screen input clip log compat
CORE_SRC := vp8enc pb sha256 session rng rdv unzstd files macbin
HOST     := ../crustdesk-macos-classic/host
NACL_SRC := $(shell find $(NACL) -name '*.c')

OBJS := $(IRIX_SRC:%=$(O)/%.o) $(CORE_SRC:%=$(O)/core_%.o) $(O)/fs_posix.o \
        $(patsubst %.c,$(O)/nacl_%.o,$(notdir $(NACL_SRC))) $(O)/zstddeclib.o

all: $(OBJS) $(O)/LINK

$(O):
	mkdir -p $(O)

$(O)/main.o: src/main.c src/*.h | $(O)
	$(CC) $(CFLAGS) -DCDV_BUILD='"$(BUILD_ID)"' -c $< -o $@

$(O)/%.o: src/%.c src/*.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/core_%.o: $(CORE)/%.c $(CORE)/*.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/fs_posix.o: $(HOST)/fs_posix.c $(HOST)/fs_posix.h | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

vpath %.c $(sort $(dir $(NACL_SRC)))
$(O)/nacl_%.o: %.c | $(O)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(O)/zstddeclib.o: $(TP)/zstd-decoder/zstddeclib.c | $(O)
	$(CC) -O2 -w -include src/compat.h -DHUF_FORCE_DECOMPRESS_X1 \
	    -DZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT -DZSTD_NO_INLINE -c $< -o $@

# What the IRIX side runs to link: the object list and the libraries.
$(O)/LINK: Makefile | $(O)
	echo "cc $(IRIX_LDFLAGS) -o C-Desk-Vint $(notdir $(OBJS)) -lXTrap -lXt -lXtst -lXext -lX11 -lm" > $@

toolchain:
	sh scripts/fetch-toolchain.sh $(TOOLCHAIN)

sysroot:
	sh scripts/make-sysroot.sh $(SYSROOT)

clean:
	rm -rf $(O)

.PHONY: all toolchain sysroot clean

# The package: the agent as linked on IRIX (scripts/indigo.py link fetches it
# to build/C-Desk-Vint) with its installer, as a tar IRIX 5.3's tar reads.
DISTNAME := C-Desk-Vint-IRIX-5.3+-$(BUILD_ID)
dist: build/C-Desk-Vint
	rm -rf build/dist
	mkdir -p build/dist/C-Desk-Vint-IRIX
	cp build/C-Desk-Vint pkg/install.sh pkg/uninstall.sh pkg/cdeskvint.init pkg/README.txt \
	    build/dist/C-Desk-Vint-IRIX/
	chmod 755 build/dist/C-Desk-Vint-IRIX/C-Desk-Vint build/dist/C-Desk-Vint-IRIX/*.sh \
	    build/dist/C-Desk-Vint-IRIX/cdeskvint.init
	cd build/dist && tar --format=v7 --owner=0 --group=0 -cf ../$(DISTNAME).tar C-Desk-Vint-IRIX
	cd build && sha256sum $(DISTNAME).tar > $(DISTNAME).SHA256SUMS
	@ls -l build/$(DISTNAME).tar

.PHONY: dist
