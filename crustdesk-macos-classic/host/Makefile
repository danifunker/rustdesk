# Linux builds of the portable core: tests and the host agent.
#   make -C host          build everything into host/build
#   make -C host test     run the libvpx comparison
CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -g -Wall -Wextra -Wno-missing-field-initializers -DCONFIGURED=1
VPXINC  ?= ../../rustdesk-ppc-agent/vpx-include
LIBVPX  ?= /usr/lib/x86_64-linux-gnu/libvpx.so.9
B       := build
CORE    := ../src/core
NACL    := ../third_party/libsodium-min
NACLSRC := $(shell find $(NACL) -name '*.c')
NACLINC := -I$(NACL)/include -I$(NACL)/include/sodium -I$(CORE) -I../third_party/bearssl/inc \
           -I../third_party/zstd-decoder
CORESRC := $(CORE)/vp8enc.c $(CORE)/pb.c $(CORE)/sha256.c $(CORE)/session.c $(CORE)/yuv.c \
           $(CORE)/macroman.c $(CORE)/rng.c $(CORE)/rdv.c $(CORE)/dns.c $(CORE)/console.c \
           $(CORE)/https.c $(CORE)/anchors.c $(CORE)/png.c $(CORE)/unzstd.c $(B)/zstddeclib.o $(NACLSRC)
BSSL    := ../third_party/bearssl
BSSLSRC := $(shell find $(BSSL)/src -name '*.c')
BSSLOBJ := $(patsubst $(BSSL)/src/%.c,$(B)/bearssl/%.o,$(BSSLSRC))

all: $(B)/test_vp8 $(B)/hostagent $(B)/test_core $(B)/vp8dump $(B)/test_crypto $(B)/consoleprobe

$(B):
	mkdir -p $(B)

$(B)/test_vp8: test_vp8.c $(CORESRC) | $(B)
	$(CC) $(CFLAGS) -I$(VPXINC) -o $@ test_vp8.c $(CORE)/vp8enc.c $(LIBVPX) -lm

# The zstd decoder, with the Mac build's options.
$(B)/zstddeclib.o: ../third_party/zstd-decoder/zstddeclib.c | $(B)
	$(CC) -O2 -w -DHUF_FORCE_DECOMPRESS_X1 -DZSTD_FORCE_DECOMPRESS_SEQUENCES_SHORT \
	    -DZSTD_NO_INLINE -c -o $@ $<

# BearSSL, once, as a library.
$(B)/bearssl/%.o: $(BSSL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -I$(BSSL)/inc -I$(BSSL)/src -c -o $@ $<

$(B)/libbearssl.a: $(BSSLOBJ)
	ar rcs $@ $^

$(B)/hostagent: hostagent.c $(CORESRC) $(B)/libbearssl.a | $(B)
	$(CC) $(CFLAGS) $(NACLINC) -D_POSIX_C_SOURCE=200809L -o $@ hostagent.c $(CORESRC) $(B)/libbearssl.a

$(B)/test_core: test_core.c $(CORESRC) $(B)/libbearssl.a | $(B)
	$(CC) $(CFLAGS) $(NACLINC) -o $@ test_core.c $(CORESRC) $(B)/libbearssl.a

$(B)/consoleprobe: consoleprobe.c $(CORESRC) $(B)/libbearssl.a | $(B)
	$(CC) $(CFLAGS) $(NACLINC) -D_POSIX_C_SOURCE=200809L -o $@ consoleprobe.c $(CORESRC) \
	    $(B)/libbearssl.a

$(B)/test_crypto: test_crypto.c $(CORESRC) | $(B)
	$(CC) $(CFLAGS) -w $(NACLINC) -o $@ test_crypto.c $(CORE)/rng.c $(NACLSRC)

$(B)/vp8dump: vp8dump.c | $(B)
	$(CC) $(CFLAGS) -I$(VPXINC) -o $@ vp8dump.c $(LIBVPX)

test: all
	$(B)/test_core
	python3 test_png.py
	python3 test_crypto.py $(B)/test_crypto
	$(B)/test_vp8 16 | tail -1
	$(B)/test_vp8 0 | tail -1
	SLICE=3 $(B)/test_vp8 16 | tail -1

clean:
	rm -rf $(B)
.PHONY: all test clean
