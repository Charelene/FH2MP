"""The FH2MP shim in the Azimuth folder: what is installed, putting a new one in, the FH2 Mod Platform it runs on, FH2 Extra Options.

Layout (the same as every FH2 Mod Platform mod):
    d3dcompiler_47.dll          FH2 Mod Platform (Trinculo54, MIT): loads every DLL in fh2-mods
    fh2-mods\\FH2MP.dll          the shim
    winmm.dll                   FH2 Extra Options (RokkuDayo, GPL-3.0), with its ini
    mods\\Forza Horizon 2\\       Extra Options' packages (0.6.0 on; up to 0.4.4 they were in mods\\)
The platform and Extra Options come from their authors' pinned GitHub releases (upstream.py). Up to 1.0.0-rc5 the shim itself was winmm.dll
and chain-loaded Extra Options from fh2extra\\; install() moves that layout over.
"""
import hashlib
import os
import re
import shutil

from . import logs, paths, upstream
from .constants import AZIMUTH_TLS_HELPERS, AZIMUTH_VERSION, BUILTIN_AZIMUTH_BUILDS, UPSTREAM
from .constants import (EXTRA_MODS_DIR, EXTRA_MODS_RESERVED, EXTRA_OPTIONS_DIR, EXTRA_OPTIONS_DLL, EXTRA_OPTIONS_DLL_SHA256, EXTRA_OPTIONS_VERSION,
                        LEGACY_SHIM_DLL, LSP_FILE, MODS_DIR, PLATFORM_DLL, PLATFORM_DLL_SHA256, PLATFORM_FILES, PLATFORM_LOADERS, PLATFORM_VERSION,
                        RETIRED_EXTRA_OPTIONS, SESSION_FILE, SHIM_DLL)

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
    except OSError as e:
        logs.swallowed("shim", "shim.unreadable", e, effect="counts as not installed")
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
    """Both loader names and every case variant must match the supplied build."""
    pk, pv = platform_installed(folder)
    if pk != "platform" or pv != PLATFORM_VERSION:
        return False
    try:
        for loader in PLATFORM_LOADERS:
            candidates = _ci_paths(folder, loader)
            if not candidates:
                return False
            for path in candidates:
                with open(path, 'rb') as f:
                    if hashlib.sha256(f.read()).hexdigest() != PLATFORM_DLL_SHA256:
                        return False
    except OSError:
        return False
    return True


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
    """Puts the exact supplied platform in place, preserving a mismatched loader."""
    if platform_ok(folder):
        return []
    pk, pv = platform_installed(folder)
    did = []
    # Inspect both names: an old compiler loader can take precedence over DWrite.
    # Keep every displaced file, including case variants on Linux.
    for loader in PLATFORM_LOADERS:
        for path in _ci_paths(folder, loader):
            with open(path, "rb") as f:
                ours = bool(platform_version_of(f.read()))
            if ours and os.path.normcase(path) == os.path.normcase(os.path.join(folder, loader)):
                saved = path + '.before-fh2mp-setup'
                n = 1
                while os.path.lexists(saved):
                    n += 1
                    saved = f"{path}.before-fh2mp-setup-{n}"
                shutil.copy2(path, saved)
            else:
                saved = _move_aside(path, "old")
            did.append(f"kept the previous {os.path.basename(path)} as {os.path.basename(saved)}")
    for name in PLATFORM_FILES:
        paths.atomic_write(os.path.join(folder, name), files[name], 0o755 if name in PLATFORM_LOADERS else 0o644, busy_ok=True)
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
        raise ShimError(f"The FH2 Mod Platform {PLATFORM_VERSION} (DWrite.dll and d3dcompiler_47.dll) is not installed.")
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


def write_session(folder, xuid, token, lsp_url, gamedb="", run="", switches=()):
    """The hand-over to the shim: who signed in, how to reach the server, (gamedb) the car database it synced, relative to the Azimuth folder,
    which the shim puts in the game's mediaoverride folder and hashes for the server, (run) the id that ties the logs of this game start
    together (logs.py) and (switches) the NO_ kill switches of the hotfix document (hotfix.py, shim/src/session_switches.inc). Private to the
    user; removed when the game ends."""
    d = bin_dir(folder)
    text = (f"xuid={xuid:x}\ntoken={token}\n" + (f"gamedb={gamedb}\n" if gamedb else "") + (f"run={run}\n" if run else "")
            + (f"switches={','.join(switches)}\n" if switches else ""))
    paths.atomic_write(os.path.join(d, SESSION_FILE), text.encode(), 0o600)
    paths.atomic_write(os.path.join(d, LSP_FILE), (lsp_url.strip() + "\n").encode(), 0o644)


def write_identity(folder, xuid):
    """The shim's fallback id for a game started without the launcher's session (Azimuth's own window, Steam): the account's, so the save profile
    stays the same. The shim keeps the low 32 bits."""
    paths.atomic_write(os.path.join(bin_dir(folder), "fh2mp-identity.txt"), f"{xuid & 0xFFFFFFFF:08x}\n".encode(), 0o644)


ONLINE_NOTE_FILE = "fh2mp-online-setup.txt"   # written by the shim when this game run never started its online setup (shim/src/online_setup.inc)
OLD_RESTART_FILE = "fh2mp-restart.txt"        # the same note from shims before 2026-11-01 (they asked for a restart; nothing restarts now)


def take_online_setup_note(folder):
    """The shim's note that this game run never started its online setup, removed on reading -> its text, or '' when there is none.
    Diagnosis only: the launcher shows it and never restarts the game."""
    text = ""
    for name in (ONLINE_NOTE_FILE, OLD_RESTART_FILE):
        path = os.path.join(bin_dir(folder), name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = text or f.read().strip() or "online setup did not start"
        except OSError:
            continue
        try:
            os.unlink(path)
        except OSError:
            pass
    return text


def online_setup_note_present(folder):
    return os.path.isfile(os.path.join(bin_dir(folder), ONLINE_NOTE_FILE))


def clear_session(folder):
    try:
        os.unlink(os.path.join(bin_dir(folder), SESSION_FILE))
    except FileNotFoundError:
        pass
    except OSError as e:          # the token stays on disk and the next game start may go online without Play, until the 30-minute limit in the shim
        logs.swallowed("play", "session.not_removed", e)


# ---- FH2 Extra Options (RokkuDayo): winmm.dll next to azimuth.exe, where Azimuth loads it and the FH2 Mod Platform recognises it.

EXTRA_INI = "FH2_ExtraOptions.ini"


def _ini_info(text):
    v = re.search(r"(?:Release|Version) v([0-9][0-9.]*)", text, re.I)
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
        with open(_extra_dll(folder), 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() == UPSTREAM['extraOptions']['dllSha256']:
                from .constants import AZIMUTH_VERSION
                return True, UPSTREAM['extraOptions']['version'], AZIMUTH_VERSION
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
    path = os.path.join(folder, TLS_HELPER)
    if os.path.isfile(path):
        # Modern Azimuth no longer loads the shared helper. Preserve a leftover
        # from an in-place/manual upgrade instead of demanding an obsolete file.
        try:
            with open(os.path.join(folder, "azimuth.exe"), "rb") as f:
                modern = hashlib.sha256(f.read()).hexdigest() == BUILTIN_AZIMUTH_BUILDS[0]["sha256"]
            if modern:
                for name, expected in AZIMUTH_TLS_HELPERS.items():
                    helper = os.path.join(os.path.dirname(path), name)
                    with open(helper, "rb") as f:
                        if hashlib.sha256(f.read()).hexdigest() != expected:
                            raise ShimError(f"Azimuth's helper does not match the supported installation: {helper}. "
                                            f"Reinstall Azimuth {AZIMUTH_VERSION} using launcher setup, then press Play again.")
                if os.path.islink(path):
                    raise ShimError(f"Azimuth's obsolete helper is a symbolic link: {path}.")
                moved = path + ".replaced"
                n = 1
                while os.path.lexists(moved):
                    n += 1
                    moved = f"{path}.replaced-{n}"
                shutil.copy2(path, moved)
                os.unlink(path)
                return [f"retired Azimuth's obsolete shared helper (kept as {os.path.basename(moved)})"]
        except OSError as e:
            raise ShimError(f"Could not retire Azimuth's obsolete helper: {e}. "
                            f"Reinstall Azimuth {AZIMUTH_VERSION} using launcher setup, then press Play again.") from None
    if not tls_helper_modified(folder):
        return []
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


def extra_options_version(folder):
    return UPSTREAM[upstream.extra_options_kind(folder)]["version"]


def is_supported_extra(dll_bytes, folder=None):
    allowed = SUPPORTED_EXTRA_SHA256 | {UPSTREAM['extraOptions']['dllSha256']}
    return hashlib.sha256(dll_bytes).hexdigest() in allowed


def extra_options_supported(folder):
    """The winmm.dll must match the pinned release for this Azimuth version."""
    path = _extra_dll(folder)
    try:
        with open(path, "rb") as f:
            return is_supported_extra(f.read(), folder)
    except (OSError, TypeError):
        return False


def extra_options_complete(folder):
    """The supported build is next to azimuth.exe with its settings file (the ini is where the launcher turns the media overrides on)."""
    if not extra_options_supported(folder):
        return False
    pin = UPSTREAM[upstream.extra_options_kind(folder)]
    if not all(os.path.isfile(os.path.join(folder, name)) for name in pin['files'].values()):
        return False
    try:
        for name, digest in pin.get('fileSha256', {}).items():
            with open(os.path.join(folder, name), 'rb') as f:
                if hashlib.sha256(f.read()).hexdigest() != digest:
                    return False
    except OSError:
        return False
    return True


def _sha256_file(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def _free_name(path):
    """`path`, or path-2, path-3, ... : the first name nothing is at yet."""
    n, dest = 1, path
    while os.path.lexists(dest):
        n += 1
        dest = f"{path}-{n}"
    return dest


def install_extra(folder, files):
    """Installs FH2 Extra Options from its release (`files` = upstream.fetch("extraOptions"): winmm.dll, its default ini and its FH2 packages
    in mods\\Forza Horizon 2) unless the supported build is there already. Moves the rc5 layout over first. The winmm.dll it replaces is kept in
    fh2extra: as winmm.dll.0.4.4 when it is the build earlier launchers installed, else as winmm.dll.unsupported-N. Settings that exist are
    kept; the 0.4.4 packages FH2MP put at the mods root are moved aside (retire_old_extra_packages), and a mods folder Extra Options would not
    load is put right where nothing is lost (eo_layout_repair). -> list of things done."""
    try:
        dll = files[EXTRA_OPTIONS_DLL]
        if MARKER.search(dll) or dll[:2] != b"MZ" or not is_supported_extra(dll, folder):
            raise ShimError(f"that is not FH2 Extra Options {extra_options_version(folder)}")
        did = migrate_legacy(folder)
        for name, data in files.items():
            if name not in (EXTRA_OPTIONS_DLL, EXTRA_INI):
                target = os.path.join(folder, name)
                if os.path.exists(target):
                    # Preserve preferences, but refresh the bundled plugin's code
                    # and metadata when upgrading between host-specific builds.
                    if name.lower().endswith('.ini'):
                        continue
                    with open(target, 'rb') as f:
                        if f.read() == data:
                            continue
                    shutil.copy2(target, target + '.before-fh2mp-setup')
                os.makedirs(os.path.dirname(target), exist_ok=True)
                paths.atomic_write(target, data, 0o644)
                did.append(f"installed {name}")
        did += retire_old_extra_packages(folder)
        if extra_options_supported(folder):            # it was in fh2extra (rc5 layout), or only its settings file is missing
            if EXTRA_INI in files and not os.path.exists(os.path.join(folder, EXTRA_INI)):
                paths.atomic_write(os.path.join(folder, EXTRA_INI), swap_stick_bindings_bytes(files[EXTRA_INI])[0], 0o644)
                did.append("restored the FH2 Extra Options settings file")
            return did + eo_layout_repair(folder)[0]
        old = _extra_dll(folder)
        updated = ""
        if old:
            retired = _sha256_file(old) == RETIRED_EXTRA_OPTIONS["dllSha256"]
            dest = os.path.join(folder, EXTRA_OPTIONS_DIR, EXTRA_OPTIONS_DLL)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if retired:
                kept = _free_name(f"{dest}.{RETIRED_EXTRA_OPTIONS['version']}")
                updated = RETIRED_EXTRA_OPTIONS["version"]
            else:
                n = 1
                while os.path.exists(f"{dest}.unsupported-{n}"):
                    n += 1
                kept = f"{dest}.unsupported-{n}"
            os.replace(old, kept)
            did.append(f"kept your previous winmm.dll as {EXTRA_OPTIONS_DIR}\\{os.path.basename(kept)}")
        paths.atomic_write(os.path.join(folder, EXTRA_OPTIONS_DLL), dll, 0o755)
        if EXTRA_INI in files and not os.path.exists(os.path.join(folder, EXTRA_INI)):
            paths.atomic_write(os.path.join(folder, EXTRA_INI), swap_stick_bindings_bytes(files[EXTRA_INI])[0], 0o644)
        did += eo_layout_repair(folder)[0]
    except OSError as e:
        raise ShimError(f"could not install FH2 Extra Options: {e.strerror or e}. Is the game still running?") from None
    if updated:
        return did + [f"updated FH2 Extra Options {updated} to {extra_options_version(folder)} (RokkuDayo, GPL-3.0) from its GitHub release; "
                      "your mods move to mods\\Forza Horizon 2 when the game starts"]
    return did + [f"installed FH2 Extra Options {extra_options_version(folder)} (RokkuDayo, GPL-3.0) from its GitHub release"]


def retire_old_extra_packages(folder):
    """Extra Options 0.4.4's bundled packages at the mods root (Garage Revving, Weather Control), which launchers up to 1.3.2 installed there:
    0.6.0 has its own copies in mods\\Forza Horizon 2, so when it moves the root folders over on its first start it leaves these behind
    (it never overwrites). Each one whose DLL and mod.json are exactly the 0.4.4 files moves to fh2extra\\mods-0.4.4\\<name>, with the player's
    settings in it; one the player changed stays and is logged. -> list of things done."""
    did = []
    for name, digests in RETIRED_EXTRA_OPTIONS["packages"].items():
        src = os.path.join(folder, "mods", name)
        if not os.path.isdir(src) or os.path.islink(src):
            continue
        try:
            same = all(_sha256_file(os.path.join(src, *rel.split("/"))) == digest for rel, digest in digests.items())
        except OSError:
            same = False
        if not same:
            logs.warn("extra", "extra.old_package_kept", package=name, why="its files are not the ones Extra Options 0.4.4 shipped",
                      effect="Extra Options 0.6.0 loads its own copy from mods\\Forza Horizon 2; this one stays at the mods root, unused")
            continue
        dest = _free_name(os.path.join(folder, EXTRA_OPTIONS_DIR, f"mods-{RETIRED_EXTRA_OPTIONS['version']}", name))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        os.replace(src, dest)
        did.append(f"moved Extra Options {RETIRED_EXTRA_OPTIONS['version']}'s {name} to {os.path.relpath(dest, folder)}")
    return did


EO_OLD_ORDER_SUFFIX = ".before-fh2mp-0.6.0"


def eo_layout_repair(folder):
    """Extra Options 0.6.0 moves mods\\loadorder.txt and the package folders at the mods root into mods\\Forza Horizon 2 on its first start. When
    both lists exist (a player went 0.6.0 -> 0.4.4 -> 0.6.0: 0.4.4 wrote a new root list) it stops with "layout failure; loader disabled" and
    loads no mods at all. When every package folder at the root is already in mods\\Forza Horizon 2, nothing would move, so the root list is
    renamed to loadorder.txt.before-fh2mp-0.6.0 and Extra Options goes on with its own. Otherwise both stay for the player to sort out.
    -> (list of things done, a note for the player or "")."""
    root = os.path.join(folder, "mods")
    fh2 = os.path.join(folder, *EXTRA_MODS_DIR.split("/"))
    order, fh2_order = os.path.join(root, "loadorder.txt"), os.path.join(fh2, "loadorder.txt")
    if not (os.path.isfile(order) and os.path.isfile(fh2_order)):
        return [], ""
    try:
        reserved = {n.lower() for n in EXTRA_MODS_RESERVED}
        left = sorted(n for n in os.listdir(root) if os.path.isdir(os.path.join(root, n)) and n.lower() not in reserved
                      and not os.path.isdir(os.path.join(fh2, n)))
    except OSError as e:
        raise ShimError(f"could not read your mods folder: {e.strerror or e}") from None
    if left:
        logs.warn("extra", "extra.two_load_orders", root=order, fh2=fh2_order, not_moved=",".join(left[:8]), count=len(left),
                  effect="Extra Options loads no mods until one list goes; FH2MP leaves both, a package would be lost otherwise")
        return [], (f"Extra Options found two mod lists (mods\\loadorder.txt and mods\\Forza Horizon 2\\loadorder.txt) and loads no mods until "
                    f"one goes. {len(left)} mod folder(s) are still only in mods, e.g. {left[0]}: move them into mods\\Forza Horizon 2, then "
                    "delete mods\\loadorder.txt")
    try:
        dest = _free_name(order + EO_OLD_ORDER_SUFFIX)
        os.replace(order, dest)
    except OSError as e:
        raise ShimError(f"could not rename mods\\loadorder.txt: {e.strerror or e}. Is the game still running?") from None
    logs.info("extra", "extra.load_order_renamed", kept=dest, why="mods\\Forza Horizon 2 has its own list and every root package",
              effect="Extra Options 0.6.0 loads mods\\Forza Horizon 2 with its own list")
    return [f"renamed the second mod list to mods\\{os.path.basename(dest)}: Extra Options uses mods\\Forza Horizon 2\\loadorder.txt"], ""


# ---- Controls: HonkHonk honks on the right stick (R3, as in the Forza games), so Extra Options' toggles move to the left stick.

STICK_KEYS = ("HeadlightsXbox", "RoofXbox")
_STICK_RE = re.compile(rb"^(" + b"|".join(k.encode() for k in STICK_KEYS) + rb")([ \t]*=[ \t]*)RightStick([ \t]*\r?)$", re.I | re.M)


def swap_stick_bindings_bytes(data):
    """The Extra Options settings file `data` with the headlight and roof toggles on LeftStick wherever they are still the default RightStick
    (any letter case); every other byte, line ending and comment stays. -> (bytes, number of keys changed)."""
    return _STICK_RE.subn(rb"\1\2LeftStick\3", data)


def swap_stick_bindings(folder):
    """swap_stick_bindings_bytes() on the FH2_ExtraOptions.ini in `folder`, written back only when something changed.
    -> (ini found, list of things done)."""
    path = os.path.join(folder, EXTRA_INI)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return False, []
    except OSError as e:
        raise ShimError(f"could not read the FH2 Extra Options settings: {e.strerror or e}") from None
    new, n = swap_stick_bindings_bytes(data)
    if not n:
        return True, []
    try:
        shutil.copy2(path, path + ".before-fh2mp-controls")
        paths.atomic_write(path, new, 0o644)
    except OSError as e:
        raise ShimError(f"could not update the FH2 Extra Options settings: {e.strerror or e}. Is the game still running?") from None
    return True, [f"moved the Extra Options headlight/roof buttons to the left stick ({n} setting(s)); the right stick is the horn"]


_UNSTICK_RE = re.compile(rb"^(" + b"|".join(k.encode() for k in STICK_KEYS) + rb")([ \t]*=[ \t]*)LeftStick([ \t]*\r?)$", re.I | re.M)


def restore_stick_bindings_bytes(data):
    """The reverse of swap_stick_bindings_bytes(): the headlight and roof toggles back on RightStick where they are LeftStick; every other byte
    stays. -> (bytes, number of keys changed)."""
    return _UNSTICK_RE.subn(rb"\1\2RightStick\3", data)


def restore_stick_bindings(folder):
    """restore_stick_bindings_bytes() on the FH2_ExtraOptions.ini in `folder` (the file is kept as .before-fh2mp-restore first).
    -> (ini found, list of things done)."""
    path = os.path.join(folder, EXTRA_INI)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return False, []
    except OSError as e:
        raise ShimError(f"could not read the FH2 Extra Options settings: {e.strerror or e}") from None
    new, n = restore_stick_bindings_bytes(data)
    if not n:
        return True, []
    try:
        shutil.copy2(path, path + ".before-fh2mp-restore")
        paths.atomic_write(path, new, 0o644)
    except OSError as e:
        raise ShimError(f"could not update the FH2 Extra Options settings: {e.strerror or e}. Is the game still running?") from None
    return True, [f"put the Extra Options headlight/roof buttons back on the right stick ({n} setting(s)); the horn stayed on the left stick"]


CONTROLS_NOTE_FILE = "fh2mp-controls.txt"    # written by the shim each game once HonkHonk is found (shim/src/honk_sync.inc honk_place_button)


def read_controls_note(folder):
    """Where the last game put HonkHonk's horn button -> {"horn": "RightStick" | "LeftStick", "movable": bool, "why": str}, or None when the
    shim did not say (no game yet, HonkHonk not loaded, an older shim). movable False: the shim could not put the horn on the right stick."""
    try:
        with open(os.path.join(bin_dir(folder), CONTROLS_NOTE_FILE), "r", encoding="utf-8", errors="replace") as f:
            text = f.read(4096)
    except OSError:
        return None
    fields = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    horn = fields.get("horn", "").strip()
    if horn not in ("RightStick", "LeftStick"):
        return None
    return {"horn": horn, "movable": fields.get("movable", "").strip() == "1", "why": fields.get("why", "").strip()}


# ---- HonkHonk (suderek22): fh2-mods\HonkHonk.dll, its bank folder and settings, loaded by the platform next to the shim.

HONK_DLL = "fh2-mods/HonkHonk.dll"
HONK_INI = "fh2-mods/HonkHonk.ini"
HONK_BANK = "fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank"


def honk_version():
    return UPSTREAM["honkHonk"]["version"]


def _honk_path(folder, rel):
    return os.path.join(folder, *rel.split("/"))


HONK_LOCAL_MARK = "fh2-mods/HonkHonk.dll.local"


def _honk_local_sha(folder):
    """The SHA-256 the player wrote into fh2-mods\\HonkHonk.dll.local to keep their own HonkHonk build (a fork), "" when there is none."""
    try:
        with open(_honk_path(folder, HONK_LOCAL_MARK), encoding="ascii") as f:
            return f.read().strip().lower()
    except OSError:
        return ""


def honk_complete(folder):
    """The pinned HonkHonk DLL and horn bank are in place (the settings file is the player's and only has to exist). A DLL whose hash is
    written in fh2-mods\\HonkHonk.dll.local counts as in place too: a local build of HonkHonk the player keeps on purpose."""
    pin = UPSTREAM["honkHonk"]
    if not os.path.isfile(_honk_path(folder, HONK_INI)):
        return False
    local = _honk_local_sha(folder)
    try:
        for rel, digest in pin["fileSha256"].items():
            with open(_honk_path(folder, rel), "rb") as f:
                have = hashlib.sha256(f.read()).hexdigest()
            if have != digest and not (rel == HONK_DLL and local and have == local):
                return False
    except OSError:
        return False
    return True


def honk_installed(folder):
    """-> (installed, version): the pinned version when the DLL is the pinned build, "unknown" for another HonkHonk.dll, ("", "") when missing."""
    path = _honk_path(folder, HONK_DLL)
    try:
        with open(path, "rb") as f:
            same = hashlib.sha256(f.read()).hexdigest() == UPSTREAM["honkHonk"]["dllSha256"]
    except OSError:
        return False, ""
    return True, honk_version() if same else "unknown"


def install_honk(folder, files):
    """Installs HonkHonk from its release (`files` = upstream.fetch("honkHonk")). The DLL and the bank are always the pinned bytes (a different
    file there is kept as <name>.before-fh2mp-setup); the player's HonkHonk.ini is kept. -> list of things done."""
    pin = UPSTREAM["honkHonk"]
    did = []
    try:
        for rel, data in files.items():
            if rel not in pin["files"].values():
                continue
            target = _honk_path(folder, rel)
            if rel == HONK_INI:
                if os.path.exists(target):
                    continue
            elif os.path.exists(target):
                with open(target, "rb") as f:
                    if f.read() == data:
                        continue
                saved = target + ".before-fh2mp-setup"
                n = 1
                while os.path.lexists(saved):
                    n += 1
                    saved = f"{target}.before-fh2mp-setup-{n}"
                shutil.copy2(target, saved)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            paths.atomic_write(target, data, 0o755 if rel == HONK_DLL else 0o644, busy_ok=True)
            did.append(f"installed {rel}")
    except OSError as e:
        raise ShimError(f"could not install HonkHonk: {e.strerror or e}. Is the game still running?") from None
    return did + ([f"installed HonkHonk {honk_version()} (suderek22, GPL-3.0) from its GitHub release"] if did else [])

# ---- FH2 ANNA (qiangqiang101): fh2-mods\FH2ANNA.dll, loaded by the platform next to the shim; its settings file (FH2ANNA.ini) is its own.

ANNA_DLL = "fh2-mods/FH2ANNA.dll"


def anna_version():
    return UPSTREAM["anna"]["version"]


def anna_complete(folder):
    """The pinned FH2 ANNA DLL is in place."""
    try:
        with open(os.path.join(folder, *ANNA_DLL.split("/")), "rb") as f:
            return hashlib.sha256(f.read()).hexdigest() == UPSTREAM["anna"]["dllSha256"]
    except OSError:
        return False


def anna_installed(folder):
    """-> (installed, version): the pinned version when the DLL is the pinned build, "unknown" for another FH2ANNA.dll, (False, "") when missing."""
    try:
        with open(os.path.join(folder, *ANNA_DLL.split("/")), "rb") as f:
            same = hashlib.sha256(f.read()).hexdigest() == UPSTREAM["anna"]["dllSha256"]
    except OSError:
        return False, ""
    return True, anna_version() if same else "unknown"


def install_anna(folder, files):
    """Installs FH2 ANNA from its release (`files` = upstream.fetch("anna"): the DLL only). A different FH2ANNA.dll there is kept as
    FH2ANNA.dll.before-fh2mp-setup. -> list of things done."""
    data = files.get(ANNA_DLL)
    if data is None:
        raise ShimError("the FH2 ANNA release has no FH2ANNA.dll")
    target = os.path.join(folder, *ANNA_DLL.split("/"))
    try:
        if os.path.exists(target):
            with open(target, "rb") as f:
                if f.read() == data:
                    return []
            saved = target + ".before-fh2mp-setup"
            n = 1
            while os.path.lexists(saved):
                n += 1
                saved = f"{target}.before-fh2mp-setup-{n}"
            shutil.copy2(target, saved)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        paths.atomic_write(target, data, 0o755, busy_ok=True)
    except OSError as e:
        raise ShimError(f"could not install FH2 ANNA: {e.strerror or e}. Is the game still running?") from None
    return [f"installed FH2 ANNA {anna_version()} (qiangqiang101, MIT) from its GitHub release"]


# ---- Extra Options' aero unlock ([Experiments] UnlockAeroTuning). With it on, Extra Options gives every stock aero part its car's race aero physics at game
# start (EO 0.4.4 winmm.dll, read statically 2026-11-09: one switch for all six aero patches, read once at start, nothing kept between runs; only SELECTs on the
# game database). The game's PI ignores the tune, so the car's PI goes up even at default tuning, and online events refused such cars ("your car is class
# S2 989, the event needs class S1", 2026-11-09). An online game starts with it at 0; the player's value comes back for offline play and after the game.
# With it at 0 a car tuned while it was on loads its stock aero physics and its stock PI; the aero values stay in its save.
AERO_KEY = "UnlockAeroTuning"
_AERO_RE = re.compile(rb"^(UnlockAeroTuning[ \t]*=[ \t]*)([^\s;]*)([ \t]*\r?)$", re.I | re.M)


def aero_unlock_bytes(data, value):
    """`data` (the Extra Options settings file) with every UnlockAeroTuning line set to `value` (bytes); every other byte stays.
    -> (bytes, the first line's previous value as bytes, or None when the file has no such line)."""
    found = _AERO_RE.search(data)
    if not found:
        return data, None
    return _AERO_RE.sub(lambda m: m.group(1) + value + m.group(3), data), found.group(2)


def _rewrite_extra_ini(folder, change, backup):
    """change(bytes) -> (new bytes, result) on the FH2_ExtraOptions.ini in `folder`, written (after a copy to .<backup>) only when it changed.
    -> (ini found, result)."""
    path = os.path.join(folder, EXTRA_INI)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return False, None
    except OSError as e:
        raise ShimError(f"could not read the FH2 Extra Options settings: {e.strerror or e}") from None
    new, result = change(data)
    if new != data:
        try:
            shutil.copy2(path, path + backup)
            paths.atomic_write(path, new, 0o644)
        except OSError as e:
            raise ShimError(f"could not update the FH2 Extra Options settings: {e.strerror or e}. Is the game still running?") from None
    return True, result


def aero_unlock_off(folder):
    """UnlockAeroTuning to 0 in `folder`'s Extra Options settings. -> the value it had (str) when it was something else, else ""."""
    _found, was = _rewrite_extra_ini(folder, lambda data: aero_unlock_bytes(data, b"0"), ".before-fh2mp-aero")
    was = (was or b"").decode("ascii", "replace").strip()
    return "" if was in ("", "0") else was


def aero_unlock_restore(folder, value):
    """UnlockAeroTuning back to `value` where it is still 0 (a value the player set since is kept). -> True when the file was changed."""
    def change(data):
        new, was = aero_unlock_bytes(data, value.encode("ascii"))
        return (new, True) if was is not None and was.strip() == b"0" else (data, False)
    _found, done = _rewrite_extra_ini(folder, change, ".before-fh2mp-aero-restore")
    return bool(done)


# ANNA opens on D-pad down and moves up its list with D-pad up, which are Extra Options' default autodrive and hazard buttons: with both, opening
# ANNA also starts Extra Options' autodrive (ANNA's README). Those two go to None once, only where they are still the defaults.
ANNA_EO_KEYS = {"AutodriveXbox": "DPadDown", "HazardsXbox": "DPadUp"}
_ANNA_EO_RE = re.compile(rb"^(" + b"|".join(k.encode() for k in ANNA_EO_KEYS) + rb")([ \t]*=[ \t]*)(DPadDown|DPadUp)([ \t]*\r?)$", re.I | re.M)


def anna_bindings_bytes(data):
    """The Extra Options settings file `data` with AutodriveXbox=DPadDown and HazardsXbox=DPadUp (each only with its own default) set to None;
    every other byte stays. -> (bytes, number of keys changed)."""
    defaults = {k.lower(): v.lower() for k, v in ANNA_EO_KEYS.items()}
    n = 0

    def one(m):
        nonlocal n
        if defaults[m.group(1).decode("ascii").lower()] != m.group(3).decode("ascii").lower():
            return m.group(0)                    # e.g. HazardsXbox=DPadDown: the player's own choice
        n += 1
        return m.group(1) + m.group(2) + b"None" + m.group(4)
    return _ANNA_EO_RE.sub(one, data), n


def anna_bindings(folder):
    """anna_bindings_bytes() on the FH2_ExtraOptions.ini in `folder`, written back only when something changed. -> (ini found, things done)."""
    path = os.path.join(folder, EXTRA_INI)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return False, []
    except OSError as e:
        raise ShimError(f"could not read the FH2 Extra Options settings: {e.strerror or e}") from None
    new, n = anna_bindings_bytes(data)
    if not n:
        return True, []
    try:
        shutil.copy2(path, path + ".before-fh2mp-anna")
        paths.atomic_write(path, new, 0o644)
    except OSError as e:
        raise ShimError(f"could not update the FH2 Extra Options settings: {e.strerror or e}. Is the game still running?") from None
    return True, [f"turned off Extra Options' D-pad autodrive/hazard buttons ({n} setting(s)); the D-pad opens FH2 ANNA"]


# ---- NitroSystem (qiangqiang101, MIT): fh2-mods\FnFNitro.dll. It tells FH2 Mod Platform 2.8.0 it is offline_only, so the platform does not load it
# for online play unless fh2-mods\platform-online.txt has "FnFNitro.dll<TAB>1" (the MODS tab's "Load when playing online"; the platform reads lines
# of "<dll><TAB><value>", "#" comments, the dll in any case, "0" off and anything else on [C: DWrite.dll 2.8.0 FUN_1800025e0 / FUN_1800072c0]).
# NitroSystem 2.0.0 and newer have an off switch other mods can pull (NitroSystem.Control, FnFNitro_SetBlocked); the shim keeps its NOS off
# online except in Free Roam lobbies (src/mods_sync.inc). So the launcher turns "Load when playing online" on for a NitroSystem with the switch and
# off for one without it. A value the player set on the MODS tab is theirs: only the one the launcher wrote last is changed again, except that a
# NitroSystem without the switch is always kept off online (it would burn NOS in events).
ONLINE_LIST = "platform-online.txt"
ONLINE_HEADER = "# The mods' \"Load when playing online\" on MODS > Installed mods: 1 loads it when the game is started from the FH2MP launcher, 0 doesn't."
NITRO_DLL = "FnFNitro.dll"
NITRO_SWITCH_EXPORT = b"FnFNitro_SetBlocked\x00"
UNDERGLOW_DLL = "Underglow.dll"


def _mods_txt_version(folder, dll):
    """The version the platform's MODS tab last saw for `dll` (fh2-mods\\platform-mods.txt: dll, name, version, ...), "" when not listed."""
    try:
        with open(os.path.join(folder, MODS_DIR, "platform-mods.txt"), encoding="utf-8-sig") as f:
            for line in f:
                cols = line.rstrip("\r\n").split("\t")
                if len(cols) >= 3 and cols[0].strip().lower() == dll.lower():
                    return cols[2].strip()
    except OSError:
        pass
    return ""


def nitro_installed(folder):
    """-> ("none" | "switch" | "no_switch", version as the MODS tab last saw it)."""
    path = os.path.join(folder, MODS_DIR, NITRO_DLL)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        return "none", ""
    except OSError as e:
        logs.swallowed("shim", "nitro.unreadable", e, effect="treated as a NitroSystem without its off switch")
        return "no_switch", ""
    return ("switch" if NITRO_SWITCH_EXPORT in data else "no_switch"), _mods_txt_version(folder, NITRO_DLL)


def underglow_installed(folder):
    """-> (fh2-mods\\Underglow.dll is there, version as the MODS tab last saw it)."""
    return os.path.isfile(os.path.join(folder, MODS_DIR, UNDERGLOW_DLL)), _mods_txt_version(folder, UNDERGLOW_DLL)


def online_value(folder, dll):
    """`dll`'s value in platform-online.txt ("0", "1", ...), None when it has no line."""
    try:
        with open(os.path.join(folder, MODS_DIR, ONLINE_LIST), encoding="utf-8-sig") as f:
            for line in f:
                cols = line.rstrip("\r\n").split("\t")
                if not line.startswith("#") and len(cols) >= 2 and cols[0].lower() == dll.lower():
                    return cols[1]
    except OSError:
        pass
    return None


def set_online_value(folder, dll, value):
    """Writes `dll`'s line in platform-online.txt (the others stay as they are)."""
    path = os.path.join(folder, MODS_DIR, ONLINE_LIST)
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        lines = [ONLINE_HEADER]
    except OSError as e:
        raise ShimError(f"could not read {ONLINE_LIST}: {e.strerror or e}") from None
    out, done = [], False
    for line in lines:
        cols = line.split("\t")
        if not line.startswith("#") and len(cols) >= 2 and cols[0].lower() == dll.lower():
            if not done:
                out.append(f"{cols[0]}\t{value}")
                done = True
            continue
        out.append(line)
    if not done:
        out.append(f"{dll}\t{value}")
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        paths.atomic_write(path, ("\n".join(out) + "\n").encode("utf-8-sig"), 0o644)
    except OSError as e:
        raise ShimError(f"could not write {ONLINE_LIST}: {e.strerror or e}. Is the game still running?") from None


def nitro_online(folder, written):
    """Every online Play: NitroSystem's "Load when playing online" as the rule above wants it. `written` is the value the launcher wrote last
    ("" none). -> (the value the launcher has written now, or `written` when it left the line alone; a note for the log, "" when nothing changed)."""
    kind, version = nitro_installed(folder)
    if kind == "none":
        return written, ""
    want = "1" if kind == "switch" else "0"
    have = online_value(folder, NITRO_DLL)
    name = f"NitroSystem {version}".strip()
    if have == want:
        return (want if have == written else written), ""
    if have is None or have == written or want == "0":
        set_online_value(folder, NITRO_DLL, want)
        why = ("it has the off switch: FH2MP keeps its NOS off online except in Free Roam lobbies" if want == "1" else
               "it has no off switch (2.0.0 and newer have one), so its NOS would also burn in events")
        return want, f"{name} {'loads' if want == '1' else 'stays off'} for online play ({why})"
    return written, ""          # the player turned it off for online play on the MODS tab: theirs to change
