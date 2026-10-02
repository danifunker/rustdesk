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
