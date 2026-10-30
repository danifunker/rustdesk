# C-Desk-Vint for Windows NT, from this (Linux) side.
#
# The program is compiled on an NT machine by the Visual C++ there (the
# compiler that targets NT on MIPS, Alpha and PowerPC is the one that ran on
# them); this machine stages the sources and drives the build over
# tools/nt/ntexec.c (see BUILD.md).
#
#   make stage           build/stage: the tree the NT side compiles
#   make nt              stage, send what changed, nmake there, fetch the .exe
#                        to build/<ARCH>/C-Desk-Vint.exe (NTX=host:port, ARCH=ppc)
#   make install-nt      nt, then put it in place of the installed service's
#                        program (NTINST) and restart the service
#   make selftest-host   the selftest built by GCC: the values -selftest expects
#   make selftest-nt     run -selftest on the NT machine
#   make dist            the release zip for ARCH
#   make icon            res/cdv.ico from tools/make-icon.py
ARCH     ?= ppc
NTDIR    ?= c:\cdv\src
NTINST   ?= C:\Program Files\C-Desk-Vint
BUILD_ID ?= $(shell date -u +%Y%m%d)-$(shell git rev-parse --short=9 HEAD 2>/dev/null || echo dev)
NTX      := python3 scripts/ntx.py

MAC  := ../crustdesk-macos-classic
CORE := $(MAC)/src/core
NACL := $(MAC)/third_party/libsodium-min

stage:
	BUILD_ID=$(BUILD_ID) python3 scripts/stage.py build/stage

nt: stage
	$(NTX) sync build/stage '$(NTDIR)'
	$(NTX) run 'cd /d $(NTDIR) && nmake /nologo'
	mkdir -p build/$(ARCH)
	$(NTX) get '$(NTDIR)\C-Desk-Vint.exe' build/$(ARCH)/C-Desk-Vint.exe
	@ls -l build/$(ARCH)/C-Desk-Vint.exe

install-nt: nt
	$(NTX) run '"$(NTINST)\C-Desk-Vint.exe" -stop'
	$(NTX) run 'copy $(NTDIR)\C-Desk-Vint.exe "$(NTINST)"'
	$(NTX) run '"$(NTINST)\C-Desk-Vint.exe" -start'

selftest-nt:
	$(NTX) run '$(NTDIR)\C-Desk-Vint.exe -selftest'

build/selftest-host: src/selftest.c $(CORE)/*.c $(CORE)/*.h
	mkdir -p build
	cc -std=gnu99 -O2 -w -DCONFIGURED=1 -DSELFTEST_MAIN -Isrc -I$(CORE) -I$(NACL)/include \
	    -I$(NACL)/include/sodium -I$(MAC)/third_party/zstd-decoder -o $@ src/selftest.c \
	    $(CORE)/sha256.c $(CORE)/png.c $(CORE)/vp8enc.c $(CORE)/unzstd.c $(CORE)/rng.c \
	    $(MAC)/third_party/zstd-decoder/zstddeclib.c $$(find $(NACL) -name '*.c')

selftest-host: build/selftest-host
	build/selftest-host

# The release: the program alone (NT 4.0 has no unzip) and a zip with the
# read-me, named for the processor, both checked for anything personal.
ARCHNAME := $(if $(filter ppc,$(ARCH)),PPC,$(if $(filter mips,$(ARCH)),MIPS,$(if $(filter alpha,$(ARCH)),Alpha,x86)))
DISTNAME := C-Desk-Vint-WinNT4-$(ARCHNAME)-$(BUILD_ID)
dist: build/$(ARCH)/C-Desk-Vint.exe
	@grep -qa 'C-Desk-Vint for Windows NT' $< || { echo "$< is not the agent"; exit 1; }
	@grep -qa '$(BUILD_ID)' $< || { echo "$< is not build $(BUILD_ID): make nt first"; exit 1; }
	sh scripts/check-generic.sh $<
	rm -rf build/dist && mkdir -p build/dist/C-Desk-Vint
	cp $< build/dist/C-Desk-Vint/C-Desk-Vint.exe
	sed 's/$$/\r/' pkg/README.txt > build/dist/C-Desk-Vint/README.txt
	cd build/dist && zip -q -X -r ../$(DISTNAME).zip C-Desk-Vint
	cp $< build/$(DISTNAME).exe
	cd build && sha256sum $(DISTNAME).zip $(DISTNAME).exe > $(DISTNAME).SHA256SUMS
	@cat build/$(DISTNAME).SHA256SUMS

icon:
	python3 tools/make-icon.py res/cdv.ico

clean:
	rm -rf build

.PHONY: stage nt install-nt selftest-nt selftest-host dist icon clean
