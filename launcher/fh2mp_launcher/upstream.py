"""Third-party mods FH2MP needs (constants.UPSTREAM): the FH2 Mod Platform (Trinculo54, MIT) that loads the shim, and FH2 Extra Options
(RokkuDayo, GPL-3.0). Fetched from FH2MP's mirror of the exact pinned release (constants.MIRROR_BASE), or from the authors' own GitHub release. Each release is pinned by the SHA-256 of
its zip and of the DLL inside it, so nothing else is installed; the launcher only fetches one when the copy in the Azimuth folder is
missing or not the pinned build (shim.py decides)."""
import hashlib
import io
import os
import zipfile

from .constants import MIRROR_BASE, UPSTREAM
from .updater import UpdateError


def extra_options_kind(folder):
    """There is one supplied dependency set, independent of installed versions."""
    return "extraOptions"


class UpstreamError(Exception):
    pass


def extract(kind, data, pin=None):
    """The pinned release zip `data` -> {Azimuth-folder file name: bytes}. Raises UpstreamError when it is not exactly the pinned build."""
    pin = pin or UPSTREAM[kind]
    if hashlib.sha256(data).hexdigest() != pin["sha256"]:
        raise UpstreamError(f"the {pin['name']} download is not the expected release")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            out = {dest: z.read(src) for src, dest in pin["files"].items()}
            dll = z.read(pin["dll"])
    except (zipfile.BadZipFile, KeyError):
        raise UpstreamError(f"the {pin['name']} release is not laid out as expected") from None
    if hashlib.sha256(dll).hexdigest() != pin["dllSha256"]:
        raise UpstreamError(f"the {pin['name']} DLL is not the expected build")
    return out


def fetch(kind, updater, progress=None, cancelled=lambda: False):
    """Downloads the pinned release of `kind` ("platform" | "extraOptions") with `updater` (updater.Updater: certificates, size and hash
    checks, cache) and returns extract()'s files. The FH2MP mirror is tried first, the author's GitHub release second; both must be exactly the
    pinned bytes."""
    pin = UPSTREAM[kind]
    sources = [u for u in (pin.get("mirror"), pin["url"]) if u]
    for url in sources:
        if not (url.startswith("https://github.com/") or url.startswith(MIRROR_BASE)):
            raise UpstreamError(f"refusing to download {pin['name']} from anywhere but GitHub or the FH2MP mirror")
    last = None
    for url in sources:
        try:
            path = updater.download_url(url, {"name": pin["url"].rsplit("/", 1)[1], "sha256": pin["sha256"], "size": pin["size"]}, progress, cancelled)
        except UpdateError as e:                      # unreachable, moved, or not the pinned bytes: the next source
            if str(e) == "cancelled":
                raise
            last = e
            continue
        with open(path, "rb") as f:
            return extract(kind, f.read(), pin)
    raise last or UpstreamError(f"no source for {pin['name']}")


def read_zip(kind, path):
    """A local copy of the pinned release (tests, tools/rig.py --platform-zip) -> extract()'s files."""
    with open(path, "rb") as f:
        return extract(kind, f.read())
