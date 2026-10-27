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

# The shim is a mod for the FH2 Mod Platform (Trinculo54, MIT): fh2-mods\FH2MP.dll, loaded by the platform's d3dcompiler_47.dll next to
# azimuth.exe, side by side with the player's other FH2 mods. Up to 1.0.0-rc5 it was a winmm.dll proxy itself (LEGACY_SHIM_DLL) that
# chain-loaded FH2 Extra Options from fh2extra\; installing a new shim moves that layout over.
MODS_DIR = "fh2-mods"
SHIM_DLL = "FH2MP.dll"
LEGACY_SHIM_DLL = "winmm.dll"
DATA_DIR = "fh2mp"                     # FH2MP's own files next to azimuth.exe (the car database copy)
PLATFORM_DLL = "d3dcompiler_47.dll"
PLATFORM_FILES = ("d3dcompiler_47.dll", "LICENSE-FH2ModPlatform.txt", "LICENSE-MinHook.txt")

# Third-party mods the launcher downloads from their authors' own GitHub releases (FH2MP does not redistribute them), pinned by the
# release zip's SHA-256 and the DLL's inside it. `files` maps a file in the zip to where it goes in the Azimuth folder.
UPSTREAM = {
    "platform": {
        "name": "FH2 Mod Platform", "author": "Trinculo54", "license": "MIT", "version": "2.7.3",
        "url": "https://github.com/Trinculo54/fh2-mod-platform/releases/download/v2.7.3/FH2-Mod-Platform-v2.7.3.zip",
        "sha256": "e69b963a54e9b1945bcfd73c5523c53f0fc95a301b0f2c1361592d300eff04c9", "size": 959320,
        "dll": "d3dcompiler_47.dll", "dllSha256": "9e1d095ff416242d03265088e732c40d3e0b90f0e38a7c74adc64c96af25bd30",
        "files": {"d3dcompiler_47.dll": "d3dcompiler_47.dll", "LICENSE.txt": "LICENSE-FH2ModPlatform.txt",
                  "LICENSE-MinHook.txt": "LICENSE-MinHook.txt"},
    },
    "extraOptions": {
        "name": "FH2 Extra Options", "author": "RokkuDayo", "license": "GPL-3.0", "version": "0.2.98",      # its ini still says v0.2.88
        "url": "https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/Azimuth_v0.9.10/FH2_Extra_Options_0.2.98-release.zip",
        "sha256": "d3b9db715416abd8efef4048445a9edbe22d333ba0267fb48fb255dfab000c38", "size": 731258,
        "dll": "Azimuth/winmm.dll", "dllSha256": "d372b3a331c93cbbbf984d9a3b5df63836fdfb2aee26a10660c838c4806ff3d5",
        "files": {"Azimuth/winmm.dll": "winmm.dll", "Azimuth/FH2_ExtraOptions.ini": "FH2_ExtraOptions.ini"},
    },
}
# The platform build the shim is tested against; a newer one (another mod's) is kept.
PLATFORM_VERSION = UPSTREAM["platform"]["version"]
PLATFORM_DLL_SHA256 = UPSTREAM["platform"]["dllSha256"]
# Wine and Proton use their own winmm.dll (Extra Options) and possibly d3dcompiler_47.dll (the platform) unless told otherwise
# (the platform's docs/linux.md).
WINE_DLL_OVERRIDES = "winmm,d3dcompiler_47=n,b"
SESSION_FILE = "fh2mp-session.txt"
LSP_FILE = "fh2mp-lsp.txt"
EXTRA_OPTIONS_DIR = "fh2extra"         # where rc5 and earlier kept FH2 Extra Options (it is winmm.dll next to azimuth.exe now)
EXTRA_OPTIONS_DLL = "winmm.dll"
# FH2 Extra Options: 0.2.98 is the only supported build, identified by its DLL.
EXTRA_OPTIONS_VERSION = UPSTREAM["extraOptions"]["version"]
EXTRA_OPTIONS_DLL_SHA256 = UPSTREAM["extraOptions"]["dllSha256"]
