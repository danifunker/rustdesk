# The encoder benchmark: cross-compiled like the agent, linked on IRIX with
# -p for prof(1). NOINLINE=1 keeps the encoder's phases apart in the profile.
CC   := ../tools/irix-gcc
CORE := ../../crustdesk-macos-classic/src/core
O    := ../build/bench
FLAGS := -std=gnu99 -O2 -include ../src/compat.h -I$(CORE) -I../build/irix53-sysroot/usr/include/X11
ifeq ($(NOINLINE),1)
FLAGS += -fno-inline-small-functions -fno-inline-functions-called-once -fno-inline-functions
endif
all: $(O)/encbench.o $(O)/vp8enc.o $(O)/yuv.o $(O)/compat.o $(O)/LINK
$(O):
	mkdir -p $(O)
$(O)/encbench.o: encbench.c | $(O)
	$(CC) $(FLAGS) -c $< -o $@
$(O)/%.o: $(CORE)/%.c | $(O)
	$(CC) $(FLAGS) -c $< -o $@
$(O)/compat.o: ../src/compat.c | $(O)
	$(CC) $(FLAGS) -c $< -o $@
$(O)/LINK: | $(O)
	echo "cc -p -o encbench encbench.o vp8enc.o yuv.o compat.o -lX11" > $@
clean:
	rm -rf $(O)

# The TLS benchmark: BearSSL's public-key operations, timed on the machine.
# Linked against the agent's BearSSL archive (make; scripts/indigo.py link):
#   make -C bench tls; scripts/indigo.py linkobj build/tlsbench /usr/tmp/cdv-tlsbench
TO := ../build/tlsbench
tls: $(TO)/tlsbench.o $(TO)/compat.o $(TO)/LINK
$(TO):
	mkdir -p $(TO)
$(TO)/tlsbench.o: tlsbench.c | $(TO)
	$(CC) -std=gnu99 -O2 -include ../src/compat.h -I../../crustdesk-macos-classic/third_party/bearssl/inc -c $< -o $@
$(TO)/compat.o: ../src/compat.c | $(TO)
	$(CC) $(FLAGS) -c $< -o $@
$(TO)/LINK: | $(TO)
	echo "cc -o tlsbench tlsbench.o compat.o /usr/tmp/cdv-build/libbssl.a" > $@
.PHONY: tls
