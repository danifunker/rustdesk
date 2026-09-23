# C-Desk-Vint for classic Mac OS, built with Retro68.
#
#   cmake -S . -B build-m68k \
#     -DCMAKE_TOOLCHAIN_FILE=$RETRO68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build-m68k
#
# 68020 and up, no FPU instructions: one binary for a Quadra 800, an
# FPU-less 68LC040 and a 68030 alike. PowerPC comes later (see README).

cmake_minimum_required(VERSION 3.13)
project(CDeskVint C ASM)

set(CDV_CPU "68020" CACHE STRING "Target CPU passed to -m (68020, 68030, 68040)")

add_application(C-Desk-Vint
    src/mac/main.c
    src/mac/net.c
    src/mac/screen.c
    src/mac/input.c
    src/mac/engine.c
    src/mac/glue.s
    src/core/session.c
    src/core/pb.c
    src/core/sha256.c
    src/core/yuv.c
    src/core/vp8enc.c
    rsrc/cdeskvint.r
    CREATOR CDVt
    TYPE APPL)

target_compile_options(C-Desk-Vint PRIVATE
    -m${CDV_CPU} -mtune=68040 -msoft-float -O2 -std=gnu99
    -Wall -Wno-multichar -Wno-unused-parameter
    -Wno-array-bounds  # low-memory globals are fixed addresses
    -ffunction-sections)
# One CODE segment: the engine runs at interrupt time, where the Segment
# Loader must never be asked to bring anything in.
target_link_options(C-Desk-Vint PRIVATE -m${CDV_CPU} -msoft-float -Wl,-gc-sections
    -Wl,--mac-single)
