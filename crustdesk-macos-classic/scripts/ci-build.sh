#!/usr/bin/env bash
# Build, package and check the GENERIC C-Desk-Vint: what the GitHub workflow
# (.github/workflows/macos-classic-agent-build.yaml) does, runnable here.
#
#   scripts/ci-build.sh [compile|package|check|all]      (default: all)
#
#   compile   both halves, configured from nothing with no server, key or
#             console (the public ID server), into $OUT/build-{m68k,ppc}
#   package   scripts/make-disk.sh into $DIST: C-Desk-Vint.hda (for a
#             BlueSCSI), C-Desk-Vint.sit.hqx (for a Mac on the network),
#             BUILD-INFO.txt and SHA256SUMS
#   check     fails if anything personal is in the build: the configuration
#             must name no server, key or console, and no built file may hold
#             a forbidden string -- "dani.tech", and one per line of
#             $CDV_PRIVATE_STRINGS (the workflow fills it from a secret of the
#             same name; here, e.g. the key from ~/.config/rustdesk/RustDesk2.toml).
#             Both halves must still hold the public server: that proves the
#             scan sees strings the 68k compiler splits into move.l immediates
#             (tools/find-strings.py), which a plain grep does not.
#
# RETRO68 is the toolchain directory: default /Retro68-build/toolchain (in
# Retro68's container image, which is how the workflow compiles), else
# ~/repos/Retro68-build/toolchain. OUT defaults to build-ci, DIST to dist.
# package needs rb-cli on the PATH (see make-disk.sh); compile does not.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=${OUT:-$here/build-ci}
dist=${DIST:-$here/dist}
step=${1:-all}

compile() {
    local t=${RETRO68:-}
    if [ -z "$t" ]; then
        t=/Retro68-build/toolchain
        [ -d "$t" ] || t=$HOME/repos/Retro68-build/toolchain
    fi
    rm -rf "$out/build-m68k" "$out/build-ppc"
    # The three named empty on purpose: this is the build that may be shared.
    local generic=(-DCDV_SERVER= -DCDV_KEY= -DCDV_API=)
    cmake -S "$here" -B "$out/build-m68k" "${generic[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$t/m68k-apple-macos/cmake/retro68.toolchain.cmake"
    cmake --build "$out/build-m68k" -j"$(nproc)"
    cmake -S "$here" -B "$out/build-ppc" "${generic[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$t/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
    cmake --build "$out/build-ppc" -j"$(nproc)"
    "$t/bin/m68k-apple-macos-gcc" --version | head -1 > "$out/compiler.txt"
}

package() {
    rm -rf "$dist"
    mkdir -p "$dist"
    "$here/scripts/make-disk.sh" -b "$out" -o "$dist/C-Desk-Vint.hda" -z "$dist/C-Desk-Vint.sit.hqx"
    {
        echo "C-Desk-Vint, generic build (public ID server rs-ny.rustdesk.com, no console)"
        echo "commit:   $(git -C "$here" rev-parse HEAD 2>/dev/null || echo unknown)"
        echo "compiler: $(cat "$out/compiler.txt" 2>/dev/null || echo unknown)"
        echo "rb-cli:   $(rb-cli --version 2>/dev/null | head -1)"
        echo "built:    $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$dist/BUILD-INFO.txt"
    (cd "$dist" && sha256sum C-Desk-Vint.hda C-Desk-Vint.sit.hqx BUILD-INFO.txt > SHA256SUMS)
    cat "$dist/BUILD-INFO.txt" "$dist/SHA256SUMS"
}

check() {
    local b68=$out/build-m68k bppc=$out/build-ppc var
    for var in CDV_SERVER CDV_KEY CDV_API; do
        for d in "$b68" "$bppc"; do
            if grep -q "^$var:[A-Z]*=." "$d/CMakeCache.txt"; then
                echo "$d is configured with $var: not a generic build" >&2
                exit 1
            fi
        done
    done
    local forbid=(--must-not dani.tech) line
    while IFS= read -r line; do
        line=${line%$'\r'}
        [ -n "$line" ] && forbid+=(--must-not "$line")
    done <<< "${CDV_PRIVATE_STRINGS:-}"
    [ ${#forbid[@]} -gt 2 ] || echo "note: CDV_PRIVATE_STRINGS is empty; checking the hostname only"
    local find=$here/tools/find-strings.py
    python3 "$find" --must rs-ny.rustdesk.com "${forbid[@]}" \
        "$b68/C-Desk-Vint.bin" "$bppc/C-Desk-Vint.bin" "$dist/C-Desk-Vint.hda"
    python3 "$find" "${forbid[@]}" \
        "$b68/C-Desk-Vint-Installer.bin" "$b68/C-Desk-Vint-Strip.bin" \
        "$dist/BUILD-INFO.txt" "$dist/C-Desk-Vint.sit.hqx"
}

case "$step" in
compile) compile ;;
package) package ;;
check) check ;;
all) compile; package; check ;;
*) echo "usage: $0 [compile|package|check|all]" >&2; exit 1 ;;
esac
