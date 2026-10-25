"""Whole-profile transfers. Back up and verify before replacing any save files."""
import errno
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


class SaveCopyError(OSError):
    def __init__(self, source, target, cause):
        super().__init__(f"Could not copy your save. The game has not been started.\n{source}\n→ {target}\n{cause}")
        self.source, self.target = str(source), str(target)
        self.permission = isinstance(cause, PermissionError) or getattr(cause, "errno", None) in (errno.EACCES, errno.EPERM)


class SaveScanError(SaveCopyError):
    def __init__(self, folder, xuid, others, cause):
        super().__init__(folder, f"account {xuid:X}", cause)
        self.folder, self.xuid, self.others = folder, xuid, list(others)


def _manifest(root):
    result = {}
    def visit(directory):
        for p in directory.iterdir():
            if p.is_symlink() or (hasattr(p, "is_junction") and p.is_junction()):
                raise OSError(f"Save folders cannot contain links: {p}")
            if p.is_dir():
                result[str(p.relative_to(root))] = None
                visit(p)
            elif p.is_file():
                h = hashlib.sha256()
                with p.open("rb") as f:
                    for chunk in iter(lambda: f.read(1024 * 1024), b""):
                        h.update(chunk)
                result[str(p.relative_to(root))] = h.hexdigest()
            else:
                raise OSError(f"Unsupported save entry: {p}")
    visit(root)
    return result


def _verified_copy(source, target):
    before = _manifest(source)
    shutil.copytree(source, target)
    if before != _manifest(target) or before != _manifest(source):
        raise OSError("Save verification failed, or the save changed during copying. Close Azimuth and try again.")


def copy_profile(source, target):
    """Return backup directory. Source stays intact; roll back if installing the staged copy fails."""
    source, target = Path(os.path.abspath(source)), Path(os.path.abspath(target))
    stage = None
    try:
        if not re.fullmatch(r"User_[0-9a-fA-F]+", target.name):
            raise OSError("Choose a User_<HEX> destination profile.")
        # Reject links in all ancestors, including for the elevated helper.
        for path in (source, target):
            for p in (path, *path.parents):
                if p.is_symlink() or (hasattr(p, "is_junction") and p.is_junction()):
                    raise OSError(f"Save paths cannot contain links: {p}")
        if source == target or source in target.parents or target in source.parents:
            raise OSError("Choose separate source and destination profiles.")
        if not (source / "revisions").is_dir() or not any(v is not None for v in _manifest(source / "revisions").values()):
            raise OSError("Choose a profile folder containing save revisions.")
        target.parent.mkdir(parents=True, exist_ok=True)
        backups = target.parent / "fh2mp-save-backups"
        if backups.is_symlink() or (hasattr(backups, "is_junction") and backups.is_junction()):
            raise OSError("The save backup directory cannot be a link.")
        backups.mkdir(exist_ok=True)
        backup = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-"), dir=backups))
        _verified_copy(source, backup / "source")
        existed = target.exists()
        original = _manifest(target) if existed else None
        if existed:
            _verified_copy(target, backup / "destination")
        stage = Path(tempfile.mkdtemp(prefix=target.name + ".adopting-", dir=target.parent))
        _verified_copy(source, stage / "profile")
        if _manifest(backup / "source") != _manifest(stage / "profile"):
            raise OSError("The source changed after backup. Close Azimuth and try again.")
        if existed and original != _manifest(target):
            raise OSError("The destination changed during backup. Close Azimuth and try again.")
        # Keep the original in place until all copies have been verified.
        retained = None
        if existed:
            retained = target.with_name(target.name + ".fresh-" + backup.name)
            target.rename(retained)
        try:
            (stage / "profile").rename(target)
        except OSError:
            if retained is not None:
                retained.rename(target)
            raise
        return str(backup)
    except OSError as e:
        raise SaveCopyError(source, target, e) from e
    finally:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)


def elevated_copy(source, target):
    """Request elevation for this transfer only; never run the launcher/game as root."""
    return _elevate(["--save-copy", str(source), str(target)])


def retry_elevated(error):
    if isinstance(error, SaveScanError):
        return _elevate(["--save-adopt", error.folder, str(error.xuid), *error.others])
    return elevated_copy(error.source, error.target)


def _elevate(args):
    from .launch import host_env
    env = host_env()
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    command = ([sys.executable, *args] if getattr(sys, "frozen", False) else
               [sys.executable, str(Path(__file__).resolve()), *args])
    if sys.platform == "win32":
        import base64
        def quote(s):
            return "'" + s.replace("'", "''") + "'"
        script = ("$ErrorActionPreference='Stop'; try { $p=Start-Process -FilePath " + quote(command[0]) +
                  " -ArgumentList " + quote(subprocess.list2cmdline(command[1:])) +
                  " -Verb RunAs -Wait -PassThru; exit $p.ExitCode } catch { exit 1 }")
        command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand",
                   base64.b64encode(script.encode("utf-16-le")).decode("ascii")]
    else:
        if getattr(sys, "frozen", False) and os.environ.get("APPIMAGE"):
            # Root cannot access the player's FUSE mount. Start a separate,
            # extracted instance from the original AppImage instead.
            command = [os.path.abspath(os.environ["APPIMAGE"]), "--appimage-extract-and-run", *args]
        pkexec = shutil.which("pkexec")
        if not pkexec:
            raise OSError("Administrative retry needs pkexec (polkit). Correct the save folder permissions and retry.")
        command = [pkexec, *command]
    result = subprocess.run(command, check=False, env=env)
    if result.returncode:
        raise OSError("Administrative save copy was cancelled or failed. Play remains blocked; check folder permissions and retry.")


def helper_main(source, target):
    target = Path(target).absolute()
    ancestor = target.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    owner = ancestor.stat()
    backup = copy_profile(source, target)
    if sys.platform != "win32" and os.geteuid() == 0:
        import pwd
        uid = int(os.environ.get("PKEXEC_UID", owner.st_uid))
        gid = pwd.getpwuid(uid).pw_gid if "PKEXEC_UID" in os.environ else owner.st_gid
        # New files must remain writable by the player after pkexec exits.
        parent = target.parent
        while parent != ancestor:
            os.chown(parent, uid, gid)
            parent = parent.parent
        for root in (Path(target), Path(backup)):
            for directory, dirs, files in os.walk(root):
                os.chown(directory, uid, gid)
                for name in files:
                    os.chown(Path(directory) / name, uid, gid)
        os.chown(Path(backup).parent, uid, gid)
    return 0


def helper_dispatch(args):
    if args[:1] == ["--save-copy"] and len(args) == 3:
        return helper_main(args[1], args[2])
    if args[:1] == ["--save-adopt"] and len(args) >= 3:
        if __package__:
            from . import azimuth
        else:
            sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
            from fh2mp_launcher import azimuth
        azimuth.adopt_saves(args[1], int(args[2]), args[3:], copier=helper_main)
        return 0
    return 2


if __name__ == "__main__":
    try:
        sys.exit(helper_dispatch(sys.argv[1:]))
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
