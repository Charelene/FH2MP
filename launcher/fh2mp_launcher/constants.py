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
AZIMUTH_VERSION = "1.0.3"
BUILTIN_AZIMUTH_BUILDS = [
    {"version": AZIMUTH_VERSION, "sha256": "ebfd5d43a165dcec5d4007b9d41ed5b2ed122fcd02ecfef7e4ed35b40826821f"},
]
# Per-title helpers from the same supplied Azimuth 1.0.3 installation.
AZIMUTH_TLS_HELPERS = {
    "azimuth-tls-1.dll": "b1f3138966eeb19de38692f2e974b7465828023ab61bf9c5fed2699f92a715f8",
    "azimuth-tls-2.dll": "22da610dcd0191049c102d3c58d6d56f296af5643ef42f1dfd92fea62d9b4f31",
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

# Third-party mods the launcher downloads from their authors' own GitHub releases (FH2MP does not redistribute them), pinned by the
# release zip's SHA-256 and the DLL's inside it. `files` maps a file in the zip to where it goes in the Azimuth folder.
UPSTREAM = {
    "platform": {
        "name": "FH2 Mod Platform", "author": "Trinculo54", "license": "MIT", "version": "2.7.4",
        "url": "https://github.com/Trinculo54/fh2-mod-platform/releases/download/v2.7.4/FH2-Mod-Platform-v2.7.4.zip",
        "sha256": "f951c129839a99150f816193282184b0a4b1a4458d271d4d5e367d46838b080f", "size": 1903212,
        "dll": "d3dcompiler_47.dll", "dllSha256": "6793aabef746614d849a9db584e475a6e24186bb163c425f48f7a9831427fcd0",
        "files": {"d3dcompiler_47.dll": "d3dcompiler_47.dll", "DWrite.dll": "DWrite.dll", "LICENSE.txt": "LICENSE-FH2ModPlatform.txt",
                  "LICENSE-MinHook.txt": "LICENSE-MinHook.txt"},
    },
    "extraOptions": {'name': 'FH2 Extra Options',
     'author': 'RokkuDayo',
     'license': 'GPL-3.0',
     'version': '0.3.9',
     'url': 'https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/Azimuth_v1.0.0/FH2_Extra_Options_0.3.9-release.zip',
     'sha256': 'b4b4e31fcc26baec569146c50aee9c060426a7c825fb1dce2641385d9547e058',
     'size': 864058,
     'dll': 'Azimuth/winmm.dll',
     'dllSha256': 'e5bbec928982c9495217522811acd39096e7c8bf994aa0062d82614c8cea64bd',
     'files': {'Azimuth/FH2_ExtraOptions.ini': 'FH2_ExtraOptions.ini',
               'Azimuth/mods/Extra Options - Garage Revving/mod.json': 'mods/Extra Options - Garage Revving/mod.json',
               'Azimuth/mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll': 'mods/Extra Options - Garage '
                                                                                             'Revving/plugins/EO_GarageEngineRev.dll',
               'Azimuth/mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.ini': 'mods/Extra Options - Garage '
                                                                                             'Revving/plugins/EO_GarageEngineRev.ini',
               'Azimuth/mods/Extra Options - Weather Control/mod.json': 'mods/Extra Options - Weather Control/mod.json',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml': 'mods/Extra '
                                                                                                                                           'Options '
                                                                                                                                           '- '
                                                                                                                                           'Weather '
                                                                                                                                           'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml': 'mods/Extra '
                                                                                                                                          'Options '
                                                                                                                                          '- '
                                                                                                                                          'Weather '
                                                                                                                                          'Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl.dll': 'mods/Extra Options - Weather '
                                                                                             'Control/plugins/EO_WeatherControl.dll',
               'Azimuth/mods/Extra Options - Weather Control/plugins/EO_WeatherControl.ini': 'mods/Extra Options - Weather '
                                                                                             'Control/plugins/EO_WeatherControl.ini',
               'Azimuth/winmm.dll': 'winmm.dll'},
     'fileSha256': {'mods/Extra Options - Garage Revving/mod.json': '4f70c5538490f61011f854c7afcdcea91658417aa54bafad98c5e61ea28af00f',
                    'mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll': 'ba6b6df6446c1c0d9f6045759b57e215ba00fe26b9d40e075907dffd56e98031',
                    'mods/Extra Options - Weather Control/mod.json': 'd5e10b1ae3c33e414d2a21a41887bf3c6bd5436192afa726d118297ff3bce547',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_BR.xaml': '7b142d09832022e0e791efb2e6b5c16f6dc8645bb21dcc0dc3a7e7bd8481c5d5',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CHT.xaml': 'f8e1c537da2b7ca6f62db36a5a92f712731b4dc94144279b0d618e0ab32a6c28',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_CZ.xaml': '15362ef2324ae8ba943e410a3ea19335461f75d7dbb16471c3a8bf426b357eca',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_DE.xaml': '7a3498a42bc60e9d74f634e74a0f935398b372795d8cb2d596a324cf0bc5195e',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_EN.xaml': 'ad4a6bf5b51418f64d666ae37d506e01fc49c454f72a7088a1d3be002f440611',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_ES.xaml': '56331483d178f20e7c1ab1e374cdf6618753214874289bfe00c9aa0ed7d80c30',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_FR.xaml': 'be6a094fcac82014e53256c3da88cc6d0348f1438992fca0b32a5be0f15f50e6',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_GB.xaml': 'ad4a6bf5b51418f64d666ae37d506e01fc49c454f72a7088a1d3be002f440611',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_HU.xaml': 'e2f346d00cec861206fe950b33481320545d0593f68f3a68f54c6e1204ee3750',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_IT.xaml': 'e7795a018af51214b35da87898e55b4cfd93a3fec4b841fe2dcbaef9a19d61d9',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_JP.xaml': '1c06d79e4b6fa870c232d3c55a36078896005faaa0a4c19679c66977283198e4',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_KO.xaml': '898a92651ceff28185ffdc554e3b5b6103529c8edeae9879dc430ebf3d1f5558',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_MX.xaml': '56331483d178f20e7c1ab1e374cdf6618753214874289bfe00c9aa0ed7d80c30',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_NL.xaml': '192acd5cb44b7f7afbf483e3633c1054c7866d49ed6f9896f48c0b46bdf45bb4',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_PL.xaml': '71a299061137dc6f6462b1ce199d7fb15eea9bae65fe1c194b1de8c1ee6725f1',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl/Media/UI/Scenes/Anthem/EO_WeatherControl_RU.xaml': 'b71bd9a8b95ab13487aaab89ffef3b1d60eed48cc7cd83d6c089a1d49e55ad71',
                    'mods/Extra Options - Weather Control/plugins/EO_WeatherControl.dll': '238f446d5d8453e22193c9880f7bbbf2453319fd53415edbe06777ce34c09b47'}},
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
