#!/usr/bin/env python3
"""Builds dist/fh2-extra-options-<ver>.zip: FH2 Extra Options (RokkuDayo, GPL-3.0) packed for the launcher.

    python3 -I release/fetch_extra_options.py --dist dist/

The mod is downloaded from its author's GitHub release, checked against the pinned SHA-256 below, and re-packed UNCHANGED together with what
the GPL asks for: the licence text, a credit/NOTICE, and the complete corresponding source (the author's tag archive). Nothing from it is
committed to this repository. The launcher installs the zip only when the player has no copy of the mod (see docs/launcher.md).

To move to a newer release: change the five constants after reading its notes, and test it against the shim (the shim chain-loads it).
"""
import argparse
import hashlib
import io
import os
import sys
import urllib.request
import zipfile

REPO = "RokkuDayo/ForzaHorizon2_ExtraOptions"
TAG = "Azimuth_v0.9.10"
VERSION = "0.2.98"
RELEASE_ZIP = f"FH2_Extra_Options_{VERSION}-release.zip"
RELEASE_SHA256 = "d3b9db715416abd8efef4048445a9edbe22d333ba0267fb48fb255dfab000c38"
DLL_SHA256 = "d372b3a331c93cbbbf984d9a3b5df63836fdfb2aee26a10660c838c4806ff3d5"      # Azimuth/winmm.dll inside that zip; the launcher only accepts this build (launcher constants.py)
SOURCE_COMMIT = "1ff0535f372be33fccefa987fc903a7b4c79bb69"     # what TAG pointed at when this was pinned

NOTICE = """FH2 Extra Options {ver} by Arthur (RokkuDayo)
https://github.com/{repo}   (release tag {tag}, commit {commit})

This folder contains FH2 Extra Options, a third-party mod that is NOT part of FH2MP. It is free software under the GNU General Public
License, version 3 (LICENSE.txt). FH2MP only copies it, unchanged, to where Azimuth can load it, and only when you do not have it already.

  winmm.dll                 the mod as released by its author (SHA-256 of the release zip: {sha})
  FH2_ExtraOptions.ini      its default settings (yours are kept on updates)
  source/                   the complete corresponding source code, as published by the author for this release

You may change, rebuild and redistribute it under the terms of the GPL. To use your own build, replace winmm.dll in this folder.
To keep FH2MP from installing it, untick "Install FH2 Extra Options if it is missing" in the launcher's Settings (before deleting this folder).
"""


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "fh2mp-release"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", required=True)
    a = ap.parse_args()

    base = f"https://github.com/{REPO}"
    rel = get(f"{base}/releases/download/{TAG}/{RELEASE_ZIP}")
    if hashlib.sha256(rel).hexdigest() != RELEASE_SHA256:
        sys.exit("the Extra Options release does not match the pinned SHA-256: refusing to package it")
    with zipfile.ZipFile(io.BytesIO(rel)) as z:
        dll, ini = z.read("Azimuth/winmm.dll"), z.read("Azimuth/FH2_ExtraOptions.ini")
    if hashlib.sha256(dll).hexdigest() != DLL_SHA256:
        sys.exit("Azimuth/winmm.dll in the release is not the pinned build")
    lic = get(f"https://raw.githubusercontent.com/{REPO}/{SOURCE_COMMIT}/LICENSE")
    if b"GNU GENERAL PUBLIC LICENSE" not in lic or b"Version 3" not in lic[:200]:
        sys.exit("the licence file is not the GPL v3 text: not packaging")
    src = get(f"{base}/archive/{SOURCE_COMMIT}.zip")
    if not zipfile.is_zipfile(io.BytesIO(src)):
        sys.exit("the source archive is not a zip")

    os.makedirs(a.dist, exist_ok=True)
    out = os.path.join(a.dist, f"fh2-extra-options-{VERSION}.zip")
    notice = NOTICE.format(ver=VERSION, repo=REPO, tag=TAG, commit=SOURCE_COMMIT, sha=RELEASE_SHA256)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in (("winmm.dll", dll), ("FH2_ExtraOptions.ini", ini), ("LICENSE.txt", lic), ("NOTICE.txt", notice.encode()),
                           (f"source/ForzaHorizon2_ExtraOptions-{SOURCE_COMMIT[:8]}.zip", src)):
            z.writestr(zipfile.ZipInfo(name, (2026, 10, 20, 0, 0, 0)), data, zipfile.ZIP_DEFLATED)
    print(out, hashlib.sha256(open(out, "rb").read()).hexdigest())


if __name__ == "__main__":
    main()
