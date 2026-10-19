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


def atomic_write(path, data, mode=0o600):
    """Write-then-rename so a crash never leaves half a file; the file is private to the user from the first byte."""
    tmp = f"{path}.tmp{os.getpid()}"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
