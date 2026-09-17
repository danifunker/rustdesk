#!/usr/bin/env python3
"""Rewrite git dependencies to path dependencies into rustdesk-ppc/vendor.

minicargo cannot read `git = "..."` dependencies:

    EXCEPTION: TODO: libs/vintage_common/Cargo.toml:28: Support git dependencies

`cargo vendor` has already flattened every git dep into rustdesk-ppc/vendor/,
so the manifests just need to point there instead. This is applied before a
transpile and reverted before re-running `cargo vendor` (cargo needs the real
git URLs to re-resolve).

Idempotent both ways. Usage:

    patches/git-deps.py            # apply  (git -> path)
    patches/git-deps.py --revert   # revert (path -> git)

The original line is preserved verbatim in a trailing `#PPCGIT:` comment, so
revert is exact and no separate backup file is needed.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VENDOR = os.path.join(REPO, "rustdesk-ppc", "vendor")
MANIFESTS = ["Cargo.toml", "libs/vintage_common/Cargo.toml",
             "libs/scrap/Cargo.toml", "libs/enigo/Cargo.toml"]

MARK = "  #PPCGIT:"
# `name = { ... git = "..." ... }`  (single line; that is how they are all written)
GIT_DEP = re.compile(r'^(?P<name>[A-Za-z0-9_-]+)\s*=\s*\{(?P<body>.*\bgit\s*=\s*".*)\}\s*$')


def vendor_dir_for(name):
    """cargo vendor names the directory after the *package*, not the dep key."""
    for cand in (name, name.replace("-", "_"), name.replace("_", "-")):
        if os.path.isdir(os.path.join(VENDOR, cand)):
            return cand
    # `rust-pulsectl` vendors as `pulsectl`, `rust-psutil` as `psutil`, etc.
    stripped = re.sub(r"^rust-", "", name)
    if os.path.isdir(os.path.join(VENDOR, stripped)):
        return stripped
    return None


def apply_to(path):
    rel_to_vendor = os.path.relpath(VENDOR, os.path.dirname(path))
    out, changed, skipped = [], 0, []
    for line in open(path).read().split("\n"):
        m = GIT_DEP.match(line.strip())
        if not m or MARK.strip() in line:
            out.append(line)
            continue
        name = m.group("name")
        vd = vendor_dir_for(name)
        if vd is None:
            skipped.append(name)
            out.append(line)
            continue
        # Keep `features`/`package`/`optional`; drop git/branch/rev/version.
        keep = [p.strip() for p in m.group("body").split(",")
                if p.strip() and not re.match(r'(git|branch|rev|tag|version)\s*=', p.strip())]
        keep.insert(0, 'path = "%s/%s"' % (rel_to_vendor, vd))
        indent = line[:len(line) - len(line.lstrip())]
        out.append('%s%s = { %s }%s%s' % (indent, name, ", ".join(keep), MARK, line.strip()))
        changed += 1
    open(path, "w").write("\n".join(out))
    return changed, skipped


def revert_to(path):
    out, changed = [], 0
    for line in open(path).read().split("\n"):
        if MARK.strip() in line:
            indent = line[:len(line) - len(line.lstrip())]
            out.append(indent + line.split(MARK.strip(), 1)[1])
            changed += 1
        else:
            out.append(line)
    open(path, "w").write("\n".join(out))
    return changed


def main():
    revert = "--revert" in sys.argv
    if not revert and not os.path.isdir(VENDOR):
        sys.exit("error: %s does not exist -- run `cargo vendor rustdesk-ppc/vendor` first" % VENDOR)
    total = 0
    for rel in MANIFESTS:
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path):
            continue
        if revert:
            n = revert_to(path)
            if n:
                print("reverted %d dep(s) in %s" % (n, rel))
        else:
            n, skipped = apply_to(path)
            if n:
                print("rewrote %d git dep(s) -> path in %s" % (n, rel))
            for s in skipped:
                print("  WARNING: no vendored dir for '%s' in %s -- left as git" % (s, rel))
        total += n
    print("%s: %d dependency line(s)" % ("reverted" if revert else "applied", total))


if __name__ == "__main__":
    main()
