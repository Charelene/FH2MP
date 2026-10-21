"""Updates from GitHub releases.

Trust does not come from GitHub or TLS alone: every release carries `manifest.json` and `manifest.json.sig`, an Ed25519 signature made with
a key that never touches GitHub (release/sign_manifest.py). The manifest lists each file with its SHA-256 and size; nothing is installed
that the signed manifest does not name, and the download location is built from the signed tag and file names, never taken from the network.
"""
import base64
import hashlib
import json
import os
import re
import ssl
import sys
import urllib.error
import urllib.request

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from . import buildcfg, paths
from .constants import GITHUB_REPO, RELEASE_PUBKEY_B64, USER_AGENT
from .version import __version__

_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$")
_TAG_RE = re.compile(r"^v?[0-9][0-9A-Za-z.+-]{0,30}$")
MAX_MANIFEST = 256 * 1024
MAX_ASSET = 400 * 1024 * 1024


class UpdateError(Exception):
    pass


def vkey(v):
    """Sort key for '1.2.3' and '1.2.3-rc1' (a pre-release sorts before the release, rc10 after rc9)."""
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)(?:[-+](.*))?$", v or "")
    if not m:
        return (0, 0, 0, 0, 0, "")
    rc = re.match(r"rc(\d+)", m[4] or "")
    return (int(m[1]), int(m[2]), int(m[3]), 0 if m[4] and "+" not in v else 1, int(rc[1]) if rc else 0, m[4] or "")


def newer(a, b):
    """a is a later version than b"""
    return vkey(a) > vkey(b)


def at_least(have, need):
    """`have` is the same as or later than `need`; nothing is required of an empty or 0.0.0 `need`. Our own debug builds always pass."""
    if not need or vkey(need)[:3] == (0, 0, 0):
        return True
    return (have or "").endswith("-debug") or vkey(have) >= vkey(need)


def verify_manifest(raw, sig_b64, pubkey_b64=None):
    pub = pubkey_b64 if pubkey_b64 is not None else RELEASE_PUBKEY_B64
    if not pub:
        raise UpdateError("this build has no release key: updates are disabled")
    try:
        Ed25519PublicKey.from_public_bytes(base64.b64decode(pub)).verify(base64.b64decode(sig_b64.strip()), raw)
    except (InvalidSignature, ValueError):
        raise UpdateError("the release manifest signature is not valid") from None
    try:
        m = json.loads(raw)
    except ValueError:
        raise UpdateError("the release manifest is not readable") from None
    if not isinstance(m, dict) or m.get("schema") != 1 or not _TAG_RE.match(str(m.get("tag", ""))):
        raise UpdateError("the release manifest is not in a format this launcher understands")
    return m


def _bundled_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except Exception:  # noqa: BLE001
        return None


_USE_BUNDLED = None       # the certifi context, once the system store failed and it worked: every later Updater (the shim download makes a new one) starts with it


def _is_cert_error(e):
    return isinstance(getattr(e, "reason", e), ssl.SSLCertVerificationError) or isinstance(e, ssl.SSLCertVerificationError)


class Updater:
    def __init__(self, repo=GITHUB_REPO, channel="stable", pubkey_b64=None, base=None, api="https://api.github.com"):
        base = base or buildcfg.update_base() or "https://github.com"        # a private build may fetch from its bundled signed mirror
        self.repo, self.channel, self.pub, self.base, self.api = repo, channel, pubkey_b64, base, api
        self._ctx = _USE_BUNDLED or ssl.create_default_context()

    def _open(self, req, timeout):
        """urlopen with the system certificate store; when that does not trust the server (the AppImage's OpenSSL cannot find the distro's store),
        once more with the bundled roots, which are then used for everything that follows."""
        global _USE_BUNDLED
        try:
            return urllib.request.urlopen(req, timeout=timeout, context=self._ctx)
        except (urllib.error.URLError, OSError, ssl.SSLError) as e:
            bundled = _bundled_context() if _is_cert_error(e) and self._ctx is not _USE_BUNDLED else None
            if bundled is None:       # the system store was not the problem (or there is nothing else to try)
                raise
            self._ctx = _USE_BUNDLED = bundled
            return urllib.request.urlopen(req, timeout=timeout, context=self._ctx)

    def _get(self, url, limit, timeout=20, accept=None):
        req = urllib.request.Request(url, headers={"User-Agent": f"{USER_AGENT}/{__version__}", **({"Accept": accept} if accept else {})})
        try:
            with self._open(req, timeout) as r:
                data = r.read(limit + 1)
        except urllib.error.HTTPError as e:
            raise UpdateError(f"GitHub answered {e.code} for {url.rsplit('/', 1)[-1]}") from None
        except (urllib.error.URLError, OSError, ssl.SSLError) as e:
            raise UpdateError(f"cannot reach GitHub ({getattr(e, 'reason', e)})") from None
        if len(data) > limit:
            raise UpdateError("the download is larger than expected")
        return data

    def _release_tag(self):
        """stable: the latest non-prerelease. dev, and any pre-release launcher (GitHub's "latest" skips pre-releases, so it could never find its
        own shim): the newest release, pre-releases included."""
        if self.channel != "dev" and "-" not in __version__:
            return None
        rels = json.loads(self._get(f"{self.api}/repos/{self.repo}/releases?per_page=15", 1_000_000, accept="application/vnd.github+json"))
        for r in rels:
            if not r.get("draft") and any(a.get("name") == "manifest.json" for a in r.get("assets", [])):
                return r["tag_name"]
        raise UpdateError("no release with a manifest was found")

    def asset_url(self, tag, name):
        if not _TAG_RE.match(tag) or not _NAME_RE.match(name):
            raise UpdateError("refusing a suspicious file name")
        return f"{self.base}/{self.repo}/releases/download/{tag}/{name}"

    def fetch_manifest(self):
        tag = self._release_tag()
        prefix = f"{self.base}/{self.repo}/releases/" + (f"download/{tag}/" if tag else "latest/download/")
        raw = self._get(prefix + "manifest.json", MAX_MANIFEST)
        sig = self._get(prefix + "manifest.json.sig", 4096).decode("ascii", "replace")
        m = verify_manifest(raw, sig, self.pub)
        if tag and m["tag"] != tag:
            raise UpdateError("the manifest belongs to a different release")
        return m

    def download(self, tag, entry, progress=None, cancelled=lambda: False, dest_dir=None):
        """Downloads one manifest entry ({name, sha256, size}) to the cache, checks size and SHA-256, returns the path."""
        name, want = entry["name"], entry["sha256"].lower()
        size = int(entry["size"])
        if size > MAX_ASSET or not re.fullmatch(r"[0-9a-f]{64}", want):
            raise UpdateError("the manifest entry is not acceptable")
        dest = os.path.join(dest_dir or paths.cache_dir(), name)
        if os.path.isfile(dest) and sha256_file(dest) == want:
            return dest
        req = urllib.request.Request(self.asset_url(tag, name), headers={"User-Agent": f"{USER_AGENT}/{__version__}"})
        h, got = hashlib.sha256(), 0
        tmp = dest + ".part"
        try:
            with self._open(req, 30) as r, open(tmp, "wb") as f:
                while True:
                    chunk = r.read(1 << 16)
                    if not chunk:
                        break
                    got += len(chunk)
                    if got > size:
                        raise UpdateError("the download is larger than the manifest says")
                    h.update(chunk)
                    f.write(chunk)
                    if progress:
                        progress(got, size)
                    if cancelled():
                        raise UpdateError("cancelled")
        except (urllib.error.URLError, OSError, ssl.SSLError) as e:
            _rm(tmp)
            raise UpdateError(f"download failed ({getattr(e, 'reason', e)})") from None
        except BaseException:
            _rm(tmp)
            raise
        if got != size or h.hexdigest() != want:
            _rm(tmp)
            raise UpdateError("the download does not match the signed checksum")
        os.replace(tmp, dest)
        return dest


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _rm(p):
    try:
        os.unlink(p)
    except OSError:
        pass


# ---- what this launcher installation is, to pick the right file when it needs replacing

def launcher_flavour():
    """windows-x64-installer | windows-x64-portable | linux-appimage | linux-deb | None (running from source: no self-update)."""
    if not getattr(sys, "frozen", False):
        return None
    if sys.platform == "win32":
        here = os.path.dirname(os.path.abspath(sys.executable))
        installed = any(n.lower().startswith("unins") and n.lower().endswith(".exe") for n in os.listdir(here))     # Inno Setup leaves its uninstaller next to the program
        return "windows-x64-installer" if installed else "windows-x64-portable"
    if os.environ.get("APPIMAGE"):
        return "linux-appimage"
    if sys.platform.startswith("linux") and os.path.realpath(sys.executable).startswith(("/usr/", "/opt/")):
        return "linux-deb"
    return None


def shim_update_needed(installed_kind, installed_version, manifest):
    """None when the installed shim is current, else a short reason."""
    want = manifest.get("shim", {}).get("version", "")
    if installed_kind != "ours":
        return "not installed"
    if newer(want, installed_version):
        return f"{installed_version} -> {want}"
    return None


def _apply_portable_zip(zip_path):
    """Unpacks the portable zip next to the running copy, then lets a small script copy it over the folder once this process has exited (a running
    exe cannot be overwritten on Windows) and start the launcher again. Returns "exit"."""
    import subprocess
    import tempfile
    import zipfile
    here = os.path.dirname(os.path.abspath(sys.executable))
    exe = os.path.abspath(sys.executable)
    if any(c in here + exe for c in '%"^&|<>!'):
        raise UpdateError("this folder name has characters the updater cannot handle: download the new portable zip from the releases page")
    stage = tempfile.mkdtemp(prefix="fh2mp-update-", dir=os.path.dirname(here) or here)
    root = os.path.realpath(stage)
    try:
        z = zipfile.ZipFile(zip_path)
    except zipfile.BadZipFile:
        raise UpdateError("the downloaded portable package is not a valid zip") from None
    with z:
        for info in z.infolist():
            dest = os.path.realpath(os.path.join(stage, info.filename))
            if dest != root and not dest.startswith(root + os.sep):
                raise UpdateError("the portable package contains an unsafe path")
        if not any(os.path.basename(n.filename).lower() == os.path.basename(exe).lower() for n in z.infolist()):
            raise UpdateError("the portable package does not contain the launcher")
        z.extractall(stage)
    script = os.path.join(stage, "apply-update.cmd")
    with open(script, "w", encoding="ascii", newline="\r\n") as f:
        f.write("@echo off\r\n"
                f":wait\r\ntasklist /FI \"PID eq {os.getpid()}\" 2>nul | find \"{os.getpid()}\" >nul\r\n"
                "if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait)\r\n"
                f"robocopy \"{stage}\" \"{here}\" /E /XF apply-update.cmd /R:5 /W:1 /NFL /NDL /NJH /NJS >nul\r\n"
                f"start \"\" \"{exe}\"\r\n"
                f"cd /d \"{os.path.dirname(here) or here}\"\r\n"
                f"rmdir /S /Q \"{stage}\"\r\n")
    subprocess.Popen(["cmd", "/c", script], creationflags=0x00000008 | 0x00000200 | 0x08000000, close_fds=True)
    return "exit"


def apply_launcher_update(flavour, path):
    """Installs a downloaded (and already verified) launcher package. Returns "exit" when the launcher must quit now so the installer can
    replace it, "restart" when it was replaced in place, or raises UpdateError."""
    import shutil
    import subprocess
    try:
        if flavour == "windows-x64-installer":
            subprocess.Popen([path, "/SILENT", "/SUPPRESSMSGBOXES", "/CLOSEAPPLICATIONS", "/RESTARTAPPLICATIONS"],
                             creationflags=0x00000008 | 0x00000200)
            return "exit"
        if flavour == "windows-x64-portable":
            return _apply_portable_zip(path)
        if flavour == "linux-appimage":
            target = os.environ["APPIMAGE"]
            staged = target + ".new"
            shutil.copyfile(path, staged)
            os.chmod(staged, 0o755)
            os.replace(staged, target)
            return "restart"
        if flavour == "linux-deb":
            r = subprocess.run(["pkexec", "apt-get", "install", "-y", path], capture_output=True, timeout=600)
            if r.returncode != 0:
                raise UpdateError("the package could not be installed (" + (r.stderr or b"").decode("utf-8", "replace")[-200:].strip() + ")")
            return "restart"
    except OSError as e:
        raise UpdateError(f"could not install the update: {e.strerror or e}") from None
    raise UpdateError("this installation cannot update itself: download the new version from the releases page")
