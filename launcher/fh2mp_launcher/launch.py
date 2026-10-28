"""Starting the game: sign in (done by the caller), hand the session to the shim, start Azimuth the way this platform needs, clean up after."""
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

from . import azimuth, paths, shim
from .constants import WINE_DLL_OVERRIDES

WIN = sys.platform == "win32"


class LaunchError(Exception):
    pass


# Search-path variables the packaging (PyInstaller bootloader, AppImage) points at the launcher's own bundle
_BUNDLE_PATH_VARS = ("LD_LIBRARY_PATH", "LD_PRELOAD", "PATH", "XDG_DATA_DIRS", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                     "GIO_EXTRA_MODULES", "GDK_PIXBUF_MODULE_FILE", "PYTHONHOME", "PYTHONPATH")
_APPIMAGE_VARS = ("APPDIR", "APPIMAGE", "ARGV0", "OWD")


def host_env(env=None):
    """The environment for a program that is not part of the launcher (Proton, Wine, pkexec): without the launcher's own bundle in it.
    The PyInstaller bootloader puts the bundle's libraries first in LD_LIBRARY_PATH (the original is kept in LD_LIBRARY_PATH_ORIG) and an AppImage
    adds its own paths. The bundle's libstdc++ is older than the one a current Mesa needs (GLIBCXX_3.4.32), so Vulkan failed to load inside
    Proton and Azimuth said "Your graphics card or driver is not supported" (CreateDXGIFactory2 0x80004005, GitHub #10)."""
    env = dict(os.environ if env is None else env)
    if WIN:
        return env
    orig = env.pop("LD_LIBRARY_PATH_ORIG", None)
    if orig is not None:
        env["LD_LIBRARY_PATH"] = orig
    bundle = [os.path.realpath(p) for p in (getattr(sys, "_MEIPASS", ""), env.get("APPDIR", "")) if p]
    for k in _BUNDLE_PATH_VARS:
        if k in env:
            keep = [e for e in env[k].split(":") if e and not any(os.path.realpath(e) == b or os.path.realpath(e).startswith(b + os.sep) for b in bundle)]
            if keep:
                env[k] = ":".join(keep)
            else:
                del env[k]
    for k in _APPIMAGE_VARS:
        env.pop(k, None)
    for k in [k for k in env if k.startswith("_PYI_") or k == "_MEIPASS2"]:
        del env[k]
    return env


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


def gpu_check_failure(folder, since=0.0):
    """True when Azimuth's last session in `folder` stopped at the graphics check (CreateDXGIFactory2: "Your graphics card or driver is not
    supported"). `since` = time the game was started; an older log (mtime before it) says nothing about this run."""
    log = os.path.join(folder, "data", "logs", "azimuth.log")
    try:
        if os.path.getmtime(log) < since:
            return False
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()[-200:]
    except OSError:
        return False
    start = max((i for i, ln in enumerate(lines) if " session " in ln and " began " in ln), default=0)
    return any("STOPPED CreateDXGIFactory2" in ln for ln in lines[start:])


def gpu_check_advice(settings):
    """What to tell the player when the game stopped at the graphics check (empty on Windows). Evidence (GitHub #10): on AMD machines every
    session started by `proton run` outside Steam stops there, every session started from Steam finds the GPU, with any Proton version
    (a Mint tester's log: GE-Proton11-9, Experimental and Valve's Proton 11.0 all work from Steam and fail outside it). Cause not found."""
    if WIN:
        return ""
    return ("Azimuth stopped at the graphics check (\"Your graphics card or driver is not supported\"). Your GPU is probably fine: on some "
            "AMD systems the game only starts from Steam. Workaround: leave this launcher open, add Azimuth to Steam as a non-Steam game with "
            "the launch option WINEDLLOVERRIDES=\"winmm,dwrite,d3dcompiler_47=n,b\" %command% , and start it from Steam.")


def resolve_proton(path):
    """The `proton` script for what the player typed in Settings: the script itself, or the Proton folder (Heroic's GE-Proton-latest/, a
    compatibilitytools.d entry) with the script inside. "" for no setting. Raises LaunchError, saying what to pick, when it cannot run
    (GitHub #10 / issue 37: a folder was executed as if it were the script and the game "could not start: Permission denied")."""
    path = os.path.expanduser((path or "").strip())
    if not path:
        return ""
    if os.path.isdir(path):
        path = os.path.join(path, "proton")
    if not os.path.isfile(path):
        raise LaunchError(f"No `proton` script at {path}. In Settings, pick the `proton` file inside your Proton or GE-Proton folder.")
    if not os.access(path, os.X_OK):
        raise LaunchError(f"{path} is not executable. Run `chmod +x` on it, or pick the `proton` script of another Proton version.")
    return path


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
    game = azimuth.fh2_title(ini).get("folder", "")
    # A game selected through the native Linux picker is stored as Z:\\... .
    # Recover the existing Steam prefix from that actual host path first.
    host_game = game[2:].replace('\\', '/') if game[:2].upper() == 'Z:' else game
    m = re.match(r"^(.*/compatdata/[^/]+)/pfx/", os.path.realpath(host_game) + '/') if host_game.startswith('/') else None
    if m and os.path.isdir(os.path.join(m.group(1), 'pfx')):
        return m.group(1)
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
        return argv, {"WINEDLLOVERRIDES": WINE_DLL_OVERRIDES}, folder
    proton = resolve_proton(settings["proton"]) if mode in ("auto", "proton") else ""
    proton = proton or (find_proton() if mode in ("auto", "proton") else "")
    if mode in ("auto", "proton") and proton:
        # Never fall back to the system Wine just because no Steam prefix was found: distro Wine is often too old for Azimuth
        # ("Your graphics card or driver is not supported", CreateDXGIFactory2). Proton makes its own prefix on first run.
        prefix = os.path.expanduser(settings["prefix"]) if settings["prefix"] else detect_prefix(folder) or os.path.join(paths.config_dir(), "prefix")
        os.makedirs(prefix, exist_ok=True)
        roots = steam_roots()
        env = {"STEAM_COMPAT_DATA_PATH": prefix, "STEAM_COMPAT_CLIENT_INSTALL_PATH": roots[0] if roots else os.path.expanduser("~/.local/share/Steam"),
               "WINEDLLOVERRIDES": WINE_DLL_OVERRIDES}
        return [proton, "run", exe, *args], env, folder
    if mode == "proton":
        raise LaunchError("Proton not found. In Settings, set the path to the `proton` script (for example a Proton 11 or GE-Proton folder).")
    if mode == "wine" and shutil.which("wine"):      # only when the player asks for it: distro Wine fails Azimuth (CreateDXGIFactory2, GitHub #10)
        env = {"WINEDLLOVERRIDES": WINE_DLL_OVERRIDES}
        if settings["prefix"]:
            env["WINEPREFIX"] = os.path.expanduser(settings["prefix"])
        return [shutil.which("wine"), exe, *args], env, folder
    if mode == "wine":
        raise LaunchError("Wine not found. In Settings, install Wine, or choose Proton.")
    raise LaunchError("No Proton found. Install Proton (Steam's Proton 11, or GE-Proton) and pick its `proton` script in Settings. "
                      "The system Wine is not used automatically: it usually fails with \"Your graphics card or driver is not supported\". "
                      "You can also choose Wine yourself or enter a custom launch command in Settings.")


def start(settings, folder, extra_args=(), skip_launcher=True, extra_env=None):
    argv, env_add, cwd = build_command(settings, folder, extra_args, skip_launcher)
    env = host_env()
    env.update(env_add)
    env.update(extra_env or {})          # e.g. FH2MP_SERVER_PORT from the server; the developer's own lines below still win
    for line in (settings["extra_env"] or "").splitlines():            # developer option: KEY=VALUE per line
        k, eq, v = line.strip().partition("=")
        if eq and k and not k.startswith("#") and k != "FH2MP_TOKEN":
            env[k.strip()] = v.strip()
    # The chosen mode and save ID take precedence over inherited/developer environment settings.
    env.pop("FH2MP_OFFLINE", None)
    if (extra_env or {}).get("FH2MP_OFFLINE"):
        env["FH2MP_OFFLINE"] = "1"
        if extra_env.get("FH2MP_XUID"):
            env["FH2MP_XUID"] = extra_env["FH2MP_XUID"]
    # the shim must find the session in its own folder, never in the environment of an unrelated process
    env.pop("FH2MP_TOKEN", None)
    try:
        with open(os.path.join(paths.cache_dir(), "game-launch.log"), "ab") as log:
            log.write(("\n--- " + " ".join(shlex.quote(a) for a in argv) + "\n").encode())
            log.flush()
            p = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 **({"creationflags": 0x00000008 | 0x00000200} if WIN else {"start_new_session": True}))
    except OSError as e:
        raise LaunchError(f"could not start the game: {e.strerror or e}") from None
    return p


def cleanup(folder):
    shim.clear_session(folder)
