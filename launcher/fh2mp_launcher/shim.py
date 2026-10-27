"""The FH2MP shim in the Azimuth folder: what is installed, putting a new one in, the FH2 Mod Platform it runs on, FH2 Extra Options.

Layout (the same as every FH2 Mod Platform mod):
    d3dcompiler_47.dll          FH2 Mod Platform (Trinculo54, MIT): loads every DLL in fh2-mods
    fh2-mods\\FH2MP.dll          the shim
    winmm.dll                   FH2 Extra Options (RokkuDayo, GPL-3.0), with its ini
The platform and Extra Options come from their authors' pinned GitHub releases (upstream.py). Up to 1.0.0-rc5 the shim itself was winmm.dll
and chain-loaded Extra Options from fh2extra\\; install() moves that layout over.
"""
import hashlib
import os
import re
import shutil

from . import paths
from .constants import (EXTRA_OPTIONS_DIR, EXTRA_OPTIONS_DLL, EXTRA_OPTIONS_DLL_SHA256, EXTRA_OPTIONS_VERSION, LEGACY_SHIM_DLL, LSP_FILE,
                        MODS_DIR, PLATFORM_DLL, PLATFORM_FILES, PLATFORM_VERSION, SESSION_FILE, SHIM_DLL)

MARKER = re.compile(rb"FH2MP-SHIM-VERSION:([0-9A-Za-z.+-]{1,32}):END")
EXTRA_FILES_PREFIX = "FH2_ExtraOptions."
DISABLED_LIST = "platform-disabled.txt"          # the platform's MODS tab: DLLs the player turned off


class ShimError(Exception):
    pass


def _dll(folder):
    return os.path.join(folder, MODS_DIR, SHIM_DLL)


def _vkey(v):
    return [int(x) for x in re.findall(r"\d+", v or "")[:3]] or [0]


def inspect_dll(path):
    """-> ("ours", version) | ("foreign", "") | ("none", "")  by looking for the marker the shim carries."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return "none", ""
    m = MARKER.search(data)
    return ("ours", m.group(1).decode()) if m else ("foreign", "")


def blob_version(data):
    m = MARKER.search(data)
    return m.group(1).decode() if m else ""


def supports_offline(folder):
    """The installed shim can keep the account's save ID while skipping all online hooks."""
    try:
        with open(_dll(folder), "rb") as f:
            return "FH2MP_OFFLINE".encode("utf-16-le") in f.read()
    except OSError:
        return False


_U16 = lambda s: s.encode("utf-16-le")  # noqa: E731


def platform_version_of(data):
    """The FH2 Mod Platform version in a d3dcompiler_47.dll's version resource, '' when the DLL is not the platform."""
    if _U16("FH2 Mod Platform") not in data:
        return ""
    m = re.search(re.escape(_U16("ProductVersion")) + rb"(?:\x00\x00)+((?:[0-9.]\x00){3,32})", data)
    return m.group(1).decode("utf-16-le") if m else ""


def _ci_paths(folder, name):
    """Every file in `folder` named `name` in any letter case. Windows sees one file; Linux can hold D3DCompiler_47.dll next to
    d3dcompiler_47.dll, and Wine then loads whichever it finds first (a radio mod's loader hid the platform that way, 2026-10-23)."""
    try:
        return [os.path.join(folder, n) for n in sorted(os.listdir(folder)) if n.lower() == name.lower()]
    except OSError:
        return []


def platform_installed(folder):
    """-> ("platform", version) | ("foreign", "") | ("none", "") for the d3dcompiler_47.dll next to azimuth.exe (any letter case; when
    there are several, "foreign" unless every one is the platform)."""
    found = []
    for path in _ci_paths(folder, PLATFORM_DLL):
        try:
            with open(path, "rb") as f:
                found.append(platform_version_of(f.read()))
        except OSError:
            found.append("")
    if not found:
        return "none", ""
    return ("platform", min(found, key=_vkey)) if all(found) else ("foreign", "")


def platform_ok(folder):
    """The FH2 Mod Platform next to azimuth.exe is there and at least the version the shim is tested with."""
    pk, pv = platform_installed(folder)
    return pk == "platform" and _vkey(pv) >= _vkey(PLATFORM_VERSION)


def installed(folder):
    """-> ("ours", version) when the shim is in fh2-mods and the platform that loads it is there and new enough; ("old", version) when
    it needs installing again (the winmm.dll layout up to 1.0.0-rc5, or the platform missing or older); ("foreign", "") for a
    fh2-mods\\FH2MP.dll that is not ours; ("none", "")."""
    kind, ver = inspect_dll(_dll(folder))
    if kind == "ours":
        return ("ours", ver) if platform_ok(folder) else ("old", ver)
    if kind == "foreign":
        return kind, ver
    lk, lv = inspect_dll(os.path.join(folder, LEGACY_SHIM_DLL))
    return ("old", lv) if lk == "ours" else ("none", "")


def _move_aside(path, suffix):
    """Renames path to path.<suffix>[-n] (first free name). -> the new name."""
    n, dest = 1, f"{path}.{suffix}"
    while os.path.exists(dest):
        n += 1
        dest = f"{path}.{suffix}-{n}"
    os.replace(path, dest)
    return dest


def migrate_legacy(folder):
    """The layout up to 1.0.0-rc5 -> the platform one: our winmm.dll goes, FH2 Extra Options comes back from fh2extra\\ to its own place
    next to azimuth.exe (its settings with it; its licence, notice and source stay in fh2extra). -> list of things done."""
    did = []
    legacy = os.path.join(folder, LEGACY_SHIM_DLL)
    if inspect_dll(legacy)[0] == "ours":
        os.unlink(legacy)
        did.append("removed the old winmm.dll shim")
        for n in (LEGACY_SHIM_DLL + ".prev",):
            try:
                os.unlink(os.path.join(folder, n))
            except OSError:
                pass
    extra = os.path.join(folder, EXTRA_OPTIONS_DIR)
    if os.path.isfile(os.path.join(extra, EXTRA_OPTIONS_DLL)) and not os.path.exists(legacy):
        for name in [EXTRA_OPTIONS_DLL] + sorted(n for n in os.listdir(extra) if n.startswith(EXTRA_FILES_PREFIX)):
            src, dest = os.path.join(extra, name), os.path.join(folder, name)
            if not os.path.exists(dest):
                os.replace(src, dest)
        try:
            os.unlink(os.path.join(extra, "Gamerpic.png"))      # Extra Options now reads Azimuth's own Gamerpic.png
        except OSError:
            pass
        did.append(f"moved FH2 Extra Options from {EXTRA_OPTIONS_DIR} back next to azimuth.exe")
    return did


def _install_platform(folder, files):
    """Puts the platform in place unless the one there is the same version or newer (other mods ship it too). -> list of things done."""
    if platform_ok(folder):
        return []
    pk, pv = platform_installed(folder)
    did = []
    for path in _ci_paths(folder, PLATFORM_DLL):        # another mod's loader (fh2wheel's, a radio mod's) or an older platform
        with open(path, "rb") as f:
            if platform_version_of(f.read()):
                # Keep the canonical DLL for atomic_write's Windows loaded-file replacement. Deleting it here fails with
                # ERROR_SHARING_VIOLATION before busy_ok can rename the loaded copy out of the way.
                if os.path.normcase(path) != os.path.normcase(os.path.join(folder, PLATFORM_DLL)):
                    _move_aside(path, "old")
                continue
        moved = _move_aside(path, "old")
        did.append(f"kept the other {os.path.basename(path)} as {os.path.basename(moved)}: only one d3dcompiler_47.dll can load, and "
                   "the platform loads fh2wheel.dll itself; another mod must go in fh2-mods")
    for name in PLATFORM_FILES:
        paths.atomic_write(os.path.join(folder, name), files[name], 0o755 if name == PLATFORM_DLL else 0o644, busy_ok=True)
    did.append(f"installed FH2 Mod Platform {PLATFORM_VERSION}" + (f" (was {pv})" if pv else ""))
    return did


def enable(folder):
    """Takes FH2MP off the platform's list of mods turned off on the in-game MODS tab. -> True when it was on it."""
    path = os.path.join(folder, MODS_DIR, DISABLED_LIST)
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except OSError:
        return False
    keep = [ln for ln in lines if ln.strip().lower() != SHIM_DLL.lower()]
    if len(keep) == len(lines):
        return False
    paths.atomic_write(path, ("\n".join(keep) + "\n").encode("utf-8"), 0o644)
    return True


def disable(folder):
    """Puts FH2MP on the platform's list of mods turned off (the in-game MODS tab), so the next game start runs without it: offline play.
    enable() takes it off again. -> True when it was added."""
    path = os.path.join(folder, MODS_DIR, DISABLED_LIST)
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    if any(ln.strip().lower() == SHIM_DLL.lower() for ln in lines):
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    paths.atomic_write(path, ("\n".join([ln for ln in lines if ln.strip()] + [SHIM_DLL]) + "\n").encode("utf-8"), 0o644)
    return True


def install(folder, dll_bytes, expect_sha256=None, platform=None):
    """Puts the shim in fh2-mods and, from `platform` ({file name: bytes} of PLATFORM_FILES, upstream.fetch("platform")), the FH2 Mod
    Platform next to azimuth.exe when it is missing or older. Without `platform` a good one must be there already. Moves the old winmm.dll
    layout over first. Returns a list of things done, for the log. Raises ShimError with a message fit for the user."""
    if expect_sha256 and hashlib.sha256(dll_bytes).hexdigest() != expect_sha256.lower():
        raise ShimError("the downloaded shim does not match its checksum")
    if not blob_version(dll_bytes):
        raise ShimError("that file is not an FH2MP shim")
    if platform is None and not platform_ok(folder):
        raise ShimError(f"The FH2 Mod Platform {PLATFORM_VERSION} (d3dcompiler_47.dll) is not installed.")
    try:
        did = migrate_legacy(folder)
        if platform is not None:
            did += _install_platform(folder, platform)
        target = _dll(folder)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if inspect_dll(target)[0] == "ours":
            shutil.copy2(target, target + ".prev")
        paths.atomic_write(target, dll_bytes, 0o755, busy_ok=True)
    except OSError as e:
        where = getattr(e, "filename2", None) or getattr(e, "filename", None) or ""
        if getattr(e, "winerror", None) in (32, 33):
            raise ShimError(f"could not install the shim: {os.path.basename(where) or 'a mod DLL'} is still in use by another process. "
                            "Close the game and all Azimuth windows, then retry. If they are already closed, check Task Manager for a "
                            "remaining azimuth.exe process, or restart Windows and retry before opening Azimuth.") from None
        raise ShimError(f"could not install the shim: {e.strerror or e}" + (f" ({os.path.basename(where)} in {os.path.dirname(where)})" if where else "")
                        + ". Close the game and try again; if it keeps happening the folder may be protected (Program Files, OneDrive, "
                        "antivirus) and Azimuth should be moved elsewhere.") from None
    if enable(folder):
        did.append("turned FH2MP back on (it was off on the in-game MODS tab)")
    did.append(f"installed shim {blob_version(dll_bytes)}")
    return did


def uninstall(folder):
    """Removes the shim (and an old winmm.dll one). The platform stays: other mods may use it."""
    for path in (_dll(folder), _dll(folder) + ".prev"):
        try:
            os.unlink(path)
        except OSError:
            pass
    migrate_legacy(folder)


def bin_dir(folder):
    """Where the shim looks for its files and writes its log: data\\bin when it exists, else the Azimuth folder itself."""
    d = os.path.join(folder, "data", "bin")
    return d if os.path.isdir(d) else folder


def write_session(folder, xuid, token, lsp_url, gamedb=""):
    """The hand-over to the shim: who signed in, how to reach the server and (gamedb) the car database it synced, relative to the Azimuth
    folder, which the shim puts in the game's mediaoverride folder and hashes for the server. Private to the user; removed when the game ends."""
    d = bin_dir(folder)
    text = f"xuid={xuid:x}\ntoken={token}\n" + (f"gamedb={gamedb}\n" if gamedb else "")
    paths.atomic_write(os.path.join(d, SESSION_FILE), text.encode(), 0o600)
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


# ---- FH2 Extra Options (RokkuDayo): winmm.dll next to azimuth.exe, where Azimuth loads it and the FH2 Mod Platform recognises it.

EXTRA_INI = "FH2_ExtraOptions.ini"


def _ini_info(text):
    v = re.search(r"Release v([0-9][0-9.]*)", text)
    c = re.search(r"Compatible with Azimuth ([0-9][0-9.]*)", text)
    return (v.group(1) if v else "", c.group(1) if c else "")


def _extra_dll(folder):
    """Extra Options' winmm.dll next to azimuth.exe (any letter case), or None when what is there is missing or our old shim."""
    for path in _ci_paths(folder, EXTRA_OPTIONS_DLL):
        if inspect_dll(path)[0] == "foreign":
            return path
    return None


def extra_options_installed(folder):
    """-> (installed, mod version, Azimuth version it targets)."""
    if not _extra_dll(folder):
        return False, "", ""
    try:
        with open(os.path.join(folder, EXTRA_INI), encoding="utf-8", errors="replace") as f:
            return (True, *_ini_info(f.read()))
    except OSError:
        return True, "", ""


TLS_HELPER = os.path.join("data", "bin", "azimuth-tls.dll")


def _tls_stock(folder, path):
    """True when `path` is the helper Azimuth ships: azimuth.exe carries the SHA-256 of it (as hex text) and checks the file against it."""
    try:
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest().encode()
        with open(os.path.join(folder, "azimuth.exe"), "rb") as f:
            return digest in f.read()
    except OSError:
        raise ShimError(f"Could not verify Azimuth's helper at {path}. Check that azimuth.exe and the helper are readable.") from None


def tls_helper_modified(folder):
    """True when data\\bin\\azimuth-tls.dll is not Azimuth's own. FH2 Extra Options up to 0.2.7x was installed by replacing it (a 284 KB
    patch DLL); left in place next to 0.2.98 it patches the same game code and the same loader import a second time, and 0.2.98 stops with
    "another mod owns Azimuth's LoadLibraryExW import" or "unexpected loaded bytes at garage_xml_parse_hook" (GitHub #20)."""
    path = os.path.join(folder, TLS_HELPER)
    return os.path.isfile(path) and not _tls_stock(folder, path)


def restore_tls_helper(folder):
    """Restore a local backup only when its hash is accepted by this azimuth.exe.
    Keep both the backup and displaced helper; failed writes leave the current helper in place.
    """
    if not tls_helper_modified(folder):
        return []
    path = os.path.join(folder, TLS_HELPER)
    try:
        names = sorted(os.listdir(os.path.dirname(path)))
        pattern = re.compile(r"azimuth-tls\.dll\.(?:old|bak|backup|replaced)(?:-?\d+)?", re.I)
        backup = next((os.path.join(os.path.dirname(path), n) for n in names
                       if pattern.fullmatch(n) and os.path.isfile(os.path.join(os.path.dirname(path), n))
                       and _tls_stock(folder, os.path.join(os.path.dirname(path), n))), None)
        if backup is None:
            with open(path, "rb") as f:
                digest = hashlib.sha256(f.read()).hexdigest()
            raise ShimError(f"Azimuth's helper does not match this azimuth.exe, and no verified local backup was found. "
                            f"File: {path} (SHA-256: {digest}). "
                            "Copy data\\bin\\azimuth-tls.dll from a fresh download of the same Azimuth version into this folder, "
                            "then press Play again. A mismatched file can come from an older mod or a mixed/incomplete Azimuth install.")
        with open(backup, "rb") as f:
            replacement = f.read()
        # Verify the actual bytes being written, rather than trusting a prior read.
        with open(os.path.join(folder, "azimuth.exe"), "rb") as f:
            if hashlib.sha256(replacement).hexdigest().encode() not in f.read():
                raise ShimError(f"Azimuth's helper backup changed during verification: {backup}. Try again.")
        moved = path + ".replaced"
        n = 1
        while os.path.exists(moved):
            n += 1
            moved = f"{path}.replaced-{n}"
        shutil.copy2(path, moved)
        paths.atomic_write(path, replacement, 0o755)
    except OSError as e:
        raise ShimError(f"could not restore Azimuth's azimuth-tls.dll: {e.strerror or e}. Is the game still running?") from None
    return [f"put Azimuth's own azimuth-tls.dll back (the previous file is kept as {os.path.basename(moved)})"]


SUPPORTED_EXTRA_SHA256 = {EXTRA_OPTIONS_DLL_SHA256}      # the only build FH2MP works with (tests may add their own)


def is_supported_extra(dll_bytes):
    return hashlib.sha256(dll_bytes).hexdigest() in SUPPORTED_EXTRA_SHA256


def extra_options_supported(folder):
    """-> True when the winmm.dll next to azimuth.exe is exactly the supported FH2 Extra Options build (0.2.98)."""
    path = _extra_dll(folder)
    try:
        with open(path, "rb") as f:
            return is_supported_extra(f.read())
    except (OSError, TypeError):
        return False


def extra_options_complete(folder):
    """The supported build is next to azimuth.exe with its settings file (the ini is where the launcher turns the media overrides on)."""
    return extra_options_supported(folder) and os.path.isfile(os.path.join(folder, EXTRA_INI))


def install_extra(folder, files):
    """Installs FH2 Extra Options from its release (`files` = upstream.fetch("extraOptions"): winmm.dll and its default ini) unless the
    supported build is there already. Moves the rc5 layout over first; another winmm.dll there is kept as fh2extra\\winmm.dll.unsupported-N;
    settings that exist are kept. -> list of things done."""
    try:
        did = migrate_legacy(folder)
        if extra_options_supported(folder):            # it was in fh2extra (rc5 layout), or only its settings file is missing
            if EXTRA_INI in files and not os.path.exists(os.path.join(folder, EXTRA_INI)):
                paths.atomic_write(os.path.join(folder, EXTRA_INI), files[EXTRA_INI], 0o644)
                did.append("restored the FH2 Extra Options settings file")
            return did
        dll = files[EXTRA_OPTIONS_DLL]
        if MARKER.search(dll) or dll[:2] != b"MZ" or not is_supported_extra(dll):
            raise ShimError(f"that is not FH2 Extra Options {EXTRA_OPTIONS_VERSION}")
        old = _extra_dll(folder)
        if old:
            dest = os.path.join(folder, EXTRA_OPTIONS_DIR, EXTRA_OPTIONS_DLL)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            n = 1
            while os.path.exists(f"{dest}.unsupported-{n}"):
                n += 1
            os.replace(old, f"{dest}.unsupported-{n}")
            did.append(f"kept your previous winmm.dll as {EXTRA_OPTIONS_DIR}\\winmm.dll.unsupported-{n}")
        paths.atomic_write(os.path.join(folder, EXTRA_OPTIONS_DLL), dll, 0o755)
        if EXTRA_INI in files and not os.path.exists(os.path.join(folder, EXTRA_INI)):
            paths.atomic_write(os.path.join(folder, EXTRA_INI), files[EXTRA_INI], 0o644)
    except OSError as e:
        raise ShimError(f"could not install FH2 Extra Options: {e.strerror or e}. Is the game still running?") from None
    return did + [f"installed FH2 Extra Options {EXTRA_OPTIONS_VERSION} (RokkuDayo, GPL-3.0) from its GitHub release"]
