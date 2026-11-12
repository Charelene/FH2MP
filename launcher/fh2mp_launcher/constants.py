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
# signs the hotfix document (hotfix.py, release/hotfix.py); its own key, so a hotfix can be signed without the release key
HOTFIX_PUBKEY_B64 = os.environ.get("FH2MP_HOTFIX_PUBKEY") or "Lm0aUDJtUtC/upbOQOduZj5IGs3mWAyM0ix1BJpIZEs="

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
_EO_WC_LANGS = ('BR', 'CHT', 'CZ', 'DE', 'EN', 'ES', 'FR', 'GB', 'HU', 'IT', 'JP', 'KO', 'MX', 'NL', 'PL', 'RU')    # Weather Control's page languages
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
    # 0.6.0 (Forza XB1 Extra Options: one winmm.dll for FH2, FH3 and FM5) keeps its FH2 packages in mods/Forza Horizon 2; on its first start it
    # moves the player's older mods/<pack> folders and mods/loadorder.txt there itself (shim.py install_extra, eo_layout_repair). Only the FH2
    # files are taken: not the FH3/FM5 settings and mods, not the plugin's import library.
    "extraOptions": {
        'name': 'FH2 Extra Options', 'author': 'RokkuDayo', 'license': 'GPL-3.0', 'version': '0.6.0',
        'url': 'https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/Azimuth_v1.0.4/Forza_XB1_Extra_Options_0.6.0-Azimuth.zip',
        'mirror': MIRROR_BASE + 'Forza_XB1_Extra_Options_0.6.0-Azimuth.zip',
        'sha256': '3531871c747843586abce8832508f0e4e9a324226f235dc235b68ebd63d18671', 'size': 1299318,
        'dll': 'winmm.dll', 'dllSha256': '2b88d0e0ce6888e43c8f772b60e30968576f2304ee227b3378ebf26e535eb70c',
        'files': {name: name for name in (
            'winmm.dll',
            'FH2_ExtraOptions.ini',
            'mods/Forza Horizon 2/Extra Options - Garage Revving/mod.json',
            'mods/Forza Horizon 2/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll',
            'mods/Forza Horizon 2/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.ini',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/mod.json',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/media/cars/DOD_ViperGTSACR_99/DOD_ViperGTSACR_99.carbin.json',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/media/cars/DOD_ViperGTSACR_99/scene/Exterior/Doors/wingMirrorL_a.modelbin',
            'mods/Forza Horizon 2/Extra Options - Weather Control/mod.json',
            *(f'mods/Forza Horizon 2/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_{lang}.xaml'
              for lang in _EO_WC_LANGS),
            'mods/Forza Horizon 2/Extra Options - Weather Control/plugins/EO_WeatherControl.dll',
            'mods/Forza Horizon 2/Extra Options - Weather Control/plugins/EO_WeatherControl.ini',
        )},
        'fileSha256': {
            'mods/Forza Horizon 2/Extra Options - Garage Revving/mod.json': '4f70c5538490f61011f854c7afcdcea91658417aa54bafad98c5e61ea28af00f',
            'mods/Forza Horizon 2/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll': 'eb48f446f22eca24d6cc93f25d6a70bd60c387a8d3ef675921cc3ab1d1bc5e17',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/mod.json': '275486dd81edf2e82852cf9b21a9088e356862a08ad4ae46af71ec4b1fc90dfe',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/media/cars/DOD_ViperGTSACR_99/DOD_ViperGTSACR_99.carbin.json': '0d51924f4c48c73672d4891e4b4c1d863bfcd427cb1bbc65fccc469a0f7373be',
            'mods/Forza Horizon 2/Extra Options - Model Fixes/media/cars/DOD_ViperGTSACR_99/scene/Exterior/Doors/wingMirrorL_a.modelbin': '8e1b7c6e125e348975841990af0d2436c3c957cd1ca56661f071041d3e82e668',
            'mods/Forza Horizon 2/Extra Options - Weather Control/mod.json': 'c9db7aba66c70b42d80d7199f83e989ea8c4a3141ebb7a8a30f0e48d311a1851',
            **{f'mods/Forza Horizon 2/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_{lang}.xaml': digest
               for lang, digest in zip(_EO_WC_LANGS, (
                   'b123f1706f8838c2d49b27bb8c995c2ec18c2cb1d04a44bff1f44e6a3d8cc4bc', 'b4b9d1dbe708c01f408aac94ee3ae74145a6cef219e76900a05b552e4a4c5c0b',
                   'f360b02218c19748b18e7e06c232d6cd026d3213a099f5cd6d0dec8cce0b249f', '512198de83cd0440d5b68a8fc8dc3b74ed3674f5c949d73a55a170e9e3b5923a',
                   '314aa157f18f42f9ec36ae4c38cd2a78da627608770363db275afd86a7a36102', '000a5e4ff23899eee5e2d77e8d378ddbfad4f0e6349ed8c550c8369f3fc18115',
                   '4b182ff6f60cfc672bb406c48813bd37978c3b56cd20c87816b81f12c386d5e1', '314aa157f18f42f9ec36ae4c38cd2a78da627608770363db275afd86a7a36102',
                   '608c91827e84445ba0ae1f56884a9abfd302a77debe87821b1cf3605c743665b', 'b881efda0d35b8eb49dafcf6fdb25e1f75a40e36d8e0163b6e74f2ca77398abe',
                   '596cc2b192369c076bea72eccf9ab318fbffc3fd71b679d6eff643804ea44bf1', '59fef45cea1fc0276c05bbbfc2e03478cce51f73c2e92b65c17746b713223bf0',
                   '000a5e4ff23899eee5e2d77e8d378ddbfad4f0e6349ed8c550c8369f3fc18115', '6acbb97480ff59800f9017d83ac72ef96034376880faf63ab5e5d7efde233499',
                   '5f9b9b0f76ba658547c55c15f7a320ce325655d8f3426bd51ec019b416138557', '7481aca0221de0f62b15030ebc0073d7cb0d6d36f673883b59203b76bf182923'))},
            'mods/Forza Horizon 2/Extra Options - Weather Control/plugins/EO_WeatherControl.dll': '8f8628c7c87c2e7f6d287fed24c35919d8babab18277b0a5e33b4e16ac7cbe67',
        },
    },
    "honkHonk": {
        "name": "HonkHonk", "author": "suderek22", "license": "GPL-3.0", "version": "0.4.0",
        "url": "https://github.com/suderek22/FH2_HonkHonk/releases/download/v0.4.0-online/HonkHonk_v0.4.0.zip",
        "mirror": MIRROR_BASE + "HonkHonk_v0.4.0.zip",
        "sha256": "bb2897d600b1e9f1c27a0d5e6ea51cbd39cf5cefb18bd55aa5f72b368e535338", "size": 5716949,
        "dll": "HonkHonk_v0.4.0/fh2-mods/HonkHonk.dll", "dllSha256": "b42484b7f70f46d5e92b3c53194c856d1d09aefb81223d55eee1701b0f182bf4",
        "files": {
            "HonkHonk_v0.4.0/fh2-mods/HonkHonk.dll": "fh2-mods/HonkHonk.dll",
            "HonkHonk_v0.4.0/fh2-mods/HonkHonk.ini": "fh2-mods/HonkHonk.ini",
            "HonkHonk_v0.4.0/fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank": "fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank",
        },
        "fileSha256": {
            "fh2-mods/HonkHonk.dll": "b42484b7f70f46d5e92b3c53194c856d1d09aefb81223d55eee1701b0f182bf4",
            "fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank": "6f657bf3258670838bdaef1d1b18dfa198d11e39efa961d1fb2f036fce501063",
        },
    },
    # Only the mod's DLL is taken: the zip also carries a build of the FH2 Mod Platform (DWrite.dll), which stays the pinned one above.
    "anna": {
        "name": "FH2 ANNA", "author": "qiangqiang101", "license": "MIT", "version": "1.1.1",
        "url": "https://github.com/qiangqiang101/FH2_ANNA/releases/download/1.1.1/FH2ANNA-1.1.1.zip",
        "mirror": MIRROR_BASE + "FH2ANNA-1.1.1.zip",
        "sha256": "423faa3c505d4271a68d4dcc0e91a914226c744d2edf0aa793bee636e88820fc", "size": 1333643,
        "dll": "fh2-mods/FH2ANNA.dll", "dllSha256": "6955cbd35d35408a8e9ed8de132e75b877d52c681c645dcec6d24c50d8d974b6",
        "files": {
            "fh2-mods/FH2ANNA.dll": "fh2-mods/FH2ANNA.dll",
        },
        "fileSha256": {
            "fh2-mods/FH2ANNA.dll": "6955cbd35d35408a8e9ed8de132e75b877d52c681c645dcec6d24c50d8d974b6",
        },
    },
    # Not required to play: the game's 88 achievements, which the Guide lists and the server shows on gamercards. Only the mod's DLL is taken:
    # the zip also carries a build of the FH2 Mod Platform (d3dcompiler_47.dll / DWrite.dll), which stays the pinned one above.
    "achievements": {
        "name": "Achievements", "author": "Trinculo54", "license": "GPL-3.0", "version": "1.0.2",
        "url": "https://github.com/Trinculo54/fh2-achievements/releases/download/v1.0.2/Achievements-v1.0.2.zip",
        "mirror": MIRROR_BASE + "Achievements-v1.0.2.zip",
        "sha256": "7655cdb7a4ceb65d4703f83fbf2afaa0e199545d089e8f1a56abd2ed0a1bc188", "size": 2780646,
        "dll": "fh2-mods/Achievements.dll", "dllSha256": "4b487ee0fc825bfd22ad13e638b051a1b16fa822815f92554dd8274e6bedb316",
        "files": {
            "fh2-mods/Achievements.dll": "fh2-mods/Achievements.dll",
        },
        "fileSha256": {
            "fh2-mods/Achievements.dll": "4b487ee0fc825bfd22ad13e638b051a1b16fa822815f92554dd8274e6bedb316",
        },
    },
}
# The Extra Options build launchers up to 1.3.2 installed (0.4.4, zip FH2_Extra_Options_0.4.4-azimuth.zip): only to recognise FH2MP's own earlier
# install when 0.6.0 replaces it. Its bundled packages were at the mods root; 0.6.0 brings its own copies in mods/Forza Horizon 2, so the old
# ones are moved aside when they are exactly these files (shim.retire_old_extra_packages).
RETIRED_EXTRA_OPTIONS = {
    "version": "0.4.4",
    "dllSha256": "c69697d015416973fdd763005bbe23d5b68f9c8acfef1165db0bbf02b8493213",
    "packages": {
        "Extra Options - Garage Revving": {
            "mod.json": "4f70c5538490f61011f854c7afcdcea91658417aa54bafad98c5e61ea28af00f",
            "plugins/EO_GarageEngineRev.dll": "eb48f446f22eca24d6cc93f25d6a70bd60c387a8d3ef675921cc3ab1d1bc5e17",
        },
        "Extra Options - Weather Control": {
            "mod.json": "d5e10b1ae3c33e414d2a21a41887bf3c6bd5436192afa726d118297ff3bce547",
            "plugins/EO_WeatherControl.dll": "4df33e3bc229e43c00bb631f4c8543e6803a9cc677721df2883342831d67dc24",
        },
    },
}
# Extra Options 0.6.0's packages folder, and the names it keeps for itself at the mods root (never a package).
EXTRA_MODS_DIR = "mods/Forza Horizon 2"
EXTRA_MODS_RESERVED = ("Forza Horizon 2", "Forza Motorsport 5", "Forza Horizon 3", "Cache")
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
