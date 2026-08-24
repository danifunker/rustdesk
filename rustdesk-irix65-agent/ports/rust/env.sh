# Environment for cross-building the agent for IRIX n32.
#
# Source this, then run cargo from ports/rust/<crate>.
#
# The private RUSTUP_HOME/CARGO_HOME are not tidiness. Both mogrix steps rewrite
# shared state in place -- patch-rust-sysroot.sh edits nightly's std source, and
# `mogrix patch-crates` edits crate sources inside the cargo registry -- and
# neither is namespaced by target. Another effort on this machine
# (~/repos/rust-irixlibstd, the o32 build) uses the default homes.
#
# Resolved from this file's own location, not from $(pwd): the paths must be
# absolute -- an unresolved -lrust_irix_compat fails somewhere else entirely and
# names nothing to do with the cause -- but hard-coding one person's home
# directory means the pipeline only runs on one machine. ${BASH_SOURCE} is where
# this file is, whatever directory it was sourced from.
# $BASH_SOURCE when a person sources this from bash, which is the usual case.
# A script that is not bash -- scripts/build.sh runs under /bin/sh -- says where
# the tree is instead, because $0 in a sourced file names the CALLER there and
# resolving from it lands in the wrong directory with no error at all.
if [ -n "${RD_RUST_DIR:-}" ]; then
	RD_RUST=$RD_RUST_DIR
elif [ -n "${BASH_SOURCE:-}" ]; then
	RD_RUST=$(cd "$(dirname "$BASH_SOURCE")" && pwd)
else
	RD_RUST=$(cd "$(dirname "$0")" && pwd)
fi

export RUSTUP_HOME="$RD_RUST/rustup"
export CARGO_HOME="$RD_RUST/cargo"
export SGUG_STAGING="${SGUG_STAGING:-/opt/sgug-staging/usr/sgug}"
export MOGRIX_CROSS="${MOGRIX_CROSS:-$HOME/repos/mogrix/cross/bin}"
export PATH="$CARGO_HOME/bin:$MOGRIX_CROSS:$SGUG_STAGING/bin:$PATH"

# mio has no epoll or eventfd here; -L points the linker at the compat archive
# the target spec's late-link-args asks for by name.
#
# The path is absolute on purpose. Using $(pwd) would bake in whatever directory
# this happened to be sourced from, and the link then fails somewhere else with
# an unresolved -lrust_irix_compat rather than anything that names the cause.
# agent-portable's compat/ is a symlink to hello's, so one archive serves both.
export IRIX_COMPAT_DIR="$RD_RUST/hello/compat"

# -rpath, so an INSTALLED agent finds its libgcc_s.so.1 with no environment
# variable and no wrapper script. libgcc_s is the one library the agent needs
# that stock IRIX 6.5 does not have -- everything else it links (libX11, libz,
# libpthread, libc, libm) ships with the OS, and libsodium, libvpx, mbedTLS and
# zstd are linked statically. The package puts a copy in /usr/lib/rustdesk-agent
# rather than in /usr/lib32, because dropping a GCC runtime into a system
# directory is the kind of thing that breaks an unrelated program a year later.
#
# Harmless in a development build: a directory that does not exist costs one
# failed stat and the /tmp workflow keeps using LD_LIBRARYN32_PATH.
export RD_RPATH="${RD_RPATH:-/usr/lib/rustdesk-agent}"
export RUSTFLAGS="--cfg mio_unsupported_force_poll_poll --cfg mio_unsupported_force_waker_pipe -L $IRIX_COMPAT_DIR -C link-arg=-Wl,-rpath,$RD_RPATH"

# libsodium-sys: SODIUM_LIB_DIR alone. SODIUM_STATIC now panics ("deprecated,
# use SODIUM_SHARED"), and leaving SODIUM_SHARED unset already means static.
export SODIUM_LIB_DIR="$SGUG_STAGING/lib32"
export SODIUM_INCLUDE_DIR="$SGUG_STAGING/include"

# The cc crate needs the cross compiler named per-target.
export CC_mips_sgi_irix6_5="$SGUG_STAGING/bin/irix-cc"
export AR_mips_sgi_irix6_5=ar

echo "IRIX rust env: RUSTUP_HOME=$RUSTUP_HOME"
echo "               CARGO_HOME=$CARGO_HOME"
echo "               compat archive=$IRIX_COMPAT_DIR"
echo "then: cd ports/rust/<crate> && cargo +nightly build --release"
