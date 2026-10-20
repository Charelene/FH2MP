"""Starting the game: sign in (done by the caller), hand the session to the shim, start Azimuth the way this platform needs, clean up after."""
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

from . import azimuth, paths, shim

WIN = sys.platform == "win32"


class LaunchError(Exception):
    pass


def find_proton():
    """The newest `proton` script in the usual Steam compatibility-tool folders."""
    roots = ["~/.steam/steam/compatibilitytools.d", "~/.local/share/Steam/compatibilitytools.d", "/usr/share/steam/compatibilitytools.d",
             "~/.steam/steam/steamapps/common", "~/.local/share/Steam/steamapps/common"]
    found = []
    for r in roots:
        found += glob.glob(os.path.join(os.path.expanduser(r), "*", "proton"))
    found = [f for f in found if os.access(f, os.X_OK) or os.path.isfile(f)]
    # highest version number in the folder name wins ("Proton 11" over "Proton 9"); no number (Experimental) ranks above all
    def rank(f):
        nums = [int(n) for n in re.findall(r"\d+", os.path.basename(os.path.dirname(f)))]
        return (not nums, nums, f)
    return max(found, key=rank) if found else ""


def steam_roots():
    return [os.path.expanduser(r) for r in ("~/.local/share/Steam", "~/.steam/steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam")
            if os.path.isdir(os.path.expanduser(r))]


def _game_dir_in_prefix(prefix_dir, win_path):
    """The folder a Windows path such as C:\\Games\\ForzaHorizon2 refers to inside a Proton prefix (compatdata/<id>)."""
    m = re.match(r"^([A-Za-z]):[\\/]+(.*)$", win_path or "")
    if not m:
        return ""
    return os.path.join(prefix_dir, "pfx", "drive_c" if m.group(1).lower() == "c" else "dosdevices", *re.split(r"[\\/]+", m.group(2)))


def detect_prefix(folder):
    """The Proton prefix (STEAM_COMPAT_DATA_PATH) that can see the game: the one Azimuth sits in, else the Steam compatdata prefix that
    holds the game folder named in Azimuth's own settings (C:\\Games\\ForzaHorizon2 -> compatdata/<id>/pfx/drive_c/Games/ForzaHorizon2)."""
    real = os.path.realpath(folder)
    m = re.match(r"^(.*/compatdata/[^/]+)/pfx/", real + "/")
    if m:
        return m.group(1)
    ini = azimuth.read_ini(folder)
    game = next((v.get("folder", "") for k, v in ini.items() if k.startswith("title-")), "")
    for root in steam_roots():
        for pre in sorted(glob.glob(os.path.join(root, "steamapps", "compatdata", "*"))):
            if game and os.path.isdir(_game_dir_in_prefix(pre, game)):
                return pre
    return ""


def build_command(settings, folder, extra_args=(), skip_launcher=True):
    """-> (argv, env additions, cwd). Raises LaunchError with something the player can act on."""
    exe = os.path.join(folder, "azimuth.exe")
    args = [*(["--skip-launcher"] if skip_launcher else []), *shlex.split(settings["extra_args"] or ""), *extra_args]
    env = {}
    if WIN:
        return [exe, *args], env, folder
    mode = settings["launch_mode"]
    if mode == "custom" or (mode == "auto" and settings["custom_command"]):
        tpl = settings["custom_command"].strip()
        if not tpl:
            raise LaunchError("Set the launch command in Settings, or pick Proton/Wine.")
        argv = [a.replace("{exe}", exe).replace("{dir}", folder) for a in shlex.split(tpl)]
        return argv, {"WINEDLLOVERRIDES": "winmm=n,b"}, folder
    proton = os.path.expanduser(settings["proton"]) if settings["proton"] else find_proton()
    if mode in ("auto", "proton") and proton:
        # Never fall back to the system Wine just because no Steam prefix was found: distro Wine is often too old for Azimuth
        # ("Your graphics card or driver is not supported", CreateDXGIFactory2). Proton makes its own prefix on first run.
        prefix = os.path.expanduser(settings["prefix"]) if settings["prefix"] else detect_prefix(folder) or os.path.join(paths.config_dir(), "prefix")
        os.makedirs(prefix, exist_ok=True)
        roots = steam_roots()
        env = {"STEAM_COMPAT_DATA_PATH": prefix, "STEAM_COMPAT_CLIENT_INSTALL_PATH": roots[0] if roots else os.path.expanduser("~/.local/share/Steam"),
               "WINEDLLOVERRIDES": "winmm=n,b"}
        return [proton, "run", exe, *args], env, folder
    if mode == "proton":
        raise LaunchError("Proton not found. In Settings, set the path to the `proton` script (for example a Proton 11 or GE-Proton folder).")
    if mode in ("auto", "wine") and shutil.which("wine"):
        env = {"WINEDLLOVERRIDES": "winmm=n,b"}
        if settings["prefix"]:
            env["WINEPREFIX"] = os.path.expanduser(settings["prefix"])
        return [shutil.which("wine"), exe, *args], env, folder
    raise LaunchError("Could not work out how to run Azimuth on this system (no Proton found)"
                      ". In Settings, choose the Proton script, or enter a custom launch command.")


def start(settings, folder, extra_args=(), skip_launcher=True, extra_env=None):
    argv, env_add, cwd = build_command(settings, folder, extra_args, skip_launcher)
    env = dict(os.environ)
    env.update(env_add)
    env.update(extra_env or {})          # e.g. FH2MP_SERVER_PORT from the server; the developer's own lines below still win
    for line in (settings["extra_env"] or "").splitlines():            # developer option: KEY=VALUE per line
        k, eq, v = line.strip().partition("=")
        if eq and k and not k.startswith("#") and k != "FH2MP_TOKEN":
            env[k.strip()] = v.strip()
    # the shim must find the session in its own folder, never in the environment of an unrelated process
    env.pop("FH2MP_TOKEN", None)
    try:
        log = open(os.path.join(paths.cache_dir(), "game-launch.log"), "ab")     # Proton / Wine output: the first place to look when nothing opens
        log.write(("\n--- " + " ".join(shlex.quote(a) for a in argv) + "\n").encode())
        log.flush()
        p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                             **({"creationflags": 0x00000008 | 0x00000200} if WIN else {"start_new_session": True}))
    except OSError as e:
        raise LaunchError(f"could not start the game: {e.strerror or e}") from None
    return p


def cleanup(folder):
    shim.clear_session(folder)
