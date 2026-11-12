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

A hotfix release (release/versions.py) builds one component and carries the other:
    python3 -I release/make_manifest.py --tag v1.1.6 --shim-version 1.1.6 --launcher-version 1.1.5 --carry-dir carry/ --dist dist/
The carried component's entry comes from carry/v<its version>/manifest.json (+ .sig), checked with this key's public half, and its files must
be in --dist with exactly the sizes and SHA-256 of that entry: every release holds each file its manifest names, so a launcher that only
knows the manifest's own tag (1.1.5 and older) still finds them. minLauncher stays the carried release's unless --min-launcher is given.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import sys
import zipfile

from cryptography.exceptions import InvalidSignature
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


def line(v):
    m = re.match(r"^(\d+)\.(\d+)\.", v)
    return f"{m[1]}.{m[2]}" if m else ""


def vkey(v):
    return [int(x) for x in re.findall(r"\d+", v or "0.0.0")[:3]]


def carried(carry_dir, ver, pub):
    """The signed manifest of release v<ver>, from carry_dir/v<ver>/, checked with `pub`."""
    d = os.path.join(carry_dir or "", f"v{ver}")
    try:
        with open(os.path.join(d, "manifest.json"), "rb") as f:
            raw = f.read()
        with open(os.path.join(d, "manifest.json.sig"), encoding="ascii") as f:
            sig = base64.b64decode(f.read().strip())
    except (OSError, ValueError) as e:
        sys.exit(f"cannot read the manifest of v{ver} to carry from ({e}): download it into {d}")
    try:
        pub.verify(sig, raw)
    except InvalidSignature:
        sys.exit(f"the manifest of v{ver} in {d} is not signed with this key")
    m = json.loads(raw)
    if m.get("tag") != f"v{ver}":
        sys.exit(f"{d}/manifest.json belongs to {m.get('tag')}, not v{ver}")
    return m


def same_file(dist, e, what):
    p = os.path.join(dist, e["name"])
    if not os.path.isfile(p):
        sys.exit(f"the carried {what} {e['name']} is not in {dist}: copy it from its release")
    got = entry(p)
    if (got["sha256"], got["size"]) != (e["sha256"], e["size"]):
        sys.exit(f"{p} is not the file the carried manifest names (SHA-256 or size differs)")


def build_shim(dist, ver):
    shim_zip = os.path.join(dist, f"fh2mp-shim-{ver}.zip")
    if not os.path.isfile(shim_zip):
        sys.exit(f"missing {shim_zip}")
    with zipfile.ZipFile(shim_zip) as z:
        names = {n.replace("\\", "/"): n for n in z.namelist()}      # Windows' Compress-Archive may write a backslash (test-build.yml)
        if "fh2-mods/FH2MP.dll" not in names:
            sys.exit(f"{shim_zip} has no fh2-mods/FH2MP.dll: build it with release/build_shim_zip.sh")
        dll = z.read(names["fh2-mods/FH2MP.dll"])
        guide = z.read(names["fh2-mods/FH2MPGuide.dll"]) if "fh2-mods/FH2MPGuide.dll" in names else None
    if f"FH2MP-SHIM-VERSION:{ver}:END".encode() not in dll:
        sys.exit("the shim in the zip does not carry this version's marker: rebuild it with shim/VERSION = " + ver)
    for marker in (b"-debug:END", "fh2mp-dbg".encode("utf-16-le"), b"fh2mp-dbg"):      # a research build must never be published
        if marker in dll:
            sys.exit("the shim in the zip is a debug build (live debugger / tracer): rebuild with shim/build.sh (no argument)")
    out = {"version": ver, **entry(shim_zip), "dllSha256": hashlib.sha256(dll).hexdigest()}
    if guide is not None:
        out["guideSha256"] = hashlib.sha256(guide).hexdigest()      # the Guide, built and signed with the shim (release.yml shim-sign)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dist", required=True)
    ap.add_argument("--key", help="file with the base64 private seed (default: env FH2MP_RELEASE_KEY)")
    # 1.0.0 is the first launcher that installs the shim as an FH2 Mod Platform mod; older ones would look for winmm.dll in the zip
    ap.add_argument("--min-launcher", help="default: the tag's version, or in a hotfix the carried release's minLauncher")
    ap.add_argument("--shim-version", help="default: the tag's version (built); another version is carried")
    ap.add_argument("--launcher-version", help="default: the tag's version (built); another version is carried")
    ap.add_argument("--carry-dir", help="holds v<version>/manifest.json(.sig) of each carried component's release")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()
    if not re.fullmatch(r"v[0-9][0-9A-Za-z.+-]*", a.tag):
        sys.exit("tag must look like v1.0.0")
    ver = a.tag[1:]
    seed_b64 = open(a.key).read().strip() if a.key else os.environ.get("FH2MP_RELEASE_KEY", "").strip()
    if not seed_b64:
        sys.exit("no signing key: pass --key or set FH2MP_RELEASE_KEY")
    sk = Ed25519PrivateKey.from_private_bytes(base64.b64decode(seed_b64))
    shim_ver, app_ver = a.shim_version or ver, a.launcher_version or ver
    sources = []                       # the manifests carried from

    if shim_ver == ver:
        shim = build_shim(a.dist, ver)
    else:
        src = carried(a.carry_dir, shim_ver, sk.public_key())
        shim = dict(src.get("shim") or {})
        if shim.get("version") != shim_ver:
            sys.exit(f"the manifest of v{shim_ver} names shim {shim.get('version')!r}, not {shim_ver}")
        same_file(a.dist, shim, "shim")
        sources.append(src)

    if app_ver == ver:
        assets = {}
        for key, pattern in LAUNCHER_ASSETS.items():
            p = os.path.join(a.dist, pattern.format(v=ver))
            if os.path.isfile(p):
                assets[key] = entry(p)
    else:
        src = carried(a.carry_dir, app_ver, sk.public_key())
        launcher = src.get("launcher") or {}
        if launcher.get("version") != app_ver:
            sys.exit(f"the manifest of v{app_ver} names launcher {launcher.get('version')!r}, not {app_ver}")
        assets = dict(launcher.get("assets") or {})
        for e in assets.values():
            same_file(a.dist, e, "launcher package")
        sources.append(src)

    # A hotfix never turns launchers away by itself: the server's minLauncher (ops/release-prod.sh --require-launcher) does that on purpose.
    min_launcher = a.min_launcher or (max((s.get("minLauncher") or "1.0.0" for s in sources), key=vkey) if sources else ver)
    kind = "release" if not sources else "hotfix" if len(sources) == 1 else "rollback"
    m = {"schema": 1, "tag": a.tag, "version": ver, "line": line(ver), "kind": kind, "minLauncher": min_launcher, "notes": a.notes,
         "shim": shim, "launcher": {"version": app_ver, "assets": assets}}
    raw = (json.dumps(m, indent=1, sort_keys=True) + "\n").encode()
    with open(os.path.join(a.dist, "manifest.json"), "wb") as f:
        f.write(raw)
    with open(os.path.join(a.dist, "manifest.json.sig"), "w") as f:
        f.write(base64.b64encode(sk.sign(raw)).decode() + "\n")
    print(f"manifest for {a.tag} ({kind}): shim {shim_ver}{'' if shim_ver == ver else ' carried'}, launcher {app_ver}"
          f"{'' if app_ver == ver else ' carried'} with {len(assets)} package(s): {', '.join(assets) or 'none'}; minLauncher {min_launcher}")


if __name__ == "__main__":
    main()
