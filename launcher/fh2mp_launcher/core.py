"""The launcher's logic with no user interface: the UI (and the tests) call these and show the results."""
import os
import platform
import re
import time

from . import azimuth, gamedb, keystore, launch, paths, shim, tunnel, updater, upstream
from .api import Api, ApiError
from .constants import BUILTIN_AZIMUTH_BUILDS, DEFAULT_WEBSITE, EXTRA_OPTIONS_VERSION, MODS_DIR, SHIM_DLL
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


def game_port_env(game):
    """The game server's UDP port as the shim wants it: the game has the port 5000 built in, the shim changes it when FH2MP_SERVER_PORT (exactly 4 digits)
    is set. Without this, a client signed in to another stack (dev) would still talk to production's game server. `game` is the server's {"host", "port"}."""
    port = (game or {}).get("port") if isinstance(game, dict) else None
    if isinstance(port, int) and not isinstance(port, bool) and 1000 <= port <= 9999:
        return {"FH2MP_SERVER_PORT": str(port)}
    return {}


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
        self.account = None             # dict from the API: xuid, gamertag
        self.process = None
        self.manifest = None
        self.gamedb_synced = False      # the server sets a car database and this start put it in place (sync_gamedb)
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
            except Exception:  # noqa: BLE001 - no card is better than an error about a card
                self._extras_at["releases"] = now - 3300           # try again in 5 minutes
        if now - self._extras_at.get("stats", 0) > 300:
            self._extras_at["stats"] = now
            try:
                st = self.api().get_json(self.links["website"].rstrip("/") + "/api/stats/")
                self.stats = st if isinstance(st, dict) and st.get("available") else {}
            except Exception:  # noqa: BLE001
                pass

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
        return list((self.server or {}).get("supportedAzimuth") or []) + BUILTIN_AZIMUTH_BUILDS

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
        try:
            self.manifest = updater.Updater(channel=self.settings["channel"]).fetch_manifest()
        except updater.UpdateError as e:
            from .api import log_failure
            log_failure("GET", "update-check", e, str(e))          # so a player's launcher.log says why no update was offered
            raise
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
        out["minLauncherOk"] = updater.at_least(__version__, need)
        return out

    def launcher_outdated(self):
        """This launcher is older than the server (or the signed release manifest) allows. Needs a refresh_server first."""
        need = max(((self.manifest or {}).get("minLauncher", "0.0.0"), (self.server or {}).get("minLauncher", "0.0.0")), key=updater.vkey)
        return not updater.at_least(__version__, need)

    def shim_outdated(self):
        """The installed shim is older than the server's minShim: it would be refused by the web services. Needs a refresh_server first."""
        kind, ver = self.shim_state()
        return kind == "ours" and not updater.at_least(ver, (self.server or {}).get("minShim", "0.0.0"))

    def shim_pinned(self, kind=None, ver=None):
        """True when the installed shim must be left alone: pinned in settings, or a debug build (version ends in -debug)."""
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
        if not self.manifest:
            self.check_updates()
        e = self.manifest["shim"]
        up = updater.Updater(channel=self.settings["channel"])
        zip_path = up.download(self.manifest["tag"], e, progress, cancelled)
        did = shim.install(self.settings["azimuth_dir"], read_shim_zip(zip_path), e.get("dllSha256"), self.platform_files(progress, cancelled))
        did += self.ensure_extra_options(progress, cancelled)
        return did

    def platform_files(self, progress=None, cancelled=lambda: False):
        """The FH2 Mod Platform from its author's pinned GitHub release when the Azimuth folder has none (or an older one), else None."""
        if shim.platform_ok(self.settings["azimuth_dir"]):
            return None
        try:
            return upstream.fetch("platform", updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None

    # ---- play
    def prepare_play(self, force_unsupported=False, require_game=True):
        """Everything up to (not including) starting the process: checks, sign-in, session hand-over. -> (account, notes)."""
        s = self.settings
        if not s["azimuth_dir"]:
            raise PlayBlocked("Choose your Azimuth folder.", "no_azimuth")
        self.refresh_server()
        if self.launcher_outdated():
            raise PlayBlocked(f"This launcher ({__version__}) is out of date and can't connect any more. Update it to keep playing.", "launcher_outdated")
        info = self.azimuth_info()
        if not info.found:
            raise PlayBlocked("azimuth.exe not found.", "no_azimuth")
        if not info.supported and not force_unsupported:
            raise PlayBlocked(f"Azimuth {info.version or '?'} is untested with FH2MP.", "azimuth_unsupported")
        kind, _v = self.shim_state()
        if kind == "old":
            raise PlayBlocked("The FH2MP shim needs installing again.", "no_shim")
        if kind != "ours":
            raise PlayBlocked("Shim not installed.", "no_shim")
        if self.shim_outdated():
            raise PlayBlocked("The FH2MP shim is out of date and the server would turn it away.", "shim_outdated")
        enabled = shim.enable(s["azimuth_dir"])           # off on the in-game MODS tab: the platform would not load it
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
        adopted = self.prepare_saves(acct["xuid"])
        cars = self.sync_gamedb()
        r = self._login_reply
        lsp = (r.get("lsp") or {}).get("url") or (self.server.get("lsp") or {}).get("url")
        if not lsp:
            raise PlayBlocked("The server sent no address.", "no_lsp")
        self.game_env = game_port_env(r.get("game") or (self.server or {}).get("game"))
        d = s["azimuth_dir"]
        shim.write_session(d, acct["xuid"], self.token, lsp, gamedb.session_path() if self.gamedb_synced else "")
        shim.write_identity(d, acct["xuid"])
        notes = [cars] if cars else []
        if enabled:
            notes.append("turned FH2MP back on in the MODS tab")
        tcp_note = self.start_tunnel(r.get("game") or (self.server or {}).get("game"))
        if tcp_note:
            notes.append(tcp_note)
        saves = adopted or "nothing to adopt"
        if adopted:
            notes.append("kept your progress: " + adopted)
        if azimuth.set_player_name(d, acct["gamertag"]):
            notes.append(f"set the in-game name to {acct['gamertag']}")
        from .api import log_note
        applied = azimuth.read_ini(d).get("launcher", {}).get("player_name") == acct["gamertag"]
        if not applied:
            # Azimuth's own default is "Player 1"; the leaderboards show the local name from here (GitHub issue #2)
            notes.append("could not set the in-game name")
        try:
            pic = self.sync_gamerpic()
        except Exception as e:  # noqa: BLE001 - the picture must never block playing
            from .api import log_failure
            log_failure("PLAY", "gamerpic", e, str(e))
            pic = "picture not updated"
        log_note(f"PLAY name={acct['gamertag']!r} written={'yes' if applied else 'NO'} gamerpic={pic or 'unchanged'} saves={saves!r} azimuth_dir={d}")
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
        if not self.settings["tcp_fallback"]:
            return ""
        tcp = (game or {}).get("tcp") if isinstance(game, dict) else None
        host = (game or {}).get("host") if isinstance(game, dict) else None
        if not (isinstance(tcp, int) and not isinstance(tcp, bool) and 1 <= tcp <= 65535 and host):
            log_note("TCP fallback is on but the server offers none: using UDP")
            return "the server has no TCP connection yet; using UDP"
        port = tunnel.free_udp_port()
        if not port:
            log_note("TCP fallback: no free local port")
            return "no free local port for the TCP connection; using UDP"
        self.tunnel = tunnel.UdpOverTcp(host, tcp, port, log=log_note).start()
        self.game_env = dict(getattr(self, "game_env", None) or {}, FH2MP_TUNNEL="1", FH2MP_SERVER_PORT=str(port))
        log_note(f"TCP fallback on: the game's UDP goes through 127.0.0.1:{port} to {host}:{tcp}")
        return "connecting over TCP"

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
        """Every start: the account's picture from the server goes into the Azimuth folder (Gamerpic.png, read by Azimuth and Extra Options),
        so the in-game picture is the player's own, not the mod's default one. The server always has one (the picture the player uploaded,
        else a generated one). `self.gamerpic_converter` (set by the UI) turns it into the 1080x1080 RGB PNG the game expects; without it
        the server's picture is written as it is. Falls back to the copy kept in the config folder. -> a short note, '' when unchanged."""
        d = self.settings["azimuth_dir"]
        mine = os.path.join(paths.config_dir(), "gamerpic.png")
        png = None
        try:
            raw = self.api().gamerpic(self.account["xuid"])
            png = (self.gamerpic_converter(raw) if self.gamerpic_converter else raw) or None
        except Exception as e:  # noqa: BLE001 - offline or a server hiccup: use the kept copy
            from .api import log_failure
            log_failure("GET", f"/v1/gamerpic/{self.account['xuid']}.png", e, str(e))
        if png is None and os.path.isfile(mine):
            with open(mine, "rb") as f:
                png = f.read()
        if not png:
            return "no picture"
        try:
            with open(os.path.join(d, "Gamerpic.png"), "rb") as f:
                if f.read() == png:
                    return ""
        except OSError:
            pass
        azimuth.write_gamerpic(d, png)
        return f"picture updated ({len(png)} bytes)"

    def prepare_session_only(self, force_unsupported=True):
        """Developer: sign in and write the session next to the shim without starting the game (start it yourself: rig, Steam, a debugger)."""
        return self.prepare_play(force_unsupported)

    def play(self, force_unsupported=False, azimuth_window=False):
        """azimuth_window: start Azimuth's own launcher window (its settings) instead of going straight into the game."""
        if self.settings["offline_mode"]:
            self.play_offline(force_unsupported, azimuth_window=azimuth_window)
            return None, ["offline mode: online features and progress sync are off"]
        acct, notes = self.prepare_play(force_unsupported, require_game=not azimuth_window)
        try:
            self.process = launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=not azimuth_window,
                                        extra_env=getattr(self, "game_env", None))
        except launch.LaunchError:
            launch.cleanup(self.settings["azimuth_dir"])
            raise
        return acct, notes

    def play_offline(self, force_unsupported=False, azimuth_window=False):
        """Solo play with the account's local save. The shim only sets the save ID; online hooks stay off."""
        s = self.settings
        d = s["azimuth_dir"]
        if not d or not os.path.isdir(d):
            raise PlayBlocked("Choose your Azimuth folder.", "no_azimuth")
        info = self.azimuth_info()
        if not info.found:
            raise PlayBlocked("azimuth.exe not found.", "no_azimuth")
        if not info.supported and not force_unsupported:
            raise PlayBlocked(f"Azimuth {info.version or '?'} is untested with FH2MP.", "azimuth_unsupported")
        self.ensure_game()
        if not azimuth_window and not azimuth.has_game_configured(d):
            raise PlayBlocked("Couldn't find your Forza Horizon 2 files.", "no_game")
        xuid = s["xuid"]
        use_identity = bool(xuid or os.path.isfile(os.path.join(shim.bin_dir(d), "fh2mp-identity.txt")))
        if use_identity and (self.shim_state()[0] != "ours" or not shim.supports_offline(d)):
            raise PlayBlocked("Update the FH2MP shim to use your account save offline.", "offline_shim_outdated")
        if xuid:
            self.prepare_saves(xuid)
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

    def ensure_extra_options(self, progress=None, cancelled=lambda: False):
        """The shim's menus (Social, Clubs, Rivals, Marketplace) and several game fixes come from FH2 Extra Options. When the winmm.dll next
        to azimuth.exe is not the supported build (0.2.98, the shim is tested with it), download the author's pinned release from GitHub and
        install it; another winmm.dll there is moved aside into fh2extra. Does nothing when 0.2.98 is there or the player turned this off in
        Settings. -> list of things done."""
        d = self.settings["azimuth_dir"]
        if not self.settings["extra_options"] or not d or not os.path.isdir(d):
            return []
        shim.migrate_legacy(d)
        if self.extra_state()[0]:
            return []
        try:
            files = upstream.fetch("extraOptions", updater.Updater(channel=self.settings["channel"]), progress, cancelled)
        except upstream.UpstreamError as e:
            raise updater.UpdateError(str(e)) from None
        return shim.install_extra(d, files)

    def open_azimuth_setup(self):
        """Azimuth's own launcher window (where its game folder is chosen), started the same way the game is."""
        return launch.start(self.settings, self.settings["azimuth_dir"], skip_launcher=False)

    def game_running(self):
        return self.process is not None and self.process.poll() is None

    def after_game(self):
        launch.cleanup(self.settings["azimuth_dir"])
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
