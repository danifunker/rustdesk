# C-Desk-Vint for classic Mac OS, built with Retro68.
#
#   cmake -S . -B build-m68k \
#     -DCMAKE_TOOLCHAIN_FILE=$RETRO68/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
#   cmake --build build-m68k
#
# 68020 and up, no FPU instructions: one binary for a Quadra 800, an
# FPU-less 68LC040 and a 68030 alike. The same file builds for PowerPC with
# the retroppc toolchain (-B build-ppc); scripts/make-fat.sh joins the two.

cmake_minimum_required(VERSION 3.13)
project(CDeskVint C ASM)

set(CDV_CPU "68020" CACHE STRING "Target CPU passed to -m (68020, 68030, 68040)")
option(CDV_PROFILE "Log where the encoder's time goes, per phase" OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Retro68")
    set(CDV_68K ON)
    set(CDV_ARCH_SOURCES src/mac/glue.s)
else()
    set(CDV_68K OFF)
    set(CDV_ARCH_SOURCES)
endif()

# libsodium 1.0.18, the parts RustDesk's crypto uses (ISC; see its LICENSE).
file(GLOB_RECURSE CDV_NACL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium-min/*.c)

add_application(C-Desk-Vint
    src/mac/main.c
    src/mac/net.c
    src/mac/screen.c
    src/mac/input.c
    src/mac/engine.c
    src/mac/traps.c
    src/mac/cursor.c
    src/mac/mem.c
    src/mac/dnr.c
    ${CDV_ARCH_SOURCES}
    src/core/session.c
    src/core/pb.c
    src/core/sha256.c
    src/core/yuv.c
    src/core/macroman.c
    src/core/rng.c
    src/core/rdv.c
    src/core/dns.c
    ${CDV_NACL_SOURCES}
    src/core/vp8enc.c
    rsrc/cdeskvint.r
    CREATOR CDVt
    TYPE APPL)

if(CDV_PROFILE)
    target_compile_definitions(C-Desk-Vint PRIVATE VP8E_PROFILE)
endif()
target_include_directories(C-Desk-Vint PRIVATE
    third_party/libsodium-min/include
    third_party/libsodium-min/include/sodium
    src/core)
target_compile_definitions(C-Desk-Vint PRIVATE CONFIGURED=1)
target_compile_options(C-Desk-Vint PRIVATE
    -O2 -std=gnu99
    -Wall -Wno-multichar -Wno-unused-parameter
    -Wno-array-bounds  # low-memory globals are fixed addresses
    -ffunction-sections)
if(CDV_68K)
    target_compile_options(C-Desk-Vint PRIVATE -m${CDV_CPU} -mtune=68040 -msoft-float)
    # One CODE segment: the engine runs at interrupt time, where the Segment
    # Loader must never be asked to bring anything in.
    target_link_options(C-Desk-Vint PRIVATE -m${CDV_CPU} -msoft-float -Wl,-gc-sections
        -Wl,--mac-single)
    # Elf2Mac lays .data straight after .text, but the startup code finds
    # the data and the relocation table at their linked addresses: any gap
    # between the two (an over-aligned .data) and every global, and every
    # relocation, lands a few bytes off -- a crash before main. libsodium
    # aligns two tables to 16 for SIMD, which a 68k has none of.
    target_compile_options(C-Desk-Vint PRIVATE "-DCRYPTO_ALIGN(x)=")
    add_custom_command(TARGET C-Desk-Vint POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} -h $<TARGET_FILE:C-Desk-Vint>.gdb > sections.txt
        COMMAND ${CMAKE_COMMAND} -DSECTIONS=sections.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check-sections.cmake
        VERBATIM)
else()
    target_compile_options(C-Desk-Vint PRIVATE -mcpu=750)
    target_link_options(C-Desk-Vint PRIVATE -Wl,-gc-sections)
endif()

# An ID server other than the public one, as the default for a fresh prefs file.
set(CDV_SERVER "" CACHE STRING "default ID server, host[:port]")
set(CDV_KEY "" CACHE STRING "that server's public key")
if(CDV_SERVER)
    target_compile_definitions(C-Desk-Vint PRIVATE DEFAULT_SERVER="${CDV_SERVER}" DEFAULT_KEY="${CDV_KEY}")
endif()
