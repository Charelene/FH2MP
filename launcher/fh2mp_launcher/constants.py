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

# One supplied compatibility set per launcher release. Server metadata cannot broaden it.
AZIMUTH_VERSION = "1.0.4"
BUILTIN_AZIMUTH_BUILDS = [
    {"version": AZIMUTH_VERSION, "sha256": "56d2c9cb6ccf3ae1836dee9676b64285f3a247efa2d12cf02c6c742696b3b092"},
]
# Per-title helpers from the same supplied Azimuth 1.0.4 installation.
AZIMUTH_TLS_HELPERS = {
    "azimuth-tls-1.dll": "c672413a9b068bbe789ecd6a5bd86c0548719e7e4bec72948fad9021f205107c",
    "azimuth-tls-2.dll": "dac7202cdf9643172b5100ab7ee83b126b24ceab58c7208501b22dd86732c547",
}

# The shim is a mod for the FH2 Mod Platform (Trinculo54, MIT): fh2-mods\FH2MP.dll, loaded by the platform's DWrite.dll next to
# azimuth.exe, side by side with the player's other FH2 mods. Up to 1.0.0-rc5 it was a winmm.dll proxy itself (LEGACY_SHIM_DLL) that
# chain-loaded FH2 Extra Options from fh2extra\; installing a new shim moves that layout over.
MODS_DIR = "fh2-mods"
SHIM_DLL = "FH2MP.dll"
LEGACY_SHIM_DLL = "winmm.dll"
DATA_DIR = "fh2mp"                     # FH2MP's own files next to azimuth.exe (the car database copy)
PLATFORM_DLL = "d3dcompiler_47.dll"
PLATFORM_LOADERS = (PLATFORM_DLL, "DWrite.dll")
PLATFORM_FILES = (*PLATFORM_LOADERS, "LICENSE-FH2ModPlatform.txt", "LICENSE-MinHook.txt")

# Where FH2MP keeps copies of the exact pinned third-party files. The authors replace release assets in place and move tags, which breaks a
# pin by hash; the copies here never change. Every download is still checked against the pinned SHA-256 (and size), wherever it came from.
MIRROR_BASE = "https://forza.scuffed.af/media/forza-mirror/"
AZIMUTH_ARCHIVE = {"name": "azimuth-windows-1.0.4.zip", "url": MIRROR_BASE + "azimuth-windows-1.0.4.zip", "size": 6835590,
                   "sha256": "f0e603a4548ae7c40c1cef584ce691dbeae0bc178666bed896ab87ab6cd52994"}

# Third-party mods the launcher downloads from the FH2MP mirror, falling back to their authors' own GitHub releases, pinned by the
# release zip's SHA-256 and the DLL's inside it. `files` maps a file in the zip to where it goes in the Azimuth folder.
UPSTREAM = {
    "platform": {
        "name": "FH2 Mod Platform", "author": "Trinculo54", "license": "MIT", "version": "2.8.0",
        "url": "https://github.com/Trinculo54/fh2-mod-platform/releases/download/v2.8.0/FH2-Mod-Platform-v2.8.0.zip",
        "mirror": MIRROR_BASE + "FH2-Mod-Platform-v2.8.0.zip",
        "sha256": "24606e6578eebcbc2d831ad48041ff74cf615c2f36160f0950e1045fd7caddd7", "size": 2339464,
        "dll": "d3dcompiler_47.dll", "dllSha256": "a7db4a14de47cd7af98fce036877a72a88ea65498818bb2ecb755628fc99d357",
        "files": {"d3dcompiler_47.dll": "d3dcompiler_47.dll", "DWrite.dll": "DWrite.dll", "LICENSE.txt": "LICENSE-FH2ModPlatform.txt",
                  "LICENSE-MinHook.txt": "LICENSE-MinHook.txt"},
    },
    "extraOptions": {
        'name': 'FH2 Extra Options', 'author': 'RokkuDayo', 'license': 'GPL-3.0', 'version': '0.4.4',
        'url': 'https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/Azimuth_v1.0.0/FH2_Extra_Options_0.4.4-azimuth.zip',
        'mirror': MIRROR_BASE + 'FH2_Extra_Options_0.4.4-azimuth.zip',
        'sha256': '42502c2befa06b17d1ba45b1cde5e6311b786ce8f6a15b179023a1be79f0147d', 'size': 309144,
        'dll': 'Azimuth/winmm.dll', 'dllSha256': 'c69697d015416973fdd763005bbe23d5b68f9c8acfef1165db0bbf02b8493213',
        'files': {
            'Azimuth/FH2_ExtraOptions.ini': 'FH2_ExtraOptions.ini',
            'Azimuth/mods/Extra Options - Garage Revving/mod.json': 'mods/Extra Options - Garage Revving/mod.json',
            'Azimuth/mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll': 'mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll',
            'Azimuth/mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.ini': 'mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.ini',
            'Azimuth/mods/Extra Options - Weather Control/mod.json': 'mods/Extra Options - Weather Control/mod.json',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl.dll': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl.dll',
            'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl.ini': 'mods/Extra Options - Weather Control/plugins/EO_WeatherControl.ini',
            'Azimuth/winmm.dll': 'winmm.dll',
        },
        'fileSha256': {
            'mods/Extra Options - Garage Revving/mod.json': '4f70c5538490f61011f854c7afcdcea91658417aa54bafad98c5e61ea28af00f',
            'mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll': 'eb48f446f22eca24d6cc93f25d6a70bd60c387a8d3ef675921cc3ab1d1bc5e17',
            'mods/Extra Options - Weather Control/mod.json': 'd5e10b1ae3c33e414d2a21a41887bf3c6bd5436192afa726d118297ff3bce547',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml': 'b123f1706f8838c2d49b27bb8c995c2ec18c2cb1d04a44bff1f44e6a3d8cc4bc',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml': 'b4b9d1dbe708c01f408aac94ee3ae74145a6cef219e76900a05b552e4a4c5c0b',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml': 'f360b02218c19748b18e7e06c232d6cd026d3213a099f5cd6d0dec8cce0b249f',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml': '512198de83cd0440d5b68a8fc8dc3b74ed3674f5c949d73a55a170e9e3b5923a',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml': '314aa157f18f42f9ec36ae4c38cd2a78da627608770363db275afd86a7a36102',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml': '000a5e4ff23899eee5e2d77e8d378ddbfad4f0e6349ed8c550c8369f3fc18115',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml': '4b182ff6f60cfc672bb406c48813bd37978c3b56cd20c87816b81f12c386d5e1',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml': '314aa157f18f42f9ec36ae4c38cd2a78da627608770363db275afd86a7a36102',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml': '608c91827e84445ba0ae1f56884a9abfd302a77debe87821b1cf3605c743665b',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml': 'b881efda0d35b8eb49dafcf6fdb25e1f75a40e36d8e0163b6e74f2ca77398abe',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml': '596cc2b192369c076bea72eccf9ab318fbffc3fd71b679d6eff643804ea44bf1',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml': '59fef45cea1fc0276c05bbbfc2e03478cce51f73c2e92b65c17746b713223bf0',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml': '000a5e4ff23899eee5e2d77e8d378ddbfad4f0e6349ed8c550c8369f3fc18115',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml': '6acbb97480ff59800f9017d83ac72ef96034376880faf63ab5e5d7efde233499',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml': '5f9b9b0f76ba658547c55c15f7a320ce325655d8f3426bd51ec019b416138557',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml': '7481aca0221de0f62b15030ebc0073d7cb0d6d36f673883b59203b76bf182923',
            'mods/Extra Options - Weather Control/plugins/EO_WeatherControl.dll': '4df33e3bc229e43c00bb631f4c8543e6803a9cc677721df2883342831d67dc24',
        },
    },
}
# The exact platform build in this release's supported combination.
PLATFORM_VERSION = UPSTREAM["platform"]["version"]
PLATFORM_DLL_SHA256 = UPSTREAM["platform"]["dllSha256"]
# Wine and Proton use their own winmm.dll (Extra Options) and possibly d3dcompiler_47.dll (the platform) unless told otherwise
# (the platform's docs/linux.md).
WINE_DLL_OVERRIDES = "winmm,dwrite,d3dcompiler_47=n,b"
SESSION_FILE = "fh2mp-session.txt"
LSP_FILE = "fh2mp-lsp.txt"
EXTRA_OPTIONS_DIR = "fh2extra"         # where rc5 and earlier kept FH2 Extra Options (it is winmm.dll next to azimuth.exe now)
EXTRA_OPTIONS_DLL = "winmm.dll"
# The only supported Extra Options build for this release.
EXTRA_OPTIONS_VERSION = UPSTREAM["extraOptions"]["version"]
EXTRA_OPTIONS_DLL_SHA256 = UPSTREAM["extraOptions"]["dllSha256"]
