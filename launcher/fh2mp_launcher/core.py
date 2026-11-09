"""The launcher's logic with no user interface: the UI (and the tests) call these and show the results."""
import os
import platform
import re
import time
import threading

from . import azimuth, gamedb, hotfix, keystore, launch, logs, memguard, paths, shim, tunnel, updater, upstream
from .api import Api, ApiError
from .constants import AZIMUTH_VERSION, BUILTIN_AZIMUTH_BUILDS, DEFAULT_WEBSITE, MODS_DIR, PLATFORM_VERSION, SHIM_DLL
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
    "invalid_token": "Session ended. Close other launcher windows, then restart the game from this launcher.",
}


def friendly(e):
    if isinstance(e, ApiError):
        text = FRIENDLY.get(e.code) or e.message or str(e)
        if e.code == "unreachable" and e.detail:
            text += f" ({e.detail[:300]})"          # the reason, so a screenshot is enough to tell what is wrong on that computer
        return text
    return str(e)


def game_port_env(game):
    """The game server's UDP port as the shim wants it: the game has the port 5000 built in, the shim changes it when FH2MP_SERVER_PORT (exactly 4 digits)
    is set. Without this, a client signed in to another stack (dev) would still talk to production's game server. `game` is the server's {"host", "port"}.
    A server that says "traffic": 1 (LSP env FH2MP_NET_TRAFFIC=1, dev) also gets FH2MP_NET_TRAFFIC=1: the shim then leaves the game's networked
    traffic on instead of making every client simulate its own (the shim only looks whether the variable exists)."""
    if not isinstance(game, dict):
        return {}
    env = {}
    port = game.get("port")
    if isinstance(port, int) and not isinstance(port, bool) and 1000 <= port <= 9999:
        env["FH2MP_SERVER_PORT"] = str(port)
    traffic = game.get("traffic")
    if traffic == 1 and not isinstance(traffic, bool):
        env["FH2MP_NET_TRAFFIC"] = "1"
    return env


def read_shim_zip(path):
    """A shim zip (release/build_shim_zip.sh) -> the bytes of its fh2-mods/FH2MP.dll. Raises updater.UpdateError when it is not one.
    Windows' Compress-Archive (the test build's mirror) may store the name with a backslash."""
    import zipfile
    try:
        with zipfile.ZipFile(path) as z:
            names = {n.replace("\\", "/"): n for n in z.namelist()}
            return z.read(names[f"{MODS_DIR}/{SHIM_DLL}"])
    except (zipfile.BadZipFile, KeyError):
        raise updater.UpdateError("the shim package is not what was expected") from None


def release_summary(body, limit=110):
    """Release notes (markdown) -> their first bullet as plain text, shortened to one or two card lines."""
    lines = [ln.strip() for ln in (body or "").splitlines()]
    pick = next((ln[2:] for ln in lines if ln.startswith(("- ", "* "))), "") or next((ln for ln in lines if ln and ln[0] not in "#>"), "")
    pick = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", pick)
    pick = re.sub(r"[*_`]", "", pick).strip()
    return pick if len(pick) <= limit else pick[:limit - 1].rstrip(" ,.;:") + "…"


def lap_time(seconds):
    """88.25 -> "1:28.250"; "" for anything that is not a time."""
    if not isinstance(seconds, (int, float)) or seconds <= 0:
        return ""
    m, sec = divmod(float(seconds), 60)
    return f"{int(m)}:{sec:06.3f}"


def ago(t, now=None):
    """unix time -> "just now" / "5 min ago" / "3 h ago" / "2 days ago"; "" when unknown."""
    if not isinstance(t, (int, float)) or t <= 0:
        return ""
    d = int((time.time() if now is None else now) - t)
    if d < 60:
        return "just now"
    if d < 3600:
        return f"{d // 60} min ago"
    if d < 2 * 86400:
        return f"{d // 3600} h ago"
    return f"{d // 86400} days ago"


def notes_markdown(body):
    """GitHub release notes for the detail view: quotes become plain paragraphs, headings one level smaller."""
    body = re.sub(r"(?m)^> ?", "", body)
    return re.sub(r"(?m)^(#{1,5}) ", lambda m: "#" * min(6, len(m.group(1)) + 1) + " ", body)


def event_card(e, ends, card):
    """A Monthly Rivals event (as /v1/home lists it) -> its home card: the best time on the card, or a call to set the first one."""
    rows = [r for r in e.get("rows") or [] if isinstance(r, dict)]
    text = (f"Best: {rows[0].get('gamertag', '')} · {lap_time(rows[0].get('score'))}" if rows else "No times yet. Set the first one!")
    facts = [(k, v) for k, v in (("Track", e.get("track")), ("Length", f"{e['km']} km" if e.get("km") else ""),
                                 ("Laps", str(e["laps"]) if e.get("laps") else ""), ("Cars", e.get("description")),
                                 ("Ends", ends.replace("ends ", "") if ends else "")) if v]
    return card("challenge", "MONTHLY CHALLENGE", e.get("name") or e.get("track") or "Monthly Rivals", text, glyph=e.get("class") or "",
                facts=facts, rows=[{"pos": r.get("position", i + 1), "name": str(r.get("gamertag") or ""), "value": lap_time(r.get("score")),
                                    "extra": str(r.get("car") or "")} for i, r in enumerate(rows)],
                action="play", action_label="Beat these times" if rows else "Set the first time")


def countdown(ends_at, now=None):
    """unix time -> "ends in 9 days" / "ends in 5 h" / "ends in 20 min"; "" when unknown or past."""
    if not isinstance(ends_at, (int, float)):
        return ""
    left = int(ends_at - (time.time() if now is None else now))
    if left <= 0:
        return ""
    if left >= 2 * 86400:
        return f"ends in {left // 86400} days"
    if left >= 3600:
        return f"ends in {left // 3600} h"
    return f"ends in {max(1, left // 60)} min"


class Launcher:
    def __init__(self, settings=None, load_identity=True):
        self.settings = settings or Settings()
        self.identity = None
        self.identity_loaded = False
        if load_identity:                # the app passes False and loads it off the UI thread: a locked keyring waits for a password prompt
            self.load_identity()
        self.server = {}                # last /v1/launcher/config
        self.home = None                # last /v1/home (the home page: news, release notes, Azimuth, Rivals events, records, stats)
        self.releases = []              # newest GitHub releases (patch-notes card); refreshed hourly (GitHub allows 60 calls an hour)
        self.stats = {}                 # the website's public counters (community card)
        self._extras_at = {}
        self.token = ""
        self._session_lock = threading.RLock()
        self.account = None             # dict from the API: xuid, gamertag
        self.process = None
        self.manifest = None            # the newest signed release manifest of the channel (check_updates)
        self.own_manifest = None        # this launcher's own release's, fetched when the newest offers it no shim (install_shim)
        self.gamedb_synced = False      # the server sets a car database and this start put it in place (sync_gamedb)
        self.hotfix = hotfix.Result()   # what the hotfix document asks of this setup (apply_hotfix, at Play)
        self.hotfix_disabled = []       # mods it turned off at the last Play
        self.gamerpic_converter = None      # set by the UI: server PNG bytes -> the PNG the game expects (needs Qt)

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

    def refresh_home(self):
        """/v1/home; a server from before it (404) gets the home cards from public sources instead (refresh_extras)."""
        try:
            self.home = self.api().home()
        except ApiError as e:
            if e.status != 404:
                raise
            self.home = None
            self.refresh_extras()
        return self.home

    def refresh_extras(self):
        """The home cards that do not come from the game server: release notes (GitHub, hourly) and the website's stats (5 min).
        Each source fails on its own; a failure keeps what was there."""
        now = time.time()
        if now - self._extras_at.get("releases", 0) > 3600:
            self._extras_at["releases"] = now
            try:
                self.releases = updater.Updater(channel=self.settings["channel"]).releases()
            except Exception as e:  # noqa: BLE001 - no card is better than an error about a card
                self._extras_at["releases"] = now - 3300           # try again in 5 minutes
                logs.swallowed("home", "home.releases_failed", e, level="INFO")
        if now - self._extras_at.get("stats", 0) > 300:
            self._extras_at["stats"] = now
            try:
                st = self.api().get_json(self.links["website"].rstrip("/") + "/api/stats/")
                self.stats = st if isinstance(st, dict) and st.get("available") else {}
            except Exception as e:  # noqa: BLE001
                logs.swallowed("home", "home.stats_failed", e, level="INFO")

    def home_data(self):
        """The /v1/home document, or the same shape put together from the server config, GitHub and the website for an older server."""
        if isinstance(self.home, dict):
            return self.home
        srv = self.server or {}
        rel = next((r for r in self.releases or [] if isinstance(r, dict) and r.get("tag")), None)
        st = self.stats or {}
        return {"motd": srv.get("motd", ""), "news": srv.get("news") or [], "heroImage": srv.get("heroImage", ""), "release": rel,
                "azimuth": {"latest": "", "supported": srv.get("latestAzimuth", ""), "posts": []}, "rotation": {}, "events": [],
                "records": [], "cars": [],
                "stats": {"online": srv.get("online"), "accounts": st.get("accounts"), "clubs": st.get("clubs"), "runs": st.get("scores"),
                          "designs": st.get("userContent")} if st else {}}

    @property
    def motd(self):
        m = self.home_data().get("motd")
        return m if isinstance(m, str) else ""

    @property
    def hero_image(self):
        u = self.home_data().get("heroImage") or ""
        return u if isinstance(u, str) and u.startswith("https://") else ""

    def home_cards(self, now=None):
        """The home carousel, most interesting first: the server's news, the latest release, this month's first Rivals event, the newest
        record, the newest Azimuth devlog post, the car of the week, the community, then the other events.
        Each card: {kind, tag, title, text, glyph (big letters on its art), image, url + link (outside page), and for its detail view:
        body (markdown), facts [(label, value)], rows [{pos, name, value, extra}], action ("play" + its label)}."""
        now = time.time() if now is None else now
        h = self.home_data()

        def card(kind, tag, title, text="", url="", link="", image="", glyph="", body="", facts=(), rows=(), action="", action_label=""):
            return {"kind": kind, "tag": tag, "title": title, "text": text, "url": url, "link": link, "image": image, "glyph": glyph,
                    "body": body, "facts": list(facts), "rows": list(rows), "action": action, "action_label": action_label}

        def https(u, prefix="https://"):
            return u if isinstance(u, str) and u.startswith(prefix) else ""
        out = []
        for n in h.get("news") or []:
            if isinstance(n, dict) and n.get("title"):
                url = str(n.get("url") or "")
                out.append(card("news", str(n.get("tag") or "NEWS").upper(), str(n["title"]), str(n.get("text") or ""),
                                url if url.startswith(("https://", "http://")) else "", "Read more", https(n.get("image")),
                                body=str(n.get("text") or "")))
        rel = h.get("release")
        if isinstance(rel, dict) and rel.get("tag"):
            out.append(card("news", "PATCH NOTES", rel.get("name") or f"FH2MP {rel['tag']}", release_summary(rel.get("body", "")),
                            https(rel.get("url"), "https://github.com/"), "View on GitHub",
                            body=notes_markdown(str(rel.get("body") or ""))))
        ends = countdown((h.get("rotation") or {}).get("endsAt"), now)
        events = [event_card(e, ends, card) for e in h.get("events") or [] if isinstance(e, dict)]
        out += events[:1]
        rec = next((r for r in h.get("records") or [] if isinstance(r, dict)), None)
        if rec:
            when = ago(rec.get("at"), now)
            out.append(card("record", "NEW RECORD", str(rec.get("gamertag") or ""),
                            f"{rec.get('event', '')} · {lap_time(rec.get('score'))}" + (f" · {when}" if when else ""), glyph="#1",
                            facts=[(k, v) for k, v in (("Event", rec.get("event")), ("Time", lap_time(rec.get("score"))), ("Car", rec.get("car")),
                                                       ("Class", rec.get("class")), ("Set", when)) if v],
                            action="play", action_label="Beat it"))
        post = next((p for p in (h.get("azimuth") or {}).get("posts") or [] if isinstance(p, dict) and https(p.get("url"))), None)
        if post:
            when = ago(post.get("date"), now)
            out.append(card("news", "AZIMUTH DEVLOG", str(post.get("title") or ""), str(post.get("summary") or ""), post["url"],
                            "Read on itch.io", body=str(post.get("summary") or "") + (f"\n\n*Posted {when}*" if when else "")))
        cars = [c for c in h.get("cars") or [] if isinstance(c, dict) and c.get("car")]
        if cars:
            out.append(card("car", "CAR OF THE WEEK", str(cars[0]["car"]), f"{int(cars[0].get('runs') or 0):,} Rivals runs this week",
                            body="Most raced in Rivals over the last seven days.",
                            rows=[{"pos": i + 1, "name": str(c["car"]), "value": f"{int(c.get('runs') or 0):,} runs", "extra": ""}
                                  for i, c in enumerate(cars)]))
        st = h.get("stats") or {}
        if isinstance(st.get("accounts"), int):
            facts = [("Online now", st.get("online")), ("Drivers", st.get("accounts")), ("Car clubs", st.get("clubs")),
                     ("Rivals times", st.get("runs")), ("Shared designs", st.get("designs"))]
            out.append(card("community", "COMMUNITY", f"{st['accounts']:,} drivers",
                            " · ".join(f"{v:,} {k.lower()}" for k, v in facts[2:4] if isinstance(v, int)), self.links["website"], "Open the website",
                            facts=[(k, f"{v:,}") for k, v in facts if isinstance(v, int)]))
        return out + events[1:]

    def builds(self):
        return list(BUILTIN_AZIMUTH_BUILDS)

    # ---- local state
    def azimuth_info(self):
        return azimuth.inspect(self.settings["azimuth_dir"], self.builds())

    def shim_state(self):
        d = self.settings["azimuth_dir"]
        return shim.installed(d) if d and os.path.isdir(d) else ("none", "")

    # ---- identity
    def load_identity(self):
        self.identity = keystore.load()
        self.identity_loaded = True

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
        with self._session_lock:
            return self._sign_in()

    def _sign_in(self):
        if not self.identity:
            raise PlayBlocked("no key yet")
        # One session per account: the sign-in ends any token handed out before (auth.session_replaced on the server), a running game's too.
        t0 = time.monotonic()
        replacing = bool(self.token)
        r = self.api().login(self.identity, f"launcher/{__version__} {platform.system().lower()}")
        self.token, self.account = r["token"], r["account"]
        self._login_reply = r
        logs.info("auth", "auth.signed_in", xuid=f"{self.account['xuid']:#x}", replacing_session=replacing, game_running=self.game_running(),
                  elapsed_ms=int((time.monotonic() - t0) * 1000))
        self.settings["gamertag"], self.settings["xuid"] = self.account["gamertag"], self.account["xuid"]
        self.settings.save()
        return self.account

    def community(self, path="", method="GET", body=None):
        """Social actions share the game session; never replace a live game's token."""
        with self._session_lock:
            if self.settings["offline_mode"] or getattr(self, "offline_run", False):
                raise PlayBlocked("Friends and lobbies are unavailable in offline mode.")
            if not self.token:
                if self.game_running():
                    raise PlayBlocked("The game session is unavailable. Close the game before signing in again.")
                logs.info("auth", "auth.sign_in_needed", why="social call without a token", path=path or None)
                self._sign_in()
            try:
                return self.api().community(self.token, path, method, body)
            except ApiError as e:
                # A revoked token may belong to another launcher using this account.
                # Automatically signing in here would revoke its token in turn.
                if e.code != "token_expired" or self.game_running():
                    raise
                logs.info("auth", "auth.sign_in_needed", why="token_expired", path=path or None)
                self._sign_in()
                return self.api().community(self.token, path, method, body)

    def sign_out(self):
        with self._session_lock:
            if self.token:
                self.api().logout(self.token)
            self.token = ""

    # ---- updates
    def check_updates(self):
        """-> manifest (verified) or raises updater.UpdateError. Remembers it."""
        try:
            m = updater.Updater(channel=self.settings["channel"]).fetch_manifest()
            self._refuse_replay(m)
        except updater.UpdateError as e:
            from .api import log_failure
            log_failure("GET", "update-check", e, str(e))          # so a player's launcher.log says why no update was offered
            raise
        self.manifest = m
        return self.manifest

    def _refuse_replay(self, m):
        """A signed manifest older than one this channel already served is a replay (or a deleted release): it would take the shim back."""
        ch = self.settings["channel"]
        seen = dict(self.settings["seen_release"] or {})
        if updater.newer(seen.get(ch, ""), m["tag"]):           # check_updates logs it (update-check call.failed)
            raise updater.UpdateError(f"the release manifest {m['tag']} on the {ch} channel is older than {seen[ch]}, which this launcher already saw")
        if seen.get(ch) != m["tag"]:
            seen[ch] = m["tag"]
            self.settings["seen_release"] = seen
            self.settings.save()

    def wanted_shim(self):
        """The shim version that belongs in the Azimuth folder: what the newest signed manifest offers this launcher (a hotfix can be newer than
        the launcher, or older when one was withdrawn), else what its own release's manifest offers, else the shim this launcher last installed
        from a signed manifest, else its own version."""
        e = updater.shim_entry(self.manifest, __version__) or updater.shim_entry(self.own_manifest, __version__)
        if e:
            return e["version"]
        rec = self.settings["shim_signed"] or {}
        if rec.get("launcher") == __version__ and rec.get("version"):
            return rec["version"]
        return __version__

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
            want = self.wanted_shim()
            if kind != 'ours' or ver != want:
                out['shim'] = f'Install FH2MP {want} for this launcher.'
        need = max((m.get("minLauncher", "0.0.0"), (self.server or {}).get("minLauncher", "0.0.0")), key=updater.vkey)
        out["minLauncherOk"] = updater.at_least(__version__, need)
        return out

    def launcher_outdated(self):
        """This launcher is older than the server (or the signed release manifest) allows. Needs a refresh_server first."""
        need = max(((self.manifest or {}).get("minLauncher", "0.0.0"), (self.server or {}).get("minLauncher", "0.0.0")), key=updater.vkey)
        return not updater.at_least(__version__, need)

    def shim_outdated(self):
        """The shim must be the one this launcher wants (wanted_shim) and meet the server's minimum."""
        kind, ver = self.shim_state()
        if self.shim_pinned(kind, ver):
            return False
        return kind == "ours" and (ver != self.wanted_shim() or not updater.at_least(ver, (self.server or {}).get("minShim", "0.0.0")))

    def auto_update_shim(self):
        """Replaces our own shim in the Azimuth folder with the one this launcher wants (wanted_shim: its release's, or a hotfix's) when it is
        another version, without waiting for Play. -> True when it did. Also downgrades, when a newer signed manifest withdrew a hotfix.
        Prod 2026-10-29: 256 of 530 players refused for an outdated shim ran launcher 1.1.2 from the same address; the game was started
        without Play (directly, or restarted from in-game), so the old shim in the folder was used. Never while the game runs (the DLL is
        loaded), never a pinned shim, and never a file that is not ours: replacing someone else's DLL needs the player's click."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or not self.manifest or self.game_running():
            return False
        kind, ver = self.shim_state()
        if kind != "ours" or self.shim_pinned(kind, ver) or ver == self.wanted_shim():
            return False
        self.install_shim()
        return True

    def shim_pinned(self, kind=None, ver=None):
        """True when the installed shim must be left alone: pinned in settings, or a debug build (version ends in -debug)."""
        if not paths.is_dev():
            return False
        if kind is None:
            kind, ver = self.shim_state()
        return kind == "ours" and (bool(self.settings["pin_shim"]) or (ver or "").endswith("-debug"))

    def install_shim_file(self, path):
        """Developer: installs a shim DLL (or a shim zip from release/build_shim_zip.sh) from disk, e.g. shim/build/FH2MP-debug.dll. Not
        signed, not checked against a manifest: the dev launcher only."""
        if not paths.is_dev():
            raise PlayBlocked("installing a local shim is a dev launcher feature")
        if path.lower().endswith(".zip"):
            data = read_shim_zip(path)
        else:
            with open(path, "rb") as f:
                data = f.read()
        did = shim.install(self.settings["azimuth_dir"], data, platform=self.platform_files())
        self.settings["dev_shim_path"] = path
        self.settings.save()
        return did

    def install_shim(self, progress=None, cancelled=lambda: False):
        up = updater.Updater(channel=self.settings["channel"])
        manifest, e = updater.shim_release(up, self.manifest, __version__)
        if manifest is not self.manifest:
            self.own_manifest = manifest
        zip_path = up.download(e["tag"], e, progress, cancelled)
        dll = read_shim_zip(zip_path)
        if shim.blob_version(dll) != e["version"]:
            raise updater.UpdateError('The FH2MP DLL does not match its signed release.')
        before = self.shim_state()
        did = shim.install(self.settings["azimuth_dir"], dll, e.get("dllSha256"), self.platform_files(progress, cancelled))
        self.remember_shim(e, manifest, before)
        did += self.repair_tls_helper()
        did += self.ensure_extra_options(progress, cancelled)
        did += self.ensure_honk(progress, cancelled)
        did += self.ensure_anna(progress, cancelled)
        did += self.ensure_controls()
        did += self.ensure_anna_bindings()
        return did

    def remember_shim(self, entry, manifest, before):
        """Notes the signed shim just put in place (wanted_shim without a manifest) and logs the change. `before` is shim_state() before it."""
        kind, old = before
        self.settings["shim_signed"] = {"launcher": __version__, "version": entry["version"], "tag": entry["tag"]}
        self.settings.save()
        if kind != "ours" or old != entry["version"]:
            logs.info("shim", "shim.installed", previous=old if kind == "ours" else kind, version=entry["version"], tag=entry["tag"],
                      kind=manifest.get("kind", "release"), launcher=__version__)

    def platform_files(self, progress=None, cancelled=lambda: False):
        """Fetch the supplied platform when any installed loader differs from its pin."""
        if shim.platform_ok(self.settings["azimuth_dir"]):
            return None
        try:
            return upstream.fetch("platform", updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None

    # ---- play
    def _hotfix_result(self, shim_version):
        """-> (hotfix.Result for this setup, mod_hashes of the Azimuth folder), from the server's document or the kept copy."""
        try:
            signed = self.api().hotfix()
        except Exception as e:  # noqa: BLE001 - an older server or no network: the kept copy applies
            logs.swallowed("hotfix", "hotfix.fetch_failed", e)
            signed = None
        doc = hotfix.Store().update(signed)
        mods = hotfix.mod_hashes(self.settings["azimuth_dir"])
        return hotfix.evaluate(doc, __version__, shim_version, mods, env=(self.server or {}).get("env")), mods

    def refresh_hotfix(self):
        """Between Plays: what the hotfix document asks of this setup, for its notice and the status lines. Nothing is turned off until Play."""
        if not self.settings["azimuth_dir"]:
            return self.hotfix
        kind, ver = self.shim_state()
        self.hotfix, _mods = self._hotfix_result(ver if kind == "ours" else "")
        return self.hotfix

    @property
    def hotfix_notice(self):
        """The hotfix document's notice, when one of its rules applies to this setup."""
        h = self.hotfix
        return h.notice if h.rules and isinstance(h.notice, str) else ""

    def apply_hotfix(self, shim_version):
        """The signed hotfix document (hotfix.py): turns off the mods it names, keeps its kill switches for the session file and refuses Play
        when a rule says so. Without an answer from the server the last good copy applies."""
        d = self.settings["azimuth_dir"]
        self.hotfix, mods = self._hotfix_result(shim_version)
        self.hotfix_disabled = hotfix.apply_mods(d, self.hotfix, mods)
        if self.hotfix.rules:
            logs.info("hotfix", "hotfix.applies", serial=self.hotfix.serial, rules=",".join(self.hotfix.rules),
                      switches=",".join(self.hotfix.switches) or None, disabled=",".join(self.hotfix_disabled) or None)
        if self.hotfix.refuse:
            raise PlayBlocked(self.hotfix.refuse, "hotfix")
        return self.hotfix

    def prepare_play(self, require_game=True):
        """Everything up to (not including) starting the process: checks, sign-in, session hand-over. -> (account, notes)."""
        s = self.settings
        if not s["azimuth_dir"]:
            raise PlayBlocked("Choose your Azimuth folder.", "no_azimuth")
        self.refresh_server()
        logs.step("server")
        if self.launcher_outdated():
            raise PlayBlocked(f"This launcher ({__version__}) is out of date and can't connect any more. Update it to keep playing.", "launcher_outdated")
        info = self.azimuth_info()
        if not info.found:
            raise PlayBlocked("azimuth.exe not found.", "no_azimuth")
        if not info.supported:
            raise PlayBlocked(f"This launcher requires Azimuth {AZIMUTH_VERSION}. Run setup to install the supported combination.", "azimuth_unsupported")
        kind, _v = self.shim_state()
        self.apply_hotfix(_v if kind == "ours" else "")
        if kind == "old":
            raise PlayBlocked("The FH2MP shim needs installing again.", "no_shim")
        if kind != "ours":
            raise PlayBlocked("Shim not installed.", "no_shim")
        if self.shim_outdated():
            raise PlayBlocked("The FH2MP shim is out of date and the server would turn it away.", "shim_outdated")
        enabled = shim.enable(s["azimuth_dir"])           # off on the in-game MODS tab: the platform would not load it
        try:
            self.repair_tls_helper()
        except shim.ShimError as e:
            raise PlayBlocked(str(e), "tls_helper") from None
        logs.step("checks")
        try:
            self.ensure_extra_options()          # only when the player has no copy of the supported build
        except Exception as e:  # noqa: BLE001
            from .api import log_failure
            log_failure("PLAY", "extra_options", e, str(e))
            if not self.extra_state()[0]:         # the car database cannot be put in play without it (gamedb.py): say why, now
                raise PlayBlocked(f"FH2 Extra Options {shim.extra_options_version(s['azimuth_dir'])} could not be installed: {e}", "extra_options") from None
        if not self.extra_state()[0]:
            raise PlayBlocked(f"FH2MP needs FH2 Extra Options {shim.extra_options_version(s['azimuth_dir'])} (winmm.dll next to azimuth.exe) and yours is another version. "
                              "Turn on \"Install FH2 Extra Options\" in Settings and press Play: the launcher installs it and keeps your "
                              "old winmm.dll in the fh2extra folder.", "extra_options")
        try:
            self.ensure_honk()
        except Exception as e:  # noqa: BLE001
            from .api import log_failure
            log_failure("PLAY", "honk", e, str(e))
            raise PlayBlocked(f"HonkHonk {shim.honk_version()} could not be installed: {e}. Run setup from Settings to get it.", "honk") from None
        if not self.honk_state()[0]:
            raise PlayBlocked(f"FH2MP needs HonkHonk {shim.honk_version()} (fh2-mods\\HonkHonk.dll and its HonkHonk folder). Run setup from Settings to install it.", "honk")
        try:
            self.ensure_anna()
        except Exception as e:  # noqa: BLE001
            from .api import log_failure
            log_failure("PLAY", "anna", e, str(e))
            raise PlayBlocked(f"FH2 ANNA {shim.anna_version()} could not be installed: {e}. Run setup from Settings to get it.", "anna") from None
        if not self.anna_state()[0]:
            raise PlayBlocked(f"FH2MP needs FH2 ANNA {shim.anna_version()} (fh2-mods\\FH2ANNA.dll). Run setup from Settings to install it.", "anna")
        try:
            self.ensure_anna_bindings()
        except shim.ShimError as e:                  # not worth stopping a game over: ANNA and Extra Options then share the D-pad
            from .api import log_failure
            log_failure("PLAY", "anna_bindings", e, str(e))
        try:
            self.ensure_controls()
        except shim.ShimError as e:                  # not worth stopping a game over: the toggles just stay where they are
            from .api import log_failure
            log_failure("PLAY", "controls", e, str(e))
        try:
            self._aero_note = self.aero_unlock_for_online()
        except shim.ShimError as e:                  # not worth stopping a game over: an aero-unlocked car is then refused by events, with a message
            from .api import log_failure
            log_failure("PLAY", "eo_aero", e, str(e))
            self._aero_note = ""
        logs.step("mods")
        self.ensure_game()
        if require_game and not azimuth.has_game_configured(s["azimuth_dir"]):
            raise PlayBlocked("Couldn't find your Forza Horizon 2 files.", "no_game")
        if (self.server or {}).get("maintenance"):
            raise PlayBlocked(self.server["maintenance"], "maintenance")
        logs.info("auth", "auth.sign_in_needed", why="play")
        acct = self.sign_in()
        logs.step("sign_in")
        adopted = self.prepare_saves(acct["xuid"])
        logs.step("saves")
        cars = self.car_database_step()
        logs.step("car_database")
        r = self._login_reply
        lsp = (r.get("lsp") or {}).get("url") or (self.server.get("lsp") or {}).get("url")
        if not lsp:
            raise PlayBlocked("The server sent no address.", "no_lsp")
        self.game_env = game_port_env(r.get("game") or (self.server or {}).get("game"))
        d = s["azimuth_dir"]
        shim.write_session(d, acct["xuid"], self.token, lsp, gamedb.session_path() if self.gamedb_synced else "", run=logs.run(),
                           switches=self.hotfix.switches)
        shim.write_identity(d, acct["xuid"])
        notes = [cars] if cars else []
        if getattr(self, "_aero_note", ""):
            notes.append(self._aero_note)
        if self.hotfix_disabled:
            notes.append("turned off for a known crash: " + ", ".join(self.hotfix_disabled))
        if enabled:
            notes.append("turned FH2MP back on in the MODS tab")
        tcp_note = self.start_tunnel(r.get("game") or (self.server or {}).get("game"))
        logs.step("tunnel")
        if tcp_note:
            notes.append(tcp_note)
        saves = adopted or "nothing to adopt"
        if adopted:
            notes.append("kept your progress: " + adopted)
        shown = azimuth.ingame_name(acct["gamertag"])       # Azimuth shows "Player 1" for a name it does not accept
        if azimuth.set_player_name(d, shown):
            notes.append(f"set the in-game name to {shown}")
        applied = azimuth.read_ini(d).get("launcher", {}).get("player_name") == shown
        if not applied:
            # Azimuth's own default is "Player 1"; the leaderboards show the local name from here (GitHub issue #2)
            notes.append("could not set the in-game name")
        try:
            pic = self.sync_gamerpic()
        except Exception as e:  # noqa: BLE001 - the picture must never block playing
            from .api import log_failure
            log_failure("PLAY", "gamerpic", e, str(e))
            pic = "picture not updated"
        logs.info("play", "play.prepared", name=acct["gamertag"], ingame=shown, name_written=applied, gamerpic=pic or "unchanged", saves=saves, azimuth_dir=d)
        return acct, notes

    def prepare_saves(self, xuid):
        """A failed migration must never fall through to starting an empty profile."""
        from .saves import SaveCopyError, SaveScanError
        if self.game_running():
            raise PlayBlocked("Close the game before copying saves.", "game_running")
        d = self.settings["azimuth_dir"]
        others = []
        try:
            others = [c for c in azimuth.find_candidates() if os.path.realpath(c) != os.path.realpath(d)]
            return azimuth.adopt_saves(d, xuid, others)
        except SaveCopyError:
            raise
        except OSError as e:
            raise SaveScanError(d, xuid, others, e) from e

    def stop_tunnel(self):
        t, self.tunnel = getattr(self, "tunnel", None), None
        if t is not None:
            t.stop()

    def start_tunnel(self, game):
        """The experimental TCP fallback (Settings: Connect over TCP): a local UDP proxy that carries the game's datagrams to the server's TCP
        carrier. The game is pointed at it with the environment (FH2MP_SERVER_PORT = the proxy's port; FH2MP_TUNNEL makes the server name
        127.0.0.1 as the game server for this client). -> a note for the player, '' when the fallback is off."""
        from .api import log_note
        self.stop_tunnel()
        forced = self.settings["tcp_fallback"]
        tcp = (game or {}).get("tcp") if isinstance(game, dict) else None
        host = (game or {}).get("host") if isinstance(game, dict) else None
        udp = (game or {}).get("port") if isinstance(game, dict) else None
        offered = isinstance(tcp, int) and not isinstance(tcp, bool) and 1 <= tcp <= 65535 and bool(host)
        automatic = False
        if not forced:
            # Automatic: only on a server that says it answers probes (an older one would look like a network that drops UDP), and only
            # when it also offers TCP. Prod 2026-10-29 (#242): a player's race requests never arrived over UDP; TCP was a hidden checkbox.
            advertised = (game or {}).get("probe")
            if not (self.settings["tcp_auto"] and offered and advertised == 1 and not isinstance(advertised, bool)
                    and isinstance(udp, int) and not isinstance(udp, bool) and 1 <= udp <= 65535):
                return ""
            if tunnel.probe_udp(host, udp):
                log_note(f"UDP probe to {host}:{udp} answered: using UDP")
                return ""
            log_note(f"UDP probe to {host}:{udp} got no answer: this network drops UDP, using TCP for this start")
            automatic = True
        if not offered:
            log_note("TCP fallback is on but the server offers none: using UDP")
            return "the server has no TCP connection yet; using UDP"
        port = tunnel.free_udp_port()
        if not port:
            log_note("TCP fallback: no free local port")
            return "no free local port for the TCP connection; using UDP"
        self.tunnel = tunnel.UdpOverTcp(host, tcp, port, log=log_note).start()
        self.game_env = dict(getattr(self, "game_env", None) or {}, FH2MP_TUNNEL="1", FH2MP_SERVER_PORT=str(port))
        log_note(f"TCP fallback on: the game's UDP goes through 127.0.0.1:{port} to {host}:{tcp}")
        return "your network blocks UDP: connecting over TCP" if automatic else "connecting over TCP"

    def car_database_step(self):
        """Every start: the server's car database goes in place, unless the player keeps their own car packs. -> a note for the player.
        Keeping them: the session names no database, so the shim leaves the game's database (and the car packs in it) alone; the server
        lets it play with others, keeps its custom cars out of events and Road Trip and its scores off the boards (lsp gamedbOK, game.CustomCar)."""
        if self.settings["keep_car_packs"]:
            self.gamedb_synced = False
            note = "your own car packs are kept: custom cars stay out of online events and Road Trip, scores stay off the leaderboards"
            return ", ".join(n for n in (note, self.sync_base_parts()) if n)
        return self.sync_gamedb()

    def sync_base_parts(self):
        """Keeping car packs: the server's database is downloaded to fh2mp\\ (not put in place) and its parts and stock cars written to
        fh2mp\\base-parts.txt, from which the shim tells a car pack's parts from the game's (gamedb.write_base_parts). Never blocks Play: the
        game server resets such parts for the others anyway. -> a note for the player, '' when it went through."""
        folder = self.settings["azimuth_dir"]
        try:
            api = self.api()
            m = api.gamedb(self.token)
            if not m:
                return ""
            gamedb.sync(api, self.token, m, folder)
            n = gamedb.write_base_parts(folder)
            standins = gamedb.custom_standins(folder)
            if standins:
                api.put_standins(self.token, standins)
                logs.info("gamedb", "gamedb.standins_sent", cars=standins.count(",") + 1)
        except (gamedb.GameDbError, ApiError, OSError) as e:
            logs.warn("gamedb", "gamedb.base_parts_failed", error=str(e), kept=os.path.isfile(os.path.join(folder, *gamedb.BASE_PARTS)))
            return "couldn't refresh the list of the game's parts: car pack parts may show their own PI in event car select"
        logs.info("gamedb", "gamedb.base_parts_written", lines=n)
        return ""

    def sync_gamedb(self):
        """Every start: the server's car database goes into fh2mp\\ in the Azimuth folder, from where the shim puts it in the game's
        mediaoverride folder (gamedb.py). -> a short note, '' when unchanged. Raises PlayBlocked when it cannot be put in place: the game
        would load another database than everyone else."""
        self.gamedb_synced = False
        try:
            api = self.api()
            m = api.gamedb(self.token)
            if not m:
                return ""
            note = "turned on Extra Options media overrides" if gamedb.ensure_media_overrides(self.settings["azimuth_dir"]) else ""
            synced = gamedb.sync(api, self.token, m, self.settings["azimuth_dir"])
        except gamedb.GameDbError as e:
            raise PlayBlocked(str(e), "gamedb") from None
        except ApiError as e:
            raise PlayBlocked("Couldn't get the car database: " + friendly(e), "gamedb") from None
        except OSError as e:
            raise PlayBlocked(f"Couldn't write the car database: {e.strerror or e}", "gamedb") from None
        self.gamedb_synced = True
        return ", ".join(n for n in (note, synced) if n)

    def sync_gamerpic(self):
        """Every start: the account's picture goes into Azimuth's native or legacy picture file,
        so the in-game picture is the player's own, not the mod's default one. The server always has one (the picture the player uploaded,
        else a generated one). `self.gamerpic_converter` (set by the UI) turns it into the 1080x1080 RGB PNG the game expects; without it
        the server's picture is written as it is. Falls back to the copy kept in the config folder. -> a short note, '' when unchanged."""
        d = self.settings["azimuth_dir"]
        mine = os.path.join(paths.config_dir(), "gamerpic.png")
        png = None
        try:
            raw = self.api().gamerpic(self.account["xuid"])
            png = (self.gamerpic_converter(raw) if self.gamerpic_converter and not azimuth.uses_native_gamerpic(d) else raw) or None
        except Exception as e:  # noqa: BLE001 - offline or a server hiccup: use the kept copy
            from .api import log_failure
            log_failure("GET", f"/v1/gamerpic/{self.account['xuid']}.png", e, str(e))
        if png is None and os.path.isfile(mine):
            with open(mine, "rb") as f:
                png = f.read()
        if not png:
            return "no picture"
        if not azimuth.write_gamerpic(d, png):
            return ""
        return f"picture updated ({len(png)} bytes)"

    def prepare_session_only(self):
        """Developer: sign in and write the session next to the shim without starting the game (start it yourself: rig, Steam, a debugger)."""
        logs.new_run()
        with logs.op("play", "play", mode="session only"):
            return self.prepare_play()

    def online_setup_note(self):
        """The shim's note that this online game run never started its online setup ("Unable to connect to the Live configuration
        servers"), read once -> its text or ''. Diagnosis only: nothing restarts the game (user decision 2026-11-01)."""
        d = self.settings["azimuth_dir"]
        if not (d and self.game_running() and not getattr(self, "offline_run", False) and shim.online_setup_note_present(d)):
            return ""
        text = shim.take_online_setup_note(d)
        if text:
            from .api import log_failure
            log_failure("GAME", "online-setup", None, f"the game did not start its online setup: {text}")
        return text

    def stop_game(self, wait=20):
        """Ends the game the launcher started (its whole process group) and waits for it."""
        p = self.process
        if p is None:
            return
        import subprocess
        try:
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True, timeout=15)
            else:
                import signal
                os.killpg(os.getpgid(p.pid), signal.SIGTERM)
        except (OSError, subprocess.SubprocessError) as e:
            logs.swallowed("game", "game.stop_failed", e, pid=p.pid, how="terminate")
        try:
            p.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            logs.warn("game", "game.stop_slow", pid=p.pid, waited_s=wait, then="kill")
            try:
                if os.name == "nt":
                    p.kill()
                else:
                    import signal
                    os.killpg(os.getpgid(p.pid), signal.SIGKILL)
                p.wait(timeout=10)
            except (OSError, subprocess.SubprocessError) as e:
                logs.swallowed("game", "game.stop_failed", e, pid=p.pid, how="kill")
        left = launch.end_leftover_game(self.settings["azimuth_dir"])
        logs.info("game", "game.stopped", pid=p.pid, code=p.returncode, leftover_processes=left or None)
        self.process = None

    def play(self, azimuth_window=False):
        """azimuth_window: start Azimuth's own launcher window (its settings) instead of going straight into the game."""
        logs.new_run()
        with logs.op("play", "play", offline=self.settings["offline_mode"] or None, azimuth_window=azimuth_window or None):
            if self.settings["azimuth_dir"]:
                shim.take_online_setup_note(self.settings["azimuth_dir"])      # a note left by an earlier game start is not this one's
            if self.settings["offline_mode"]:
                self.play_offline(azimuth_window=azimuth_window)
                self.game_started_at = time.monotonic()
                return None, ["offline mode: online features and progress sync are off"]
            acct, notes = self.prepare_play(require_game=not azimuth_window)
            try:
                self.process = launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=not azimuth_window,
                                            extra_env=getattr(self, "game_env", None))
            except launch.LaunchError:
                launch.cleanup(self.settings["azimuth_dir"])
                raise
            self.game_started_at = time.monotonic()
            logs.step("start")
            return acct, notes

    def note_game_exit(self, code):
        """The game's process ended (by itself, or ended by the memory guard). On Windows a crash shows as an NTSTATUS exit code, which is only
        readable in hex. -> the memory guard's stop ({"grew_mb", "seconds", "free_mb", ...}) when it ended the game, else None."""
        began = getattr(self, "game_started_at", None)
        guard = getattr(getattr(self, "process", None), "memguard", None)
        if not isinstance(guard, (memguard.Guard, memguard.NoGuard)):
            guard = None
        stopped = guard.stopped if guard is not None and isinstance(guard.stopped, dict) else None
        logs.event("INFO" if code in (0, None) else "WARN", "game", "game.exited", code=code, code_hex=f"{code & 0xffffffff:#x}" if code not in (0, None) else None,
                   ran_s=int(time.monotonic() - began) if began else None, offline=getattr(self, "offline_run", False) or None,
                   ended_by="memory guard" if stopped else None)
        if guard is not None:
            try:
                peak = guard.peak()
                if peak is not None:
                    logs.info("game", "game.memory_peak", peak_mb=peak // (1 << 20), cap_mb=(guard.cap or 0) // (1 << 20) or None)
            finally:
                guard.close()
        return stopped

    def play_offline(self, azimuth_window=False):
        """Solo play with the account's local save. The shim only sets the save ID; online hooks stay off."""
        s = self.settings
        d = s["azimuth_dir"]
        if not d or not os.path.isdir(d):
            raise PlayBlocked("Choose your Azimuth folder.", "no_azimuth")
        info = self.azimuth_info()
        if not info.found:
            raise PlayBlocked("azimuth.exe not found.", "no_azimuth")
        if not info.supported:
            raise PlayBlocked(f"This launcher requires Azimuth {AZIMUTH_VERSION}. Run setup to install the supported combination.", "azimuth_unsupported")
        try:
            self.repair_tls_helper()
        except shim.ShimError as e:
            raise PlayBlocked(str(e), "tls_helper") from None
        self.ensure_game()
        if not azimuth_window and not azimuth.has_game_configured(d):
            raise PlayBlocked("Couldn't find your Forza Horizon 2 files.", "no_game")
        xuid = s["xuid"]
        use_identity = bool(xuid or os.path.isfile(os.path.join(shim.bin_dir(d), "fh2mp-identity.txt")))
        if use_identity and (self.shim_state()[0] != "ours" or not shim.supports_offline(d)):
            raise PlayBlocked("Update the FH2MP shim to use your account save offline.", "offline_shim_outdated")
        if xuid:
            self.prepare_saves(xuid)
        self.aero_unlock_restore()                  # offline play gets the player's own Extra Options aero setting
        self.stop_tunnel()
        self.token = ""
        self.account = None
        self.game_env = {"FH2MP_OFFLINE": "1"}
        if xuid:
            self.game_env["FH2MP_XUID"] = f"{xuid:x}"
            shim.write_identity(d, xuid)
        self.gamedb_synced = False
        shim.clear_session(d)
        self._offline_enabled = shim.enable(d) if use_identity else False
        self._offline_disabled = shim.disable(d) if not use_identity else False
        self.offline_run = True
        try:
            self.process = launch.start(s, d, skip_launcher=not azimuth_window, extra_env=self.game_env)
        except Exception:
            self.offline_run = False
            launch.cleanup(d)
            if self._offline_disabled:
                shim.enable(d)
            if self._offline_enabled:
                shim.disable(d)
            raise

    def ensure_game(self):
        """If Azimuth does not know the game folder yet, find the install and write it into Azimuth's settings. -> path set, or ''. """
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or azimuth.has_game_configured(d):
            return ""
        # A different title may be selected in a shared Azimuth installation.
        # Reuse FH2's own configured folder before searching for another copy.
        own = azimuth.fh2_title(azimuth.read_ini(d)).get("folder", "")
        if own:
            azimuth.configure_game(d, own, preserve_content=True)
            return own
        found = azimuth.find_game_installs()
        if not found:
            return ""
        host, win = found[0]
        azimuth.configure_game(d, win)
        return host

    # ---- FH2 Extra Options (optional third-party mod, imported from the player's own copy)
    def extra_state(self):
        """-> (the matching pinned Extra Options is in place, version, target Azimuth)."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or not shim.extra_options_complete(d):
            return False, "", ""
        _i, _v, a = shim.extra_options_installed(d)
        return True, shim.extra_options_version(d), a

    def ensure_extra_options(self, progress=None, cancelled=lambda: False):
        """Install the author's pinned Extra Options release for this Azimuth,
        keeping the previous DLL and existing settings. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not self.settings["extra_options"] or not d or not os.path.isdir(d):
            return []
        shim.migrate_legacy(d)
        if shim.extra_options_complete(d):
            return []
        try:
            files = upstream.fetch(upstream.extra_options_kind(d), updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None
        return shim.install_extra(d, files)

    # ---- HonkHonk (required: the horns other players hear)
    def honk_state(self):
        """-> (the pinned HonkHonk is in place, version)."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or not shim.honk_complete(d):
            return False, ""
        return True, shim.honk_version()

    def ensure_honk(self, progress=None, cancelled=lambda: False):
        """Install the pinned HonkHonk release when the DLL or the horn bank is missing or another build; the player's HonkHonk.ini stays.
        -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or shim.honk_complete(d):
            return []
        try:
            files = upstream.fetch("honkHonk", updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None
        return shim.install_honk(d, files)

    # ---- FH2 ANNA (required: the in-car menu whose lights and doors other players see)
    def anna_state(self):
        """-> (the pinned FH2 ANNA is in place, version)."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or not shim.anna_complete(d):
            return False, ""
        return True, shim.anna_version()

    def ensure_anna(self, progress=None, cancelled=lambda: False):
        """Install the pinned FH2 ANNA release when its DLL is missing or another build. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or shim.anna_complete(d):
            return []
        try:
            files = upstream.fetch("anna", updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None
        return shim.install_anna(d, files)

    def ensure_anna_bindings(self):
        """Extra Options' D-pad autodrive and hazard buttons (ANNA's buttons) go to None once; a binding the player chose is never touched, and a
        player who sets them again afterwards keeps them. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or self.settings["anna_bindings_moved"]:
            return []
        found, did = shim.anna_bindings(d)
        if found:
            self.settings["anna_bindings_moved"] = True
            self.settings.save()
        return did

    def aero_unlock_for_online(self):
        """Extra Options' aero unlock off for an online game (it raises a car's PI, shim.AERO_KEY). The player's value is remembered and comes back
        with aero_unlock_restore(). -> a note for the player when it was turned off for this game, else ''."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d):
            return ""
        was = shim.aero_unlock_off(d)
        if not was:
            return ""
        if not self.settings["eo_aero_restore"]:
            self.settings["eo_aero_restore"] = was
            self.settings.save()
        logs.info("game", "eo.aero_unlock_off", was=was, why="it raises a car's PI, online events refuse such cars")
        return "Extra Options' aero unlock is off for online play (it raises a car's PI); it comes back for offline play"

    def aero_unlock_restore(self):
        """Puts back the aero unlock value aero_unlock_for_online() remembered, when no game runs. -> True when the settings file was changed."""
        value, d = self.settings["eo_aero_restore"], self.settings["azimuth_dir"]
        if not value or not d or not os.path.isdir(d) or self.game_running():
            return False
        try:
            done = shim.aero_unlock_restore(d, value)
        except shim.ShimError as e:
            logs.swallowed("game", "eo.aero_unlock_restore_failed", e, value=value, then="kept for the next try")
            return False
        self.settings["eo_aero_restore"] = ""
        self.settings.save()
        if done:
            logs.info("game", "eo.aero_unlock_restored", value=value)
        return done

    def ensure_controls(self):
        """Extra Options' headlight and roof toggles leave the right stick (the horn) once; a binding the player chose is never touched, and
        a player who sets RightStick again afterwards keeps it. When the last game says the shim could not put the horn on the right stick
        (fh2mp-controls.txt), the toggles this launcher moved go back to the right stick, so horn and toggles never share the left stick; they
        move again after a game that can move the horn. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d):
            return []
        note = shim.read_controls_note(d)
        if note and not note["movable"]:
            if not self.settings["controls_swapped"]:
                return []
            _found, did = shim.restore_stick_bindings(d)
            self.settings["controls_swapped"] = False
            self.settings.save()
            return [f"{line} ({note['why'] or 'the shim could not move it'})" for line in did]
        if self.settings["controls_swapped"]:
            return []
        found, did = shim.swap_stick_bindings(d)
        if found:                                   # no Extra Options settings yet: install_extra writes them already swapped
            self.settings["controls_swapped"] = True
            self.settings.save()
        return did

    def repair_tls_helper(self):
        """An old FH2 Extra Options (0.2.7x) replaced data\\bin\\azimuth-tls.dll; next to 0.2.98 that patches the game twice and 0.2.98
        refuses to start (GitHub #20). Puts Azimuth's own file back. -> list of things done; raises shim.ShimError when it cannot."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d):
            return []
        did = shim.restore_tls_helper(d)
        if did:
            from .api import log_note
            log_note("PLAY " + did[0])
        return did

    def open_azimuth_setup(self):
        """Azimuth's own launcher window (where its game folder is chosen), started the same way the game is."""
        return launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=False)

    def game_running(self):
        return self.process is not None and self.process.poll() is None

    def status_lines(self):
        """The player's setup as (component, what is installed, what this launcher needs, ok: True / False / None for "nothing to judge").
        Everything a support request starts with, in one place: a game that was started without Play, or with an old shim, shows here."""
        s = self.settings
        d = s["azimuth_dir"]
        out = [("FH2MP launcher", __version__, (self.server or {}).get("minLauncher") or "-", not self.launcher_outdated())]
        if not d or not os.path.isdir(d):
            return out + [("Azimuth folder", "not chosen", "choose it in Settings", False)]
        info = self.azimuth_info()
        out.append(("Azimuth", info.version or ("not found" if not info.found else "unknown"), ", ".join(b.get("version", "") for b in BUILTIN_AZIMUTH_BUILDS if b.get("version")) or "-",
                    bool(info.found and info.supported)))
        kind, ver = self.shim_state()
        out.append(("FH2MP shim", ver if kind == "ours" else {"none": "not installed", "old": "needs installing again"}.get(kind, kind),
                    self.wanted_shim() + (f" (server needs {(self.server or {}).get('minShim')})" if (self.server or {}).get("minShim") else ""),
                    kind == "ours" and not self.shim_outdated()))
        pkind, pver = shim.platform_installed(d)
        out.append(("FH2 Mod Platform", pver if pkind == "platform" else {"none": "not installed"}.get(pkind, pkind), PLATFORM_VERSION,
                    pkind == "platform" and pver == PLATFORM_VERSION))
        have, eo_ver, _target = shim.extra_options_installed(d)
        want_eo = shim.extra_options_version(d)
        out.append(("FH2 Extra Options", eo_ver if have else "not installed", want_eo, have and eo_ver == want_eo))
        hh_ok, hh_ver = shim.honk_installed(d)
        out.append(("HonkHonk", hh_ver if hh_ok else "not installed", shim.honk_version(), shim.honk_complete(d)))
        an_ok, an_ver = shim.anna_installed(d)
        out.append(("FH2 ANNA", an_ver if an_ok else "not installed", shim.anna_version(), shim.anna_complete(d)))
        session = os.path.exists(os.path.join(shim.bin_dir(d), shim.SESSION_FILE))
        out.append(("Session for the game", "present (written by Play)" if session else "none: the game starts offline", "Play writes it", None))
        out.append(("Car database", "your own car packs kept: custom cars stay out of online events" if s["keep_car_packs"] else "the server's, put in place at Play", "-", None))
        out.append(("Connection", "TCP always" if s["tcp_fallback"] else "UDP, TCP when the probe fails" if s["tcp_auto"] else "UDP only", "-", None))
        out.append(("Mode", "offline" if s["offline_mode"] else "online", "-", None))
        h = self.hotfix
        if h.rules:
            what = ", ".join(filter(None, [("switches " + ", ".join(h.switches)) if h.switches else "",
                                           ("turned off " + ", ".join(self.hotfix_disabled)) if self.hotfix_disabled else ""]))
            out.append(("Hotfix", f"serial {h.serial}: {', '.join(h.rules)}" + (f" ({what})" if what else ""), "-", None))
        return out

    def clear_leftover_session(self):
        """Removes the session file an earlier Play left in the Azimuth folder when no game of ours is running (called when the launcher starts
        and when it closes). A leftover lets the game go online without Play, on the old shim and the old car database, for as long as its
        token lives (12 hours after its last use); the shim also ignores a file older than 30 minutes. -> True when one was removed."""
        d = self.settings["azimuth_dir"]
        if not d or not os.path.isdir(d) or self.game_running():
            return False
        self.aero_unlock_restore()                  # an online game that ended while the launcher was closed
        before = os.path.exists(os.path.join(shim.bin_dir(d), shim.SESSION_FILE))
        launch.cleanup(d)
        return before

    def after_game(self):
        launch.cleanup(self.settings["azimuth_dir"])
        self.aero_unlock_restore()
        if getattr(self, "offline_run", False):
            self.offline_run = False
            if self._offline_disabled:
                shim.enable(self.settings["azimuth_dir"])
            if self._offline_enabled:
                shim.disable(self.settings["azimuth_dir"])
            return
        if self.token:
            try:
                self.sign_out()
            except ApiError:
                pass
