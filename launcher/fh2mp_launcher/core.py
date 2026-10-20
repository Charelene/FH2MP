"""The launcher's logic with no user interface: the UI (and the tests) call these and show the results."""
import os
import platform

from . import azimuth, keystore, launch, paths, shim, updater
from .api import Api, ApiError
from .constants import BUILTIN_AZIMUTH_BUILDS, DEFAULT_WEBSITE, EXTRA_OPTIONS_VERSION
from .identity import Identity
from .settings import Settings
from .version import __version__


class PlayBlocked(Exception):
    """Something the player has to fix first; the message says what."""

    def __init__(self, message, code=""):
        super().__init__(message)
        self.code = code


FRIENDLY = {
    "unreachable": "Can't reach the server.",
    "banned": "This account is banned.",
    "registration_closed": "Registration is closed.",
    "invite_required": "Invite code needed.",
    "gamertag_taken": "That gamertag is taken.",
    "key_exists": "This key already has an account.",
    "rename_cooldown": "Names can change once a week.",
    "rate_limited": "Too many attempts. Wait a minute.",
    "ip_changed": "Your address changed. Restart from the launcher.",
    "token_expired": "Session expired. Press Play.",
}


def friendly(e):
    if isinstance(e, ApiError):
        text = FRIENDLY.get(e.code) or e.message or str(e)
        if e.code == "unreachable" and e.detail:
            text += f" ({e.detail[:300]})"          # the reason, so a screenshot is enough to tell what is wrong on that computer
        return text
    return str(e)


class Launcher:
    def __init__(self, settings=None):
        self.settings = settings or Settings()
        self.identity = keystore.load()
        self.server = {}                # last /v1/launcher/config
        self.token = ""
        self.account = None             # dict from the API: xuid, gamertag
        self.process = None
        self.manifest = None

    # ---- server
    def api(self):
        s = self.settings
        return Api(s["api_base"] or None, s["channel"], s["allow_insecure"])

    def refresh_server(self):
        self.server = self.api().config()
        return self.server

    @property
    def links(self):
        l = (self.server or {}).get("links") or {}
        return {"website": l.get("website") or DEFAULT_WEBSITE, "patreon": l.get("patreon") or "", "discord": l.get("discord") or ""}

    @property
    def online(self):
        return (self.server or {}).get("online")

    def builds(self):
        return list((self.server or {}).get("supportedAzimuth") or []) + BUILTIN_AZIMUTH_BUILDS

    # ---- local state
    def azimuth_info(self):
        return azimuth.inspect(self.settings["azimuth_dir"], self.builds())

    def shim_state(self):
        d = self.settings["azimuth_dir"]
        return shim.installed(d) if d and os.path.isdir(d) else ("none", "")

    # ---- identity
    def has_identity(self):
        return self.identity is not None

    def create_identity(self):
        self.identity = Identity.generate()
        return keystore.save(self.identity)

    def restore_identity(self, code):
        ident = Identity.from_recovery_code(code)
        self.identity = ident
        return keystore.save(ident)

    def forget_identity(self):
        keystore.delete()
        self.identity, self.token, self.account = None, "", None

    def sign_up(self, gamertag, invite="", progress=None, cancelled=lambda: False):
        if not self.identity:
            raise PlayBlocked("no key yet")
        try:
            acct = self.api().register(self.identity, gamertag, invite, progress, cancelled)
        except ApiError as e:
            # The answer may simply not have arrived (the request went through), or an earlier try already made the account: the key is
            # the account, so ask the server whether this key can sign in before treating it as a failure.
            if not e.uncertain and e.code != "key_exists":
                raise
            try:
                acct = self.sign_in()
            except ApiError as e2:
                # only "no account for this key" proves the registration did not happen; any other failure leaves the key worth keeping
                e.uncertain = e2.code != "unknown_account"
                raise e from None
        self.settings["gamertag"], self.settings["xuid"] = acct["gamertag"], acct["xuid"]
        self.settings.save()
        return acct

    def sign_in(self):
        """-> account dict. Raises ApiError (404 unknown_account when the key has no account on this server)."""
        if not self.identity:
            raise PlayBlocked("no key yet")
        r = self.api().login(self.identity, f"launcher/{__version__} {platform.system().lower()}")
        self.token, self.account = r["token"], r["account"]
        self._login_reply = r
        self.settings["gamertag"], self.settings["xuid"] = self.account["gamertag"], self.account["xuid"]
        self.settings.save()
        return self.account

    def sign_out(self):
        if self.token:
            self.api().logout(self.token)
        self.token = ""

    # ---- updates
    def check_updates(self):
        """-> manifest (verified) or raises updater.UpdateError. Remembers it."""
        self.manifest = updater.Updater(channel=self.settings["channel"]).fetch_manifest()
        return self.manifest

    def update_status(self):
        """What a refresh found: dict(launcher=None|version, shim=None|reason, min_launcher_ok=bool)."""
        m = self.manifest
        if not m:
            return {"launcher": None, "shim": None, "minLauncherOk": True}
        kind, ver = self.shim_state()
        out = {"launcher": None, "shim": None, "minLauncherOk": True}
        lv = m.get("launcher", {}).get("version", "")
        if updater.newer(lv, __version__):
            out["launcher"] = lv
        if self.settings["azimuth_dir"] and not self.shim_pinned(kind, ver):
            out["shim"] = updater.shim_update_needed(kind, ver, m)
        need = max((m.get("minLauncher", "0.0.0"), (self.server or {}).get("minLauncher", "0.0.0")), key=updater.vkey)
        out["minLauncherOk"] = not updater.newer(need, __version__)
        return out

    def shim_pinned(self, kind=None, ver=None):
        """True when the installed shim must be left alone: pinned in settings, or a debug build (version ends in -debug)."""
        if kind is None:
            kind, ver = self.shim_state()
        return kind == "ours" and (bool(self.settings["pin_shim"]) or (ver or "").endswith("-debug"))

    def install_shim_file(self, path):
        """Developer: installs a shim DLL (or a zip with winmm.dll) from disk, e.g. shim/build/winmm-debug.dll. Not signed, not checked
        against a manifest: the dev launcher only."""
        import zipfile
        if not paths.is_dev():
            raise PlayBlocked("installing a local shim is a dev launcher feature")
        if path.lower().endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                data = z.read("winmm.dll")
        else:
            with open(path, "rb") as f:
                data = f.read()
        did = shim.install(self.settings["azimuth_dir"], data)
        self.settings["dev_shim_path"] = path
        self.settings.save()
        return did

    def install_shim(self, progress=None, cancelled=lambda: False):
        if not self.manifest:
            self.check_updates()
        e = self.manifest["shim"]
        up = updater.Updater(channel=self.settings["channel"])
        zip_path = up.download(self.manifest["tag"], e, progress, cancelled)
        import zipfile
        with zipfile.ZipFile(zip_path) as z:
            names = [n for n in z.namelist() if n.endswith("winmm.dll")]
            if len(names) != 1:
                raise updater.UpdateError("the shim package is not what was expected")
            data = z.read(names[0])
        expect = e.get("dllSha256")
        did = shim.install(self.settings["azimuth_dir"], data, expect)
        self.carry_over_mod()
        did += self.ensure_extra_options(progress, cancelled)
        return did

    # ---- play
    def prepare_play(self, force_unsupported=False, require_game=True):
        """Everything up to (not including) starting the process: checks, sign-in, session hand-over. -> (account, notes)."""
        s = self.settings
        if not s["azimuth_dir"]:
            raise PlayBlocked("Choose your Azimuth folder.", "no_azimuth")
        self.refresh_server()
        info = self.azimuth_info()
        if not info.found:
            raise PlayBlocked("azimuth.exe not found.", "no_azimuth")
        if not info.supported and not force_unsupported:
            raise PlayBlocked(f"Azimuth {info.version or '?'} is untested with FH2MP.", "azimuth_unsupported")
        kind, _v = self.shim_state()
        if kind != "ours":
            raise PlayBlocked("Shim not installed.", "no_shim")
        try:
            self.ensure_extra_options()          # only when the player has no copy; never blocks playing
        except Exception:  # noqa: BLE001
            pass
        self.ensure_game()
        if require_game and not azimuth.has_game_configured(s["azimuth_dir"]):
            raise PlayBlocked("Couldn't find your Forza Horizon 2 files.", "no_game")
        if (self.server or {}).get("maintenance"):
            raise PlayBlocked(self.server["maintenance"], "maintenance")
        acct = self.sign_in()
        r = self._login_reply
        lsp = (r.get("lsp") or {}).get("url") or (self.server.get("lsp") or {}).get("url")
        if not lsp:
            raise PlayBlocked("The server sent no address.", "no_lsp")
        d = s["azimuth_dir"]
        shim.write_session(d, acct["xuid"], self.token, lsp)
        shim.write_identity(d, acct["xuid"])
        notes = []
        adopted = azimuth.adopt_saves(d, acct["xuid"])
        if adopted:
            notes.append("kept your progress: " + adopted)
        if azimuth.set_player_name(d, acct["gamertag"]):
            notes.append(f"set the in-game name to {acct['gamertag']}")
        return acct, notes

    def prepare_session_only(self, force_unsupported=True):
        """Developer: sign in and write the session next to the shim without starting the game (start it yourself: rig, Steam, a debugger)."""
        return self.prepare_play(force_unsupported)

    def play(self, force_unsupported=False, azimuth_window=False):
        """azimuth_window: start Azimuth's own launcher window (its settings) instead of going straight into the game."""
        acct, notes = self.prepare_play(force_unsupported, require_game=not azimuth_window)
        try:
            self.process = launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=not azimuth_window)
        except launch.LaunchError:
            launch.cleanup(self.settings["azimuth_dir"])
            raise
        return acct, notes

    def ensure_game(self):
        """If Azimuth does not know the game folder yet, find the install and write it into Azimuth's settings. -> path set, or ''. """
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or azimuth.has_game_configured(d):
            return ""
        found = azimuth.find_game_installs()
        if not found:
            return ""
        host, win = found[0]
        azimuth.configure_game(d, win)
        return host

    # ---- FH2 Extra Options (optional third-party mod, imported from the player's own copy)
    def extra_state(self):
        """-> (the supported FH2 Extra Options 0.2.98 is in place, version, Azimuth version it targets)."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or not shim.extra_options_supported(d):
            return False, "", ""
        _i, _v, a = shim.extra_options_installed(d)
        return True, EXTRA_OPTIONS_VERSION, a

    def extra_sources(self):
        d = self.settings["azimuth_dir"]
        others = [c for c in azimuth.find_candidates() if os.path.realpath(c) != os.path.realpath(d or "/nonexistent")]
        return shim.find_extra_sources(others)

    def carry_over_mod(self):
        """The player's own FH2 Extra Options, when it is in another Azimuth folder of theirs and not in this one: copied over quietly (no UI)."""
        try:
            if not self.extra_state()[0]:
                found = self.extra_sources()
                if found:
                    self.import_extra(found[0]["path"])
        except Exception:  # noqa: BLE001 - never blocks the install
            pass

    def ensure_extra_options(self, progress=None, cancelled=lambda: False):
        """The shim's menus (Social, Clubs, Rivals, Marketplace) and several game fixes come from FH2 Extra Options. When it is not in the
        Azimuth folder (and not found in another of the player's folders), install the author's unchanged release from our signed assets.
        Only 0.2.98 is supported (the shim is built against it): another version in the folder is moved aside and replaced.
        Does nothing when 0.2.98 is there, when the player turned this off in Settings, or when the release carries no copy. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not self.settings["extra_options"] or not d or not os.path.isdir(d) or self.extra_state()[0]:
            return []
        self.carry_over_mod()
        if self.extra_state()[0]:
            return ["imported your FH2 Extra Options"]
        if not self.manifest:
            self.check_updates()
        e = (self.manifest or {}).get("extraOptions")
        if not e:
            return []
        path = updater.Updater(channel=self.settings["channel"]).download(self.manifest["tag"], e, progress, cancelled)
        return shim.install_bundled_extra(d, path, e.get("dllSha256"), e.get("version", ""))

    def import_extra(self, source):
        d = self.settings["azimuth_dir"]
        did = shim.import_extra_options(d, source)
        mine = os.path.join(paths.config_dir(), "gamerpic.png")          # keep the player's own picture, not the mod's default one
        if os.path.isfile(mine):
            with open(mine, "rb") as f:
                azimuth.write_gamerpic(d, f.read())
        return did

    def open_azimuth_setup(self):
        """Azimuth's own launcher window (where its game folder is chosen), started the same way the game is."""
        return launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=False)

    def game_running(self):
        return self.process is not None and self.process.poll() is None

    def after_game(self):
        launch.cleanup(self.settings["azimuth_dir"])
        if self.token:
            try:
                self.sign_out()
            except ApiError:
                pass
