"""Fixed values. Everything an operator or tester may want to change can also be set in the launcher's settings or by environment."""
import os

APP_NAME = "FH2MP"
GITHUB_REPO = "Charelene/FH2MP"
USER_AGENT = "FH2MP-Launcher"

# Where each channel's server lives. "stable" is the public server, "dev" the test stack (different database, different accounts).
API_BASES = {
    "stable": "https://api.forza.scuffed.af",
    "dev": "https://dev-api.forza.scuffed.af",
}
DEFAULT_WEBSITE = "https://forza.scuffed.af"
AZIMUTH_PAGE = "https://almost-there-games.itch.io/azimuth"

# Ed25519 public key (base64) whose signature every downloaded manifest must carry. The private half lives only with the maintainer
# (release/keygen.py); a compromised GitHub account alone therefore cannot push code to players.
RELEASE_PUBKEY_B64 = os.environ.get("FH2MP_RELEASE_PUBKEY") or "zo6hBWSZYaeL9YsZOC03kHJ8MR9f4awtgndgWR9h0Ic="

# Azimuth builds the shim was verified against. The server's launcher config can add newer ones without a launcher release.
BUILTIN_AZIMUTH_BUILDS = [
    {"version": "0.9.10", "sha256": "78a5f635b3241918707b5acddeaa96bd59ccb3819490e9218ee152497ca73639"},
]

SHIM_DLL = "winmm.dll"
SESSION_FILE = "fh2mp-session.txt"
LSP_FILE = "fh2mp-lsp.txt"
EXTRA_OPTIONS_DIR = "fh2extra"
# FH2 Extra Options: 0.2.98 is the only supported build, identified by its DLL (its ini still says v0.2.88). Keep identical to
# release/fetch_extra_options.py (DLL_SHA256).
EXTRA_OPTIONS_VERSION = "0.2.98"
EXTRA_OPTIONS_DLL_SHA256 = "d372b3a331c93cbbbf984d9a3b5df63836fdfb2aee26a10660c838c4806ff3d5"
