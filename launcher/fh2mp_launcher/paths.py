"""Per-user folders without Qt (so the core is testable headless)."""
import os
import sys


def is_dev():
    """Dev launcher (--dev / FH2MP_DEV=1): its own settings, key and cache, so it never touches the player's real launcher state."""
    return os.environ.get("FH2MP_DEV") == "1"


def config_dir():
    if os.environ.get("FH2MP_LAUNCHER_HOME"):
        d = os.environ["FH2MP_LAUNCHER_HOME"]
    elif sys.platform == "win32":
        d = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "FH2MP-Dev" if is_dev() else "FH2MP")
    elif sys.platform == "darwin":
        d = os.path.expanduser("~/Library/Application Support/" + ("FH2MP-Dev" if is_dev() else "FH2MP"))
    else:
        d = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "fh2mp-dev" if is_dev() else "fh2mp")
    os.makedirs(d, exist_ok=True)
    return d


def cache_dir():
    if os.environ.get("FH2MP_LAUNCHER_HOME"):
        d = os.path.join(os.environ["FH2MP_LAUNCHER_HOME"], "cache")
    elif sys.platform == "win32":
        d = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "FH2MP-Dev" if is_dev() else "FH2MP", "cache")
    else:
        d = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "fh2mp-dev" if is_dev() else "fh2mp")
    os.makedirs(d, exist_ok=True)
    return d


def atomic_write(path, data, mode=0o600, busy_ok=False):
    """Write-then-rename so a crash never leaves half a file; the file is private to the user from the first byte.
    busy_ok: on Windows a DLL a running process has loaded cannot be replaced ("Access is denied") but can be renamed, so the old file is
    renamed to <path>.old<pid> first and the new one takes its place (the running game keeps its copy until it restarts)."""
    tmp = f"{path}.tmp{os.getpid()}"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        try:
            os.replace(tmp, path)
        except PermissionError:
            if not (busy_ok and sys.platform == "win32" and os.path.exists(path)):
                raise
            aside = f"{path}.old{os.getpid()}"
            if os.path.exists(aside):
                os.unlink(aside)
            os.replace(path, aside)
            try:
                os.replace(tmp, path)
            except BaseException:
                if not os.path.exists(path):
                    os.replace(aside, path)
                raise
            try:
                os.unlink(aside)
            except OSError:
                pass                 # still loaded: it goes with the next install
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
