#!/usr/bin/env python3
"""Builds and signs the release manifest from the files in a directory.

    python3 -I release/make_manifest.py --tag v1.0.0 --dist dist/ --key <private key file | env FH2MP_RELEASE_KEY>

The directory must hold (names are what the launcher expects):
    fh2mp-shim-<ver>.zip                              release/build_shim_zip.sh: fh2-mods/FH2MP.dll (+ licence, README)
    FH2MP-Launcher-<ver>-windows-x64-setup.exe        Inno Setup installer          (optional)
    FH2MP-Launcher-<ver>-windows-x64-portable.zip                                  (optional)
    fh2mp-launcher_<ver>_amd64.deb                                                 (optional)
    FH2MP-Launcher-<ver>-x86_64.AppImage                                           (optional)
Writes manifest.json and manifest.json.sig next to them. The signature is Ed25519 over the exact bytes of manifest.json.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import sys
import zipfile

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

LAUNCHER_ASSETS = {
    "windows-x64-installer": "FH2MP-Launcher-{v}-windows-x64-setup.exe",
    "windows-x64-portable": "FH2MP-Launcher-{v}-windows-x64-portable.zip",
    "linux-deb": "fh2mp-launcher_{v}_amd64.deb",
    "linux-appimage": "FH2MP-Launcher-{v}-x86_64.AppImage",
}


def entry(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return {"name": os.path.basename(path), "sha256": h.hexdigest(), "size": os.path.getsize(path)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dist", required=True)
    ap.add_argument("--key", help="file with the base64 private seed (default: env FH2MP_RELEASE_KEY)")
    # 1.0.0 is the first launcher that installs the shim as an FH2 Mod Platform mod; older ones would look for winmm.dll in the zip
    ap.add_argument("--min-launcher", default="1.0.0")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()
    if not re.fullmatch(r"v[0-9][0-9A-Za-z.+-]*", a.tag):
        sys.exit("tag must look like v1.0.0")
    ver = a.tag[1:]
    seed_b64 = open(a.key).read().strip() if a.key else os.environ.get("FH2MP_RELEASE_KEY", "").strip()
    if not seed_b64:
        sys.exit("no signing key: pass --key or set FH2MP_RELEASE_KEY")
    sk = Ed25519PrivateKey.from_private_bytes(base64.b64decode(seed_b64))

    shim_zip = os.path.join(a.dist, f"fh2mp-shim-{ver}.zip")
    if not os.path.isfile(shim_zip):
        sys.exit(f"missing {shim_zip}")
    with zipfile.ZipFile(shim_zip) as z:
        names = set(z.namelist())
        if "fh2-mods/FH2MP.dll" not in names:
            sys.exit(f"{shim_zip} has no fh2-mods/FH2MP.dll: build it with release/build_shim_zip.sh")
        dll = z.read("fh2-mods/FH2MP.dll")
    if f"FH2MP-SHIM-VERSION:{ver}:END".encode() not in dll:
        sys.exit("the shim in the zip does not carry this version's marker: rebuild it with shim/VERSION = " + ver)
    for marker in (b"-debug:END", "fh2mp-dbg".encode("utf-16-le"), b"fh2mp-dbg"):      # a research build must never be published
        if marker in dll:
            sys.exit("the shim in the zip is a debug build (live debugger / tracer): rebuild with shim/build.sh (no argument)")
    shim = {"version": ver, **entry(shim_zip), "dllSha256": hashlib.sha256(dll).hexdigest()}

    assets = {}
    for key, pattern in LAUNCHER_ASSETS.items():
        p = os.path.join(a.dist, pattern.format(v=ver))
        if os.path.isfile(p):
            assets[key] = entry(p)
    m = {"schema": 1, "tag": a.tag, "version": ver, "minLauncher": a.min_launcher, "notes": a.notes,
         "shim": shim, "launcher": {"version": ver, "assets": assets}}
    raw = (json.dumps(m, indent=1, sort_keys=True) + "\n").encode()
    with open(os.path.join(a.dist, "manifest.json"), "wb") as f:
        f.write(raw)
    with open(os.path.join(a.dist, "manifest.json.sig"), "w") as f:
        f.write(base64.b64encode(sk.sign(raw)).decode() + "\n")
    print(f"manifest for {a.tag}: shim + {len(assets)} launcher package(s): {', '.join(assets) or 'none'}")


if __name__ == "__main__":
    main()
