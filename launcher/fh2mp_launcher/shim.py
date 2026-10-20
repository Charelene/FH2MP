"""The FH2MP shim in the Azimuth folder: what is installed, putting a new one in, coexisting with FH2 Extra Options."""
import hashlib
import os
import re
import shutil

from . import paths
from .constants import EXTRA_OPTIONS_DIR, EXTRA_OPTIONS_DLL_SHA256, EXTRA_OPTIONS_VERSION, LSP_FILE, SESSION_FILE, SHIM_DLL

MARKER = re.compile(rb"FH2MP-SHIM-VERSION:([0-9A-Za-z.+-]{1,32}):END")
EXTRA_FILES_PREFIX = "FH2_ExtraOptions."


class ShimError(Exception):
    pass


def _dll(folder):
    return os.path.join(folder, SHIM_DLL)


def inspect_dll(path):
    """-> ("ours", version) | ("foreign", "") | ("none", "")  by looking for the marker the shim carries."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return "none", ""
    m = MARKER.search(data)
    return ("ours", m.group(1).decode()) if m else ("foreign", "")


def installed(folder):
    return inspect_dll(_dll(folder))


def blob_version(data):
    m = MARKER.search(data)
    return m.group(1).decode() if m else ""


def install(folder, dll_bytes, expect_sha256=None):
    """Puts the shim next to azimuth.exe. A winmm.dll that is not ours (FH2 Extra Options) is moved to fh2extra\\ so the shim can chain-load
    it. Returns a list of things done, for the log. Raises ShimError with a message fit for the user."""
    if expect_sha256 and hashlib.sha256(dll_bytes).hexdigest() != expect_sha256.lower():
        raise ShimError("the downloaded shim does not match its checksum")
    if not blob_version(dll_bytes):
        raise ShimError("that file is not an FH2MP shim")
    did = []
    target = _dll(folder)
    kind, _ver = inspect_dll(target)
    if kind == "foreign":
        extra = os.path.join(folder, EXTRA_OPTIONS_DIR)
        dest = os.path.join(extra, SHIM_DLL)
        if os.path.exists(dest):
            raise ShimError(f"{SHIM_DLL} in the Azimuth folder is not ours, and {EXTRA_OPTIONS_DIR}\\{SHIM_DLL} already exists. "
                            "Please sort the two out by hand.")
        os.makedirs(extra, exist_ok=True)
        try:
            shutil.move(target, dest)
            did.append(f"moved the existing {SHIM_DLL} (FH2 Extra Options?) to {EXTRA_OPTIONS_DIR}")
            for name in os.listdir(folder):
                if name.startswith(EXTRA_FILES_PREFIX):
                    shutil.move(os.path.join(folder, name), os.path.join(extra, name))
                    did.append(f"moved {name}")
        except OSError as e:
            raise ShimError(f"could not move the existing {SHIM_DLL}: {e.strerror or e}. Is the game still running?") from None
    elif kind == "ours":
        try:
            shutil.copy2(target, target + ".prev")
        except OSError:
            pass
    try:
        paths.atomic_write(target, dll_bytes, 0o755)
    except OSError as e:
        raise ShimError(f"could not write {SHIM_DLL}: {e.strerror or e}. Close the game and try again.") from None
    did.append(f"installed shim {blob_version(dll_bytes)}")
    return did


def uninstall(folder):
    """Removes the shim and puts a moved Extra Options winmm.dll back."""
    target = _dll(folder)
    if inspect_dll(target)[0] == "ours":
        os.unlink(target)
    moved = os.path.join(folder, EXTRA_OPTIONS_DIR, SHIM_DLL)
    if os.path.isfile(moved) and not os.path.exists(target):
        shutil.move(moved, target)


def bin_dir(folder):
    """Where the shim looks for its files and writes its log: data\\bin when it exists, else the Azimuth folder itself."""
    d = os.path.join(folder, "data", "bin")
    return d if os.path.isdir(d) else folder


def write_session(folder, xuid, token, lsp_url):
    """The hand-over to the shim: who signed in and how to reach the server. Private to the user; removed when the game ends."""
    d = bin_dir(folder)
    paths.atomic_write(os.path.join(d, SESSION_FILE), f"xuid={xuid:x}\ntoken={token}\n".encode(), 0o600)
    paths.atomic_write(os.path.join(d, LSP_FILE), (lsp_url.strip() + "\n").encode(), 0o644)


def write_identity(folder, xuid):
    """The shim's fallback id for a game started without the launcher's session (Azimuth's own window, Steam): the account's, so the save profile
    stays the same. The shim keeps the low 32 bits."""
    paths.atomic_write(os.path.join(bin_dir(folder), "fh2mp-identity.txt"), f"{xuid & 0xFFFFFFFF:08x}\n".encode(), 0o644)


def clear_session(folder):
    try:
        os.unlink(os.path.join(bin_dir(folder), SESSION_FILE))
    except OSError:
        pass


# ---- FH2 Extra Options (RokkuDayo): third-party, never bundled. The launcher imports the player's own copy into fh2extra\\, where the shim chain-loads it.

import re as _re
import zipfile as _zipfile

EXTRA_INI = "FH2_ExtraOptions.ini"
EXTRA_FILES = (SHIM_DLL, EXTRA_INI, "FH2_ExtraOptions.dat", "Gamerpic.png")      # winmm.dll = the mod itself; the rest is its settings/picture


def _ini_info(text):
    v = _re.search(r"Release v([0-9][0-9.]*)", text)
    c = _re.search(r"Compatible with Azimuth ([0-9][0-9.]*)", text)
    return (v.group(1) if v else "", c.group(1) if c else "")


def extra_options_installed(folder):
    """-> (installed, mod version, Azimuth version it targets)."""
    d = os.path.join(folder, EXTRA_OPTIONS_DIR)
    if not os.path.isfile(os.path.join(d, SHIM_DLL)):
        return False, "", ""
    try:
        with open(os.path.join(d, EXTRA_INI), encoding="utf-8", errors="replace") as f:
            return (True, *_ini_info(f.read()))
    except OSError:
        return True, "", ""


SUPPORTED_EXTRA_SHA256 = {EXTRA_OPTIONS_DLL_SHA256}      # the only build FH2MP works with (tests may add their own)


def is_supported_extra(dll_bytes):
    return hashlib.sha256(dll_bytes).hexdigest() in SUPPORTED_EXTRA_SHA256


def extra_options_supported(folder):
    """-> True when the fh2extra folder holds exactly the supported FH2 Extra Options build (0.2.98)."""
    try:
        with open(os.path.join(folder, EXTRA_OPTIONS_DIR, SHIM_DLL), "rb") as f:
            return is_supported_extra(f.read())
    except OSError:
        return False


def _slurp(path):
    with open(path, "rb") as f:
        return f.read()


def _source_files(source):
    """-> {basename: bytes} of the mod files found in a folder or zip (shallowest copy of each wins); {} if it has no winmm.dll + ini."""
    found, depth = {}, {}

    def take(path, read):
        base = os.path.basename(path)
        if base in EXTRA_FILES:
            dp = path.replace("\\", "/").count("/")
            if base not in found or dp < depth[base]:
                found[base], depth[base] = read(), dp
    if os.path.isdir(source):
        for root, _dirs, files in os.walk(source):
            if root[len(source):].count(os.sep) > 3:
                continue
            for fn in files:
                p = os.path.join(root, fn)
                take(p, lambda p=p: _slurp(p))
    elif _zipfile.is_zipfile(source):
        with _zipfile.ZipFile(source) as z:
            for n in z.namelist():
                if not n.endswith("/") and z.getinfo(n).file_size < 40 * 1024 * 1024:
                    take(n, lambda n=n: z.read(n))
    if SHIM_DLL not in found or EXTRA_INI not in found or MARKER.search(found[SHIM_DLL]):
        return {}                                                   # not the mod (or it is our own shim)
    if not is_supported_extra(found[SHIM_DLL]):
        return {}                                                   # another version: only 0.2.98 is supported
    return found


def inspect_extra_source(source):
    """-> dict(path, version, azimuth) for a folder/zip that holds a usable Extra Options, else None."""
    files = _source_files(source)
    if not files:
        return None
    v, c = _ini_info(files[EXTRA_INI].decode("utf-8", "replace"))
    import hashlib
    return {"path": source, "version": v, "azimuth": c, "sig": hashlib.sha256(files[SHIM_DLL]).hexdigest()}


def import_extra_options(folder, source):
    """Copies the mod into <folder>\\fh2extra. Settings already there (ini, dat) are kept; the mod's DLL is replaced. Returns what was done."""
    files = _source_files(source)
    if not files:
        raise ShimError(f"That isn't FH2 Extra Options {EXTRA_OPTIONS_VERSION} (the only supported version).")
    dest = os.path.join(folder, EXTRA_OPTIONS_DIR)
    os.makedirs(dest, exist_ok=True)
    did = []
    for name, data in files.items():
        target = os.path.join(dest, name)
        if name != SHIM_DLL and os.path.exists(target):
            continue
        paths.atomic_write(target, data, 0o755 if name == SHIM_DLL else 0o644)
        did.append(name)
    v, _c = _ini_info(files[EXTRA_INI].decode("utf-8", "replace"))
    return [f"imported FH2 Extra Options {v}".strip()]


def find_extra_sources(azimuth_folders=()):
    """Where the player already has the mod: other Azimuth folders (fh2extra or next to azimuth.exe), and zips/folders in Downloads."""
    cands = []
    for f in azimuth_folders:
        cands += [os.path.join(f, EXTRA_OPTIONS_DIR), f]
    dl = os.path.expanduser("~/Downloads")
    try:
        cands += [os.path.join(dl, n) for n in sorted(os.listdir(dl)) if "extra" in n.lower() and "option" in n.lower()]
    except OSError:
        pass
    out, seen = [], set()
    for c in cands:
        r = os.path.realpath(c)
        if r in seen or not os.path.exists(c):
            continue
        seen.add(r)
        info = inspect_extra_source(c)
        if info and info["sig"] not in {o["sig"] for o in out}:
            out.append(info)
    out.sort(key=lambda i: [int(x) for x in re.findall(r"\d+", i["version"])] or [0], reverse=True)
    return out


BUNDLED_EXTRA_FILES = ("winmm.dll", EXTRA_INI, "LICENSE.txt", "NOTICE.txt")


def install_bundled_extra(folder, zip_path, expect_dll_sha256=None, version=""):
    """Installs the Extra Options package from our release (fh2-extra-options-<ver>.zip) into the fh2extra folder of the Azimuth folder, but only when the mod is
    not there yet. The mod's DLL is checked against the signed manifest. Settings that exist are kept. -> list of things done."""
    if extra_options_supported(folder):
        return []
    with _zipfile.ZipFile(zip_path) as z:
        names = set(z.namelist())
        if not set(BUNDLED_EXTRA_FILES) <= names:
            raise ShimError("the Extra Options package is not what was expected")
        files = {n: z.read(n) for n in BUNDLED_EXTRA_FILES}
        sources = {n: z.read(n) for n in names if re.fullmatch(r"source/[A-Za-z0-9._-]+\.zip", n) and z.getinfo(n).file_size < 20 * 1024 * 1024}
    dll = files["winmm.dll"]
    if expect_dll_sha256 and hashlib.sha256(dll).hexdigest() != expect_dll_sha256.lower():
        raise ShimError("the Extra Options DLL does not match the signed checksum")
    if MARKER.search(dll) or dll[:2] != b"MZ" or not is_supported_extra(dll):
        raise ShimError(f"that is not FH2 Extra Options {EXTRA_OPTIONS_VERSION}")
    dest = os.path.join(folder, EXTRA_OPTIONS_DIR)
    os.makedirs(os.path.join(dest, "source"), exist_ok=True)
    did = []
    old_dll = os.path.join(dest, SHIM_DLL)
    if os.path.isfile(old_dll):                      # another version: only the supported one works with the shim; keep theirs aside
        n = 1
        while os.path.exists(f"{old_dll}.unsupported-{n}"):
            n += 1
        try:
            os.replace(old_dll, f"{old_dll}.unsupported-{n}")
        except OSError as e:
            raise ShimError(f"could not replace the old FH2 Extra Options: {e.strerror or e}. Is the game still running?") from None
        did.append(f"moved your previous FH2 Extra Options aside (winmm.dll.unsupported-{n})")
    for name in BUNDLED_EXTRA_FILES:
        target = os.path.join(dest, name)
        if name in (EXTRA_INI,) and os.path.exists(target):
            continue
        paths.atomic_write(target, files[name], 0o755 if name == SHIM_DLL else 0o644)
    for name, data in sources.items():
        paths.atomic_write(os.path.join(dest, name), data, 0o644)
    v = version or _ini_info(files[EXTRA_INI].decode("utf-8", "replace"))[0]      # the ini of 0.2.98 still says v0.2.88
    return did + [f"installed FH2 Extra Options {v} (RokkuDayo, GPL-3.0)"]
