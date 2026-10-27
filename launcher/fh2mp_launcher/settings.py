"""User settings (JSON in the config folder). Nothing secret lives here; the key is in keystore.py."""
import json
import os

from . import buildcfg, paths

DEFAULTS = {
    "setup_revision": 0,             # independent of account/key existence; completed only on the wizard's final page
    "azimuth_dir": "",
    "channel": "stable",            # stable | dev
    "api_base": "",                 # empty = the channel's default
    "allow_insecure": False,        # plain http to a non-local server (testing only)
    "launch_mode": "auto",          # auto | proton | wine | custom   (Windows always starts azimuth.exe directly)
    "proton": "",                   # path to the `proton` script (Linux)
    "prefix": "",                   # Proton prefix directory = STEAM_COMPAT_DATA_PATH (Linux)
    "custom_command": "",           # e.g. steam steam://rungameid/1234567890 ; {exe} {dir} are replaced
    "check_updates": True,
    "offline_mode": False,          # solo play without sign-in, server checks or downloads
    "gamertag": "",                 # last known, for display before the server answers
    "xuid": 0,
    "extra_args": "",
    "tcp_fallback": False,          # experimental: carry the game's UDP over TCP (for networks that drop UDP)
    "crash_report_ask": True,       # offer to send a report when the game crashed
    "crash_handled": 0.0,           # crashes recorded before this time (epoch) were already offered; 0 = not set up yet
    "extra_options": True,          # put FH2 Extra Options (GPL-3.0, RokkuDayo) next to the shim when the player has no copy
    # developer options (the dev launcher's panel; harmless elsewhere)
    "pin_shim": False,              # never replace the installed shim automatically (local / debug builds)
    "extra_env": "",                # KEY=VALUE lines added to the game's environment (FH2MP_NET_TRAFFIC=1, FH2MP_XUID=..., ...)
    "dev_shim_path": "",            # last local shim file installed
}


class Settings:
    def __init__(self, path=None):
        self.path = path or os.path.join(paths.config_dir(), "settings.json")
        self.data = dict(DEFAULTS)
        if buildcfg.get("api_base"):
            self.data["api_base"] = os.environ.get("FH2MP_API") or buildcfg.get("api_base")
            self.data["allow_insecure"] = bool(buildcfg.get("allow_insecure", False))
        if paths.is_dev():
            # until the dev domain has DNS, the dev stack is reached through `ssh -N -L 127.0.0.1:5180:127.0.0.1:5180 <vps>`
            self.data.update(channel="dev", allow_insecure=True, pin_shim=True,
                             api_base=os.environ.get("FH2MP_API", "http://127.0.0.1:5180"))
        try:
            with open(self.path, encoding="utf-8") as f:
                loaded = json.load(f)
            self.data.update({k: v for k, v in loaded.items() if k in DEFAULTS})
        except (OSError, ValueError):
            pass

    def __getitem__(self, k):
        return self.data[k]

    def __setitem__(self, k, v):
        if k not in DEFAULTS:
            raise KeyError(k)
        self.data[k] = v

    def save(self):
        paths.atomic_write(self.path, json.dumps(self.data, indent=1).encode(), 0o600)
