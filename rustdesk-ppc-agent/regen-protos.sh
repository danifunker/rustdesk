#!/usr/bin/env bash
#
# Regenerate src/protos/*.rs from ../libs/hbb_common/protos/*.proto.
#
# The generated files are CHECKED IN rather than produced by a build script, and
# this script is why that is affordable.
#
# `protobuf-codegen-pure` drags in ~16 crates that exist only to run codegen —
# protobuf-parse, protoc, tempfile, rustix (x2), which, walkdir, syn, serde_derive
# and friends. Under mrustc every one of those has to be transpiled for the host
# before a single line of the agent compiles, and one of them actually failed:
# rustix's build script sets `#[cfg(apple)]` even on a Linux host build, so its
# Apple backend reached for `libc::host_info64_t` and could not resolve it.
#
# None of that work is needed. The .proto files change roughly never, so codegen
# runs here, on demand, in a throwaway crate — and the agent's own dependency
# list stays at 5 direct crates with no build script at all.
#
# Usage:  rustdesk-ppc-agent/regen-protos.sh
# Then:   git diff src/protos/   (review, and commit if the .proto really moved)
set -euo pipefail

AGENT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTOS="$AGENT/../libs/hbb_common/protos"
OUT="$AGENT/src/protos"

[ -d "$PROTOS" ] || { echo "error: no protos at $PROTOS" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src"

cat > "$WORK/Cargo.toml" <<EOF
[workspace]
[package]
name = "protogen"
version = "0.0.0"
edition = "2018"

[dependencies]
protobuf-codegen-pure = "3.0.0-alpha.2"

# This vintage of protobuf-codegen-pure has a slice-indexing UB that modern
# std's debug-only precondition checks abort on. It only bites in a debug build.
[profile.dev]
debug-assertions = false
opt-level = 1
EOF

cat > "$WORK/src/main.rs" <<EOF
fn main() {
    let protos = std::path::Path::new("$PROTOS");
    let out = std::path::Path::new("$OUT");
    std::fs::create_dir_all(out).unwrap();
    protobuf_codegen_pure::Codegen::new()
        .out_dir(out)
        .inputs(&[protos.join("rendezvous.proto"), protos.join("message.proto")])
        .include(protos)
        .run()
        .expect("protobuf codegen failed");
    println!("regenerated into {}", out.display());
}
EOF

echo "regenerating from $PROTOS ..."
( cd "$WORK" && cargo run --quiet )
echo
ls -l "$OUT"
echo
echo "Now review: git diff $OUT"
