#!/usr/bin/env python3
"""The component versions of a release, from shim/VERSION and launcher/fh2mp_launcher/version.py, checked against the tag.

    python3 -I release/versions.py --tag v1.1.6 [--full] [--github-output FILE]
    python3 -I release/versions.py                      (no tag: a test build of both)

A full release builds the shim and the launcher at the tag's version. A hotfix (a patch tag of the same X.Y line, cut from a hotfix/X.Y branch
that starts at the release tag) builds only the component whose version file was set to the tag. The other keeps the version of the
release it was built in and is carried: release/make_manifest.py --carry-dir copies its signed manifest entry, and the release workflow
copies its files, from that release. Setting shim/VERSION back to an earlier release's version withdraws a shim hotfix the same way.

Rules: the tag is vX.Y.Z or vX.Y.Z-rcN; both versions are of its X.Y line and not above it; a carried version is a stable X.Y.Z and the tag
is too; --full (release/publish.sh for ops/ship.sh) requires both to equal the tag.
Prints key=value lines (shim, launcher, build_shim, build_launcher, kind = release | hotfix | rollback); --github-output appends them there.
Exit 1 with the reason when a rule does not hold.
"""
import argparse
import ast
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$")


def vkey(v):
    m = VER_RE.match(v)
    return (int(m[1]), int(m[2]), int(m[3]), 0 if m[4] else 1, int(m[4] or 0))


def read(root=ROOT):
    with open(os.path.join(root, "shim", "VERSION"), encoding="utf-8") as f:
        shim = f.read().strip()
    with open(os.path.join(root, "launcher", "fh2mp_launcher", "version.py"), encoding="utf-8") as f:
        src = f.read()
    launcher = next(ast.literal_eval(n.value) for n in ast.parse(src).body
                    if isinstance(n, ast.Assign) and any(getattr(t, "id", "") == "__version__" for t in n.targets))
    return shim, launcher


def plan(tag, shim, launcher, full=False):
    """-> dict of the outputs, or raises ValueError with the reason."""
    for name, v in (("shim/VERSION", shim), ("launcher version.py", launcher)):
        if not VER_RE.match(v):
            raise ValueError(f"{name} is {v!r}, not X.Y.Z or X.Y.Z-rcN")
    if tag is None:
        return {"shim": shim, "launcher": launcher, "build_shim": "true", "build_launcher": "true", "kind": "release"}
    if not tag.startswith("v") or not VER_RE.match(tag[1:]):
        raise ValueError(f"tag {tag!r} is not vX.Y.Z or vX.Y.Z-rcN")
    ver = tag[1:]
    built = {"shim": shim == ver, "launcher": launcher == ver}
    if full and not all(built.values()):
        raise ValueError(f"version mismatch: tag {ver}, shim {shim}, launcher {launcher} (a full release sets both to the tag)")
    for name, v in (("shim", shim), ("launcher", launcher)):
        if built[name]:
            continue
        if vkey(v)[:2] != vkey(ver)[:2]:
            raise ValueError(f"the {name} ({v}) is not of the {ver} line: only a full release changes X.Y")
        if vkey(v) > vkey(ver):
            raise ValueError(f"the {name} ({v}) is newer than the tag {ver}")
        if "-" in v or "-" in ver:
            raise ValueError(f"a carried {name} needs stable versions (tag {ver}, {name} {v}): release candidates build both")
    kind = "release" if all(built.values()) else "hotfix" if any(built.values()) else "rollback"
    return {"shim": shim, "launcher": launcher, "build_shim": str(built["shim"]).lower(), "build_launcher": str(built["launcher"]).lower(),
            "kind": kind}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tag")
    ap.add_argument("--full", action="store_true", help="both components must equal the tag")
    ap.add_argument("--github-output", help="append the key=value lines to this file ($GITHUB_OUTPUT)")
    ap.add_argument("--root", default=ROOT, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    try:
        out = plan(a.tag, *read(a.root), full=a.full)
    except ValueError as e:
        print(e, file=sys.stderr)
        return 1
    lines = "".join(f"{k}={v}\n" for k, v in out.items())
    sys.stdout.write(lines)
    if a.github_output:
        with open(a.github_output, "a", encoding="utf-8") as f:
            f.write(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
