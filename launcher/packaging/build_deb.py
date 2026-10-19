#!/usr/bin/env python3
"""Builds dist/fh2mp-launcher_<ver>_amd64.deb from dist/fh2mp-launcher/ (PyInstaller onedir). Pure Python (ar + tar), so it runs on any distro, no dpkg needed.
   python3 packaging/build_deb.py [--dist DIR] [--out FILE]"""
import argparse
import gzip
import io
import os
import re
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DESKTOP = """[Desktop Entry]
Type=Application
Name=FH2MP
Comment=Forza Horizon 2 online
Exec=fh2mp-launcher
Icon=fh2mp-launcher
Categories=Game;
Terminal=false
StartupWMClass=fh2mp-launcher
"""


def version():
    return re.search(r'"(.+)"', open(os.path.join(ROOT, "fh2mp_launcher", "version.py")).read())[1]


def add(tar, name, data=b"", mode=0o644, kind=tarfile.REGTYPE, link=""):
    ti = tarfile.TarInfo(name)
    ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname, ti.mode, ti.type = int(time.time()), 0, 0, "root", "root", mode, kind
    if kind == tarfile.SYMTYPE:
        ti.linkname = link
        tar.addfile(ti)
    elif kind == tarfile.DIRTYPE:
        tar.addfile(ti)
    else:
        ti.size = len(data)
        tar.addfile(ti, io.BytesIO(data))


def ar(members):
    out = b"!<arch>\n"
    for name, data in members:
        out += (f"{name}/".ljust(16) + str(int(time.time())).ljust(12) + "0".ljust(6) + "0".ljust(6) + "100644".ljust(8) + str(len(data)).ljust(10) + "`\n").encode()
        out += data + (b"\n" if len(data) % 2 else b"")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(ROOT, "dist", "fh2mp-launcher"))
    ap.add_argument("--out")
    a = ap.parse_args()
    ver = version()
    out = a.out or os.path.join(ROOT, "dist", f"fh2mp-launcher_{ver}_amd64.deb")
    if not os.path.isfile(os.path.join(a.dist, "fh2mp-launcher")):
        sys.exit(f"{a.dist}/fh2mp-launcher not found: run PyInstaller first")
    data = io.BytesIO()
    size = 0
    with tarfile.open(fileobj=data, mode="w:gz", format=tarfile.GNU_FORMAT) as t:
        for d in ("./opt", "./opt/fh2mp-launcher", "./usr", "./usr/bin", "./usr/share", "./usr/share/applications", "./usr/share/icons",
                  "./usr/share/icons/hicolor", "./usr/share/icons/hicolor/256x256", "./usr/share/icons/hicolor/256x256/apps"):
            add(t, d, mode=0o755, kind=tarfile.DIRTYPE)
        for base, dirs, files in os.walk(a.dist):
            rel = os.path.relpath(base, a.dist)
            dest = "./opt/fh2mp-launcher" + ("" if rel == "." else "/" + rel)
            if rel != ".":
                add(t, dest, mode=0o755, kind=tarfile.DIRTYPE)
            for fn in sorted(files):
                p = os.path.join(base, fn)
                if os.path.islink(p):
                    add(t, f"{dest}/{fn}", kind=tarfile.SYMTYPE, link=os.readlink(p), mode=0o777)
                    continue
                with open(p, "rb") as f:
                    blob = f.read()
                size += len(blob)
                add(t, f"{dest}/{fn}", blob, 0o755 if os.access(p, os.X_OK) else 0o644)
        add(t, "./usr/bin/fh2mp-launcher", kind=tarfile.SYMTYPE, link="/opt/fh2mp-launcher/fh2mp-launcher", mode=0o777)
        add(t, "./usr/share/applications/fh2mp-launcher.desktop", DESKTOP.encode())
        with open(os.path.join(ROOT, "fh2mp_launcher", "assets", "icon.png"), "rb") as f:
            add(t, "./usr/share/icons/hicolor/256x256/apps/fh2mp-launcher.png", f.read())
    control = f"""Package: fh2mp-launcher
Version: {ver}
Section: games
Priority: optional
Architecture: amd64
Installed-Size: {size // 1024}
Depends: libc6, libgl1, libegl1, libxkbcommon-x11-0, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-render-util0, libxcb-shape0, libdbus-1-3
Recommends: policykit-1 | pkexec, gnome-keyring | kwalletmanager
Maintainer: FH2MP <noreply@invalid>
Homepage: https://forza.scuffed.af
Description: FH2MP launcher
 Forza Horizon 2 online: sign in, keep the shim and Azimuth in order, play.
"""
    ctl = io.BytesIO()
    with tarfile.open(fileobj=ctl, mode="w:gz", format=tarfile.GNU_FORMAT) as t:
        add(t, "./", mode=0o755, kind=tarfile.DIRTYPE)
        add(t, "./control", control.encode())
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(ar([("debian-binary", b"2.0\n"), ("control.tar.gz", ctl.getvalue()), ("data.tar.gz", data.getvalue())]))
    print(out, f"{os.path.getsize(out) // (1 << 20)} MB")


if __name__ == "__main__":
    main()
