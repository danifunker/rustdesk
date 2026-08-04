#!/usr/bin/env python3
"""Fix undefined behaviour in protobuf 3.0.0-alpha.2's wire-parse path.

`BufReadIter::read_exact_to_vec` does:

    target.reserve_exact(count);
    unsafe {
        self.read_exact(&mut target.get_unchecked_mut(..count))?;
        target.set_len(count);
    }

`reserve_exact` grows *capacity*, not *length* — so at that point `target.len()`
is 0 and `get_unchecked_mut(..count)` indexes past the end of the slice. It
happens to work in release builds because the allocation is real, but it is UB,
and modern std's debug-only precondition checks abort on it:

    unsafe precondition(s) violated: slice::get_unchecked_mut requires that the
    range is within the slice

`count` here comes straight off the wire, so this is on the attacker-controlled
path. Replaced with a safe `resize`, which costs one memset of a buffer we are
about to overwrite anyway.

Applied to the vendored copy, which `cargo vendor` regenerates - so this is
re-run rather than committed. Idempotent.

Usage:  rustdesk-ppc-agent/patches/protobuf-ub.py
"""
import hashlib
import json
import os
import sys

AGENT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(AGENT, "vendor", "protobuf", "src", "buf_read_iter.rs")

OLD = """            target.reserve_exact(count);

            unsafe {
                self.read_exact(&mut target.get_unchecked_mut(..count))?;
                target.set_len(count);
            }
"""

NEW = """            // `reserve_exact` grows capacity, not length, so the original
            // `get_unchecked_mut(..count)` here indexed past the end of a
            // zero-length slice - UB on a wire-controlled length.
            target.resize(count, 0);
            self.read_exact(&mut target[..count])?;
"""


def main():
    if not os.path.isfile(TARGET):
        sys.exit("error: %s not found -- run `cargo vendor vendor` first" % TARGET)
    s = open(TARGET).read()
    if NEW in s:
        print("protobuf UB fix: already applied")
        return
    if OLD not in s:
        sys.exit("error: protobuf source does not match the expected shape; "
                 "check whether the version changed")
    open(TARGET, "w").write(s.replace(OLD, NEW, 1))
    _refresh_checksum()
    print("protobuf UB fix: applied to %s" % os.path.relpath(TARGET, AGENT))


def _refresh_checksum():
    """cargo verifies vendored files against .cargo-checksum.json; update ours."""
    cs = os.path.join(AGENT, "vendor", "protobuf", ".cargo-checksum.json")
    if not os.path.isfile(cs):
        return
    d = json.load(open(cs))
    key = "src/buf_read_iter.rs"
    if key in d.get("files", {}):
        d["files"][key] = hashlib.sha256(open(TARGET, "rb").read()).hexdigest()
        json.dump(d, open(cs, "w"))


if __name__ == "__main__":
    main()
