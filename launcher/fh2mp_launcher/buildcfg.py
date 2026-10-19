"""Build-time defaults. A test or private build can carry a `buildcfg.json` next to this file (the release build has none):
    {"api_base": "http://host:5080", "allow_insecure": true, "update_base": "bundled"}
`update_base` is where releases are fetched from instead of GitHub: a URL, or "bundled" for the signed mirror packed inside the executable
(still verified against the release key). User settings and environment (FH2MP_API, FH2MP_UPDATE_BASE) win over these."""
import json
import os
import sys

_cfg = None


def _load():
    global _cfg
    if _cfg is None:
        _cfg = {}
        base = getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        try:
            with open(os.path.join(base, "fh2mp_launcher", "buildcfg.json"), encoding="utf-8") as f:
                _cfg = json.load(f)
        except (OSError, ValueError):
            pass
    return _cfg


def get(key, default=None):
    return _load().get(key, default)


def update_base():
    v = os.environ.get("FH2MP_UPDATE_BASE") or get("update_base") or ""
    if v == "bundled":
        root = os.path.join(getattr(sys, "_MEIPASS", None) or os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fh2mp_launcher", "mirror")
        return "file://" + root
    return v
