# Environment for cross-building the agent for IRIX n32.
#
# Source this, then run cargo from ports/rust/<crate>.
#
# The private RUSTUP_HOME/CARGO_HOME are not tidiness. Both mogrix steps rewrite
# shared state in place -- patch-rust-sysroot.sh edits nightly's std source, and
# `mogrix patch-crates` edits crate sources inside the cargo registry -- and
# neither is namespaced by target. Another effort on this machine
# (~/repos/rust-irixlibstd, the o32 build) uses the default homes.
export RUSTUP_HOME=/home/dani/repos/rustdesk/rustdesk-irix65-agent/ports/rust/rustup
export CARGO_HOME=/home/dani/repos/rustdesk/rustdesk-irix65-agent/ports/rust/cargo
export PATH="$CARGO_HOME/bin:/home/dani/repos/mogrix/cross/bin:/opt/sgug-staging/usr/sgug/bin:$PATH"

# mio has no epoll or eventfd here; -L points the linker at the compat archive
# the target spec's late-link-args asks for by name.
#
# The path is absolute on purpose. Using $(pwd) would bake in whatever directory
# this happened to be sourced from, and the link then fails somewhere else with
# an unresolved -lrust_irix_compat rather than anything that names the cause.
# agent-portable's compat/ is a symlink to hello's, so one archive serves both.
export IRIX_COMPAT_DIR=/home/dani/repos/rustdesk/rustdesk-irix65-agent/ports/rust/hello/compat
export RUSTFLAGS="--cfg mio_unsupported_force_poll_poll --cfg mio_unsupported_force_waker_pipe -L $IRIX_COMPAT_DIR"

# libsodium-sys: SODIUM_LIB_DIR alone. SODIUM_STATIC now panics ("deprecated,
# use SODIUM_SHARED"), and leaving SODIUM_SHARED unset already means static.
export SODIUM_LIB_DIR=/opt/sgug-staging/usr/sgug/lib32
export SODIUM_INCLUDE_DIR=/opt/sgug-staging/usr/sgug/include

# The cc crate needs the cross compiler named per-target.
export CC_mips_sgi_irix6_5=/opt/sgug-staging/usr/sgug/bin/irix-cc
export AR_mips_sgi_irix6_5=ar

echo "IRIX rust env: RUSTUP_HOME=$RUSTUP_HOME"
echo "               CARGO_HOME=$CARGO_HOME"
echo "               compat archive=$IRIX_COMPAT_DIR"
echo "then: cd ports/rust/<crate> && cargo +nightly build --release"
