import os
import sys

from PySide6.QtCore import QEvent, QTimer, QUrl, Qt
from PySide6.QtGui import QDesktopServices, QPixmap
from PySide6.QtWidgets import (QApplication, QFrame, QHBoxLayout, QLabel, QMainWindow, QMessageBox, QProgressBar, QScrollArea,
                               QVBoxLayout, QWidget, QWidgetAction)

from .. import azimuth, crashreport, keystore, launch, paths, saves, shim, updater
from ..api import ApiError
from ..constants import AZIMUTH_PAGE, AZIMUTH_VERSION, GITHUB_REPO
from ..core import Launcher, PlayBlocked, friendly
from ..version import __version__
from . import images, theme
from .dialogs import CrashReportDialog, ProfileDialog, RecoveryDialog, SettingsDialog, offer_save_elevation, open_folder
from . import sfx
from .widgets import (AccountButton, Banner, Brand, Carousel, DetailSheet, GradientText, Hero, Icon, IconButton, LinkButton, OnlineCount, PlaySplit,
                      has_detail, link_colour, menu)
from .wizard import Wizard
from .workers import run_async
from .community import CommunityController, FriendsPanel
from .social_widgets import BadgeIcon


def open_url(u):
    if u:
        QDesktopServices.openUrl(QUrl(u))


class MainWindow(QMainWindow):
    def __init__(self, lc: Launcher):
        super().__init__()
        self.lc = lc
        lc.clear_leftover_session()
        self.setWindowTitle("FH2MP Dev" if paths.is_dev() else "FH2MP")
        self._content_size = (980, 660)       # what the layout needs; a smaller screen scrolls instead of pushing the window off the monitor
        self._apply_screen_limits()
        self._busy = False
        self._msg = None                     # (text, kind) of the last say(); shown in the status box instead of the status
        self._server_state = "checking"      # checking | ok | down
        self._images = {}                    # url -> QPixmap (None while loading or after a failed load)
        self._update_asked = False
        self._wizard = None                # the setup wizard that is open, if any (it runs its own event loop: a second one would open on top of it)
        self._avatar_png = None
        self._launched_at = 0
        self._crash_dialog = False
        self.community = CommunityController(lc, self)
        self._build()
        self.community.changed.connect(self._community_render)
        self.community.lobbies_changed.connect(self._community_render)
        self.community.message.connect(lambda text: self.say(text, theme.WARN))
        link_colour()
        self.poll = QTimer(self)
        self.poll.timeout.connect(self._tick)
        self.poll.start(1000)
        self.status_timer = QTimer(self)
        self.status_timer.timeout.connect(self.refresh_server)
        self.status_timer.start(30_000)

    # ---- layout
    def _build(self):
        root = QWidget()
        self._content = root
        scroll = QScrollArea()                # the window may be smaller than the layout (small screen, Windows display scaling): it scrolls
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setWidget(root)
        self.setCentralWidget(scroll)         # overlays use it as their host: it never scrolls under them
        outer = QVBoxLayout(root)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        top = QFrame()
        top.setObjectName("topbar")
        top.setFixedHeight(72)
        head = QHBoxLayout(top)
        head.setContentsMargins(28, 0, 24, 0)
        head.setSpacing(22)
        head.addWidget(Brand(38))
        if paths.is_dev():
            dev = QLabel("DEV")
            dev.setStyleSheet(f"color: {theme.WARN}; font-weight: 700;")
            head.addWidget(dev)
        head.addStretch(1)
        self.online = OnlineCount()
        head.addWidget(self.online)
        site = self._link("Website", lambda: open_url(self.lc.links["website"]))
        head.addWidget(site)
        self.patreon = self._link("Patreon", lambda: open_url(self.lc.links["patreon"]))
        self.patreon.hide()
        head.addWidget(self.patreon)
        self.account = AccountButton()
        self.account_menu = menu(self)
        self.account_menu.aboutToShow.connect(self._fill_account_menu)
        self.account.set_menu(self.account_menu)
        head.addWidget(self.account)
        self.friends_button = BadgeIcon("friends", 44, 12, 22, label_text="Show or hide Friends")
        self.friends_button.clicked.connect(self._toggle_friends)
        head.addWidget(self.friends_button)
        gear = IconButton("gear", 44, 12, 22, spin=45, label_text="Settings and logs")
        gm = menu(self)
        gm.addAction("Settings", self.open_settings)
        gm.addAction("Logs", self.open_logs)
        gear.set_menu(gm, right=True)
        head.addWidget(gear)
        outer.addWidget(top)

        main = QWidget()
        body = QVBoxLayout(main)
        body.setContentsMargins(28, 24, 28, 0)
        body.setSpacing(0)
        self.hero = Hero()
        hl = QVBoxLayout(self.hero)
        hl.setContentsMargins(36, 36, 36, 32)
        hl.setSpacing(16)
        hl.addWidget(GradientText("Forza Horizon 2 Multiplayer", 54, 520))
        hl.addStretch(1)
        self.motd = Banner()
        hl.addWidget(self.motd)
        self.cards = Carousel()
        self.cards.opened.connect(self._open_card)
        hl.addWidget(self.cards)
        self.sheet = DetailSheet(self.hero)
        self.sheet.open_url.connect(open_url)
        self.sheet.action.connect(lambda a: self.on_play() if a == "play" else None)
        body.addWidget(self.hero, 1)

        bottom = QFrame()
        bottom.setObjectName("bottombar")
        bottom.setMinimumHeight(88)                # a long message may need more
        foot = QHBoxLayout(bottom)
        foot.setContentsMargins(0, 12, 0, 12)
        foot.setSpacing(20)
        self.playsplit = PlaySplit()
        self.play = self.playsplit.play
        self.play.clicked.connect(lambda: self.on_play())
        pm = menu(self)
        pm.addAction("Start with Azimuth settings", lambda: self.on_play(azimuth_window=True))
        off = pm.addAction("Play offline", lambda: self.on_play_offline())
        off.setToolTip("Start the game without FH2MP: no sign-in, no server, no online features, nothing synced")
        pm.setToolTipsVisible(True)
        if paths.is_dev():
            sa = pm.addAction("Sign in only", self.on_session_only)
            sa.setToolTip("Sign in and write the session next to the shim without starting the game (start it yourself: rig, Steam, debugger)")
            pm.setToolTipsVisible(True)
        self.playsplit.set_menu(pm)
        foot.addWidget(self.playsplit)
        self.offline_icon = Icon("cloud_off", theme.MUTED, 22)
        self.offline_icon.setToolTip("Offline mode")
        self.offline_icon.setAccessibleName("Offline mode")
        self.offline_icon.hide()
        foot.addWidget(self.offline_icon, 0, Qt.AlignmentFlag.AlignVCenter)
        self.status = Banner(name="pill")
        self.status.action.connect(lambda: self._status_cb and self._status_cb())
        self._status_cb = None
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setFixedWidth(120)
        self.progress.hide()
        self.status.extra.addWidget(self.progress)
        foot.addWidget(self.status, 0, Qt.AlignmentFlag.AlignVCenter)
        foot.addStretch(1)
        self.channel = QLabel("")
        self.channel.setObjectName("small")
        foot.addWidget(self.channel)
        self.version = QLabel(f"v{__version__}")
        self.version.installEventFilter(self)
        self._taps = 0
        foot.addWidget(self.version)
        body.addWidget(bottom)
        content = QHBoxLayout()
        content.setContentsMargins(0, 0, 0, 0)
        content.setSpacing(0)
        content.addWidget(main, 1)
        self.friends_panel = FriendsPanel(self.community)
        content.addWidget(self.friends_panel)
        outer.addLayout(content, 1)
        root.installEventFilter(self)

    def _toggle_friends(self):
        self.friends_panel.toggle()
        if self.friends_panel.opened:
            self.community.refresh()

    def _community_render(self):
        if not hasattr(self, "cards"):
            return
        cards = self._home_cards()
        if cards != self.cards.cards:
            self.cards.set_cards(cards, self._pix, restart_timer=False)
        n = sum(f['state'] == 'playing' for f in self.community.data.get('friends', []))
        unread = sum(ch.get('unread', 0) for ch in self.community.data.get('chats', []))
        requests = len(self.community.data.get('incoming', []))
        self.friends_button.set_count(unread + requests)
        self.friends_button.setToolTip(f"Friends · {n} playing · {unread} unread messages · {requests} requests")
        self._render_account()

    def _home_cards(self):
        cards = self.lc.home_cards()
        hold = self.community.hold
        text = f"Spot held · {self.community.remaining()}" if hold else "Hold a slot for 5 min"
        lobbies = self.community.lobby_data.get('lobbies')
        live_counts = self.community.data.get('lobbyCounts')
        counts = [f"{live_counts[kind]} active" if live_counts is not None else
                  f"{sum(r['kind'] == kind for r in lobbies)} active" if lobbies is not None else "Browse" for kind in ('freeroam', 'roadtrip')]
        return [{"kind": "lobbies", "tag": "ONLINE LOBBIES", "title": "Reserve a spot", "text": text,
                 "glyph": "|".join(counts), "image": "", "url": "", "link": ""}] + cards

    @staticmethod
    def _link(text, fn):
        b = LinkButton(text)
        b.clicked.connect(fn)
        return b

    def _fill_account_menu(self):
        m, lc = self.account_menu, self.lc
        m.clear()
        if lc.has_identity() and lc.settings["gamertag"]:
            xu = (lc.account or {}).get("xuid") or lc.settings["xuid"]
            if xu:
                action = QWidgetAction(m)
                action.setText(f"ID {xu:#x}")
                row = QWidget()
                layout = QHBoxLayout(row)
                layout.setContentsMargins(14, 4, 8, 4)
                label = QLabel(f"ID {xu:#x}")
                label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
                layout.addWidget(label)
                copy = IconButton("copy", size=30, radius=8, icon_px=18, label_text="Copy account ID")
                copy.setToolTip("Copy account ID")
                copy.clicked.connect(lambda: (QApplication.clipboard().setText(f"{xu:#x}"), copy.setToolTip("Copied")))
                layout.addWidget(copy)
                action.setDefaultWidget(row)
                m.addAction(action)
            m.addAction("Edit profile", self.open_profile)
            m.addAction("Recovery key", self.show_recovery)
        else:
            m.addAction("Set up account", self.run_wizard)
        m.addAction("Run setup wizard", self.run_wizard)

    # ---- rendering
    def _render_account(self):
        lc, s = self.lc, self.lc.settings
        has_acct = bool(s["gamertag"]) and (lc.has_identity() or s["offline_mode"] or getattr(lc, "offline_run", False))
        name = (lc.account or {}).get("gamertag") or s["gamertag"]
        if s["offline_mode"] or getattr(lc, "offline_run", False):
            state = ("Offline", theme.MUTED)
        elif not has_acct:
            state = ("Not signed up", theme.MUTED)
        elif lc.game_running():
            state = ("In game", theme.GOOD)
        else:
            state = ("Online", theme.GOOD) if self._server_state == "ok" else ("Offline", theme.MUTED)
        me = self.community.data.get('me')
        if has_acct and self.community.available() and me and me.get('xuid') == s['xuid'] and me.get('club'):
            state = (f"[{me['club']['tag']}] · {state[0]}", state[1])
        self.account.set(name if has_acct else "Guest", *state)
        self.account.avatar.set_png(self._avatar_png)

    def render(self):
        lc, s = self.lc, self.lc.settings
        self._render_account()
        self.channel.setText("DEV LAUNCHER, channel " + s["channel"] + (f", server {s['api_base']}" if s["api_base"] else "") if paths.is_dev()
                             else ("DEV CHANNEL" if s["channel"] == "dev" else ""))
        self.channel.setStyleSheet(f"color: {theme.WARN};")
        self.patreon.setVisible(bool(lc.links["patreon"]))
        self.online.set_count(None if s["offline_mode"] or getattr(lc, "offline_run", False) else lc.online)
        self.version.setStyleSheet(f"color: {theme.MUTED}; font-size: 13px;")
        m = lc.motd
        if m:
            self.motd.show_message(m, "info")
        else:
            self.motd.hide()
        self.hero.set_picture(self._pix(lc.hero_image))
        self.cards.set_cards(self._home_cards(), self._pix)
        if self.sheet.isVisible():
            self.sheet.place()
        self._render_status()
        self._render_play()

    def _problem(self):
        """The most urgent thing in the way, as (kind, text, button, action), or None when all is well."""
        lc, s = self.lc, self.lc.settings
        up = lc.update_status()
        if not s["offline_mode"] and not up["minLauncherOk"]:
            return "bad", f"Launcher {__version__} is too old.", "Update", self.update_launcher
        if not s["azimuth_dir"]:
            return "bad", "Choose your Azimuth folder.", "Choose...", self.open_settings
        info = lc.azimuth_info()
        if not info.found:
            return "bad", "azimuth.exe not found.", "Change...", self.open_settings
        if not info.supported:
            return "warn", f"This launcher requires Azimuth {AZIMUTH_VERSION} and its supplied mod versions.", "Run setup", self.run_wizard
        if not info.game_folder:
            return "warn", "Azimuth can't find the game files.", "Azimuth setup", self._azimuth_clicked
        if s["offline_mode"]:
            return None
        if not (lc.has_identity() and s["gamertag"]):
            return "warn", "Set up an account to play.", "Set up", self.run_wizard
        kind, ver = lc.shim_state()
        if kind == "none":
            return "bad", "The FH2MP shim is not installed.", "Install", self.install_shim
        if kind == "old":
            return "bad", "The FH2MP shim needs installing again.", "Install", self.install_shim
        if kind == "foreign":
            return "warn", "fh2-mods\\FH2MP.dll is not ours. It will be replaced.", "Install", self.install_shim
        if up["shim"] and lc.manifest:
            return "warn", f"Shim {ver}: update available.", "Update", self.install_shim
        srv = lc.server
        if self._server_state == "checking":
            return "info", "Connecting to the server...", None, None
        if not srv:
            return "warn", "Can't reach the server.", "Retry", self.refresh_all
        if srv.get("maintenance"):
            return "warn", srv["maintenance"], None, None
        if up["launcher"]:
            return "warn", f"Launcher {up['launcher']} is available.", "Update", self.update_launcher
        return None

    def _render_status(self):
        if self._msg:
            text, kind = self._msg
            self._status_cb = None
            self.status.show_message(text, kind)
            return
        pr = self._problem()
        if pr is None and not self.progress.isVisible():
            self._status_cb = None
            self.status.hide()
            return
        kind, text, button, self._status_cb = pr or ("info", "", None, None)
        self.status.show_message(text, kind, button)

    def _open_card(self, c):
        if c.get("kind") == "lobbies":
            self.community.lobbies()
            return
        if has_detail(c):
            self.sheet.show_card(c)
        else:
            open_url(c.get("url"))

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._sync_minimum_height()
        if self.sheet.isVisible():
            self.sheet.place()

    def _apply_screen_limits(self):
        """The window can be as small as the screen's usable area (less a margin for the frame) but never larger: the content scrolls inside."""
        area = self.screen().availableGeometry()
        self.setMinimumSize(min(self._content_size[0], max(480, area.width() - 40)), min(self._content_size[1], max(360, area.height() - 80)))

    def _sync_minimum_height(self):
        layout = self._content.layout()
        height = layout.totalHeightForWidth(max(self._content.width(), self._content_size[0]))
        if height < 0:
            height = layout.minimumSize().height()
        height = max(660, height)
        self._content_size = (980, height)
        self._content.setMinimumSize(980, height)       # the scroll area shows a bar when the window is smaller than this
        self._apply_screen_limits()

    def eventFilter(self, obj, e):
        if obj is self._content and e.type() == QEvent.Type.LayoutRequest:
            self._sync_minimum_height()
        if obj is self.version and e.type() == QEvent.Type.MouseButtonPress:
            self._tap()
        return super().eventFilter(obj, e)

    def _tap(self):
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
            return
        if self._taps >= 7:                         # once a run
            return
        self._taps += 1
        n, api = self._taps, self.lc.api()
        run_async(lambda j: sfx.fetch(api, f"v{n}.wav"), lambda path: path and sfx.play(path), lambda e: None)
        if n == 7:
            theme.use("ALT")
            QApplication.instance().setStyleSheet(theme.QSS)
            link_colour()
            self.account.chev.colour = theme.MUTED
            self.motd._shown = self.status._shown = None
            self.render()
            self.cards._show()
            for w in self.findChildren(QWidget):
                w.update()

    def _render_play(self):
        lc = self.lc
        offline = lc.settings["offline_mode"]
        ready = bool(lc.settings["azimuth_dir"]) and (offline or (lc.has_identity() and bool(lc.settings["gamertag"])))
        running = lc.game_running()
        self.playsplit.set_state("IN GAME" if running else "PLAY", ready and not self._busy and not running)
        self.offline_icon.setVisible(offline or getattr(lc, "offline_run", False))

    def _pix(self, url):
        """The picture at url (hero, news card) once it has been fetched; the first ask starts the download and re-renders after it."""
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False) or not url:
            return None
        if url not in self._images:
            self._images[url] = None

            def ok(data):
                pm = QPixmap()
                if pm.loadFromData(data):
                    self._images[url] = pm
                    self.render()
            run_async(lambda j: self.lc.api().image(url), ok, lambda e: None)
        return self._images[url]

    def say(self, text, colour=None):
        kind = "bad" if colour == theme.BAD else "warn" if colour == theme.WARN else "info"
        self._msg = (text, kind) if text else None
        self._render_status()

    def set_busy(self, busy, text=""):
        self._busy = busy
        if text:
            self.say(text)
        self._render_play()

    # ---- startup and refresh
    def start(self):
        lc = self.lc
        from ..setup import REVISION
        needs_setup = lc.settings["setup_revision"] < REVISION
        if lc.settings["offline_mode"] and not needs_setup:
            lc.ensure_game()
            self.render()
            return
        if not lc.identity_loaded:           # the keyring may be locked and wait for the user's password: show the window first, decide about the wizard after
            self.say("Waiting for your system keyring (unlock it if a prompt appears)...", theme.WARN)

            def loaded(_r):
                self.say("")
                self.start()

            def failed(e):
                lc.identity_loaded = True
                self.say(f"Could not read the keyring: {e}", theme.WARN)
                self.start()
            run_async(lambda j: lc.load_identity(), loaded, failed)
            return
        if not needs_setup:
            lc.ensure_game()
        self.render()
        self.community.refresh()
        if needs_setup or not lc.has_identity() or not lc.settings["azimuth_dir"]:
            QTimer.singleShot(0, self.run_wizard)
        else:
            self.refresh_all()
            self._load_avatar()
            self._check_crash()               # a crash while the launcher was closed (or the game was started from Steam)

    def _check_crash(self):
        """Offer to send a report when Azimuth recorded a crash since the last one was dealt with (see crashreport.check)."""
        lc = self.lc
        if lc.settings["offline_mode"] or lc.game_running() or self._crash_dialog:
            return

        def shown(found):
            crash, report = found
            if not crash or lc.game_running():
                return
            self._crash_dialog = True
            try:
                d = CrashReportDialog(lc, report, self)
                d.exec()
                crashreport.handled(lc.settings, crash)
                if not d.ask_again:
                    lc.settings["crash_report_ask"] = False
                    lc.settings.save()
            finally:
                self._crash_dialog = False

        def find(job):
            crash = crashreport.check(lc.settings)
            return crash, crash and crashreport.build(lc, crash)
        run_async(find, shown, lambda e: None)

    def _open_wizard(self, **kwargs):
        """Runs the setup wizard -> whether it was accepted, or None when one is already open (that one is raised instead). Seen on Windows
        (2026-10-30): start() ran again while the first wizard was installing the add-ons, so a second wizard opened on top of the busy one,
        at the next page, and the two overlapped."""
        if self._wizard is not None:
            self._wizard.raise_()
            self._wizard.activateWindow()
            return None
        self._wizard = Wizard(self.lc, self, **kwargs)
        try:
            return bool(self._wizard.exec())
        finally:
            self._wizard = None

    def run_wizard(self):
        accepted = self._open_wizard()
        if accepted is None:
            return
        if accepted:
            self.lc.ensure_game()
            self.say("")
            self.refresh_all()
            self._load_avatar()
            self.community.refresh()
        self.render()

    def _require_update(self):
        """The server no longer accepts this launcher: the only ways on are to update or to quit."""
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False) or self._update_asked:
            return
        self._update_asked = True
        box = QMessageBox(self)
        box.setWindowTitle("Update required")
        box.setIcon(QMessageBox.Icon.Warning)
        box.setText(f"This FH2MP launcher ({__version__}) is too old to connect.\nUpdate it to keep playing.")
        upd = box.addButton("Update now", QMessageBox.ButtonRole.AcceptRole)
        box.addButton("Quit", QMessageBox.ButtonRole.RejectRole)
        box.exec()
        self._update_asked = False
        if box.clickedButton() is upd:
            self.update_launcher()
        else:
            QApplication.quit()

    def refresh_server(self):
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
            return

        def ok(_c):
            if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
                return
            self._server_state = "ok"
            self.render()
            if self.lc.launcher_outdated():
                QTimer.singleShot(0, self._require_update)
            run_async(lambda j: self.lc.refresh_home(), lambda _h: self.render(), lambda e: None)

        def fail(e):
            if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
                return
            self.lc.server = {}
            self._server_state = "down"
            self.render()
            self.say(friendly(e), theme.WARN)
        run_async(lambda j: self.lc.refresh_server(), ok, fail)

    def refresh_all(self):
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
            return
        self.refresh_server()
        if not self.lc.settings["check_updates"]:
            return

        def fail(e):
            if "no release key" not in str(e):
                self.say(f"Could not check for updates: {friendly(e)}", theme.WARN)
        def check(_job):
            self.lc.check_updates()
            try:
                return self.lc.auto_update_shim()
            except Exception:  # noqa: BLE001 - the update button and Play still offer it, with the reason
                return False
        run_async(check, lambda _m: self.render(), fail)

    def _load_avatar(self):
        if self.lc.settings["offline_mode"] or getattr(self.lc, "offline_run", False):
            return
        xu = self.lc.settings["xuid"]
        if not xu:
            return
        local = os.path.join(paths.config_dir(), "gamerpic.png")

        def fetch(j):
            return self.lc.api().gamerpic(xu)

        def ok(png):
            self._avatar_png = png
            self.render()
        run_async(fetch, ok, lambda e: None)

    # ---- actions
    def open_settings(self):
        d = SettingsDialog(self.lc, self)
        if d.exec():
            self._msg = None
            self.render()
            self.start()

    def _azimuth_clicked(self):
        d = self.lc.settings["azimuth_dir"]
        if d and azimuth.inspect(d, self.lc.builds()).found and not azimuth.has_game_configured(d):
            self.on_play(azimuth_window=True)
            return
        self.open_settings()

    def open_logs(self):
        d = self.lc.settings["azimuth_dir"]
        open_folder(shim.bin_dir(d) if d and os.path.isdir(d) else paths.config_dir())

    def show_recovery(self):
        if self.lc.identity:
            RecoveryDialog(self.lc, self).exec()

    def open_profile(self):
        lc = self.lc
        if lc.game_running() and lc.token:                 # the game holds the session: signing in again would end it
            return self._profile_dialog(False)
        self.set_busy(True, "Signing in...")

        def fail(e):
            self.set_busy(False)
            self.say(friendly(e), theme.BAD)
        run_async(lambda j: lc.sign_in(), lambda _a: (self.set_busy(False), self.say(""), self._profile_dialog(True)), fail)

    def _profile_dialog(self, own_session):
        d = ProfileDialog(self.lc, self.lc.token, self._avatar_png, self)
        d.exec()
        if d.changed:
            self._load_avatar()
        if own_session:
            run_async(lambda j: self.lc.sign_out(), None, lambda e: None)
        self.render()

    def install_shim(self):
        self._shim_job(True)

    def _shim_job(self, announce):
        if self._busy:
            return
        self.set_busy(True, "Installing the FH2MP shim...")
        self.progress.setValue(0)
        self.progress.show()

        def work(job):
            self.lc.check_updates()
            return self.lc.install_shim(lambda got, size: job.progress(int(got * 100 / max(1, size))), lambda: job.cancelled)

        def ok(did):
            self.progress.hide()
            self.set_busy(False, "; ".join(did).capitalize() + ".")
            self.render()

        def fail(e):
            self.progress.hide()
            self.set_busy(False)
            self.say(str(e) if not isinstance(e, ApiError) else friendly(e), theme.BAD)
            self.render()
        run_async(work, ok, fail, progress=self.progress.setValue)

    def update_launcher(self):
        lc = self.lc
        m = lc.manifest
        flavour = updater.launcher_flavour()
        if not m:
            return open_url(f"https://github.com/{GITHUB_REPO}/releases/latest")
        entry = m.get("launcher", {}).get("assets", {}).get(flavour or "")
        if not flavour or not entry:
            return open_url(f"https://github.com/{GITHUB_REPO}/releases/tag/{m['tag']}")
        self.set_busy(True, "Downloading the launcher update...")
        self.progress.setValue(0)
        self.progress.show()

        def work(job):
            up = updater.Updater(channel=lc.settings["channel"])
            path = up.download(m["tag"], entry, lambda got, size: job.progress(int(got * 100 / max(1, size))), lambda: job.cancelled)
            return updater.apply_launcher_update(flavour, path)

        def ok(action):
            self.progress.hide()
            if action == "exit":
                QApplication.quit()
            else:
                self.say("Updated. Restart the launcher.")
                self.set_busy(False)

        def fail(e):
            self.progress.hide()
            self.set_busy(False)
            self.say(str(e), theme.BAD)
        run_async(work, ok, fail, progress=self.progress.setValue)

    # ---- play
    def _save_failure(self, error, retry):
        if not isinstance(error, saves.SaveCopyError):
            return False
        self.say(str(error), theme.BAD)
        if error.permission and offer_save_elevation(self, error):
            self.set_busy(True, "Copying save with administrator rights...")

            def ok(_result):
                self.set_busy(False)
                retry()

            def fail(e):
                self.set_busy(False)
                QMessageBox.warning(self, "Save copy failed", str(e))
                self.say(str(e), theme.BAD)
            run_async(lambda j: saves.retry_elevated(error), ok, fail)
        elif not error.permission:
            QMessageBox.warning(self, "Save copy failed", str(error))
        return True

    def on_play_offline(self, azimuth_window=False):
        lc = self.lc
        if self._busy or lc.game_running():
            return
        self.set_busy(True, "Starting offline...")

        def work(job):
            lc.play_offline(azimuth_window=azimuth_window)

        def ok(_r):
            import time
            self._launched_at = time.time()
            self.set_busy(False, "Game started offline. Nothing is sent to the server.")
            self.render()

        def fail(e):
            self.set_busy(False)
            self.render()
            if self._save_failure(e, lambda: self.on_play_offline(azimuth_window)):
                return
            if isinstance(e, PlayBlocked) and e.code == "azimuth_unsupported":
                return self.say(str(e), theme.BAD)
            self.say(str(e) if isinstance(e, (PlayBlocked, launch.LaunchError)) else friendly(e), theme.BAD)
        run_async(work, ok, fail)

    def on_play(self, azimuth_window=False):
        lc = self.lc
        if lc.settings["offline_mode"]:
            return self.on_play_offline(azimuth_window=azimuth_window)
        if self._busy or lc.game_running():
            return
        self.set_busy(True, "Checking everything...")
        lc.gamerpic_converter = images.azimuth_png_from_bytes

        def work(job):
            if lc.settings["check_updates"]:
                try:
                    lc.check_updates()
                    if lc.update_status()["shim"]:
                        job.progress("Updating the shim...")
                        lc.install_shim()
                except Exception:  # noqa: BLE001 - play with what is installed
                    pass
            job.progress("Signing in...")
            return lc.play(azimuth_window=azimuth_window)

        def ok(res):
            import time
            _acct, notes = res
            self._launched_at = time.time()
            self.set_busy(False, "Game started. Have fun!")
            self.render()

        def fail(e):
            self.set_busy(False)
            self.render()
            if self._save_failure(e, lambda: self.on_play(azimuth_window)):
                return
            if isinstance(e, PlayBlocked):
                if e.code == "no_game":
                    self.say("Couldn't find your Forza Horizon 2 files. Open Azimuth setup and pick the folder.", theme.BAD)
                    return self.render()
                if e.code == "azimuth_unsupported":
                    return self.say(str(e), theme.BAD)
                if e.code in ("no_shim", "shim_outdated"):
                    self.say("Installing the shim...")
                    return self._shim_job(False)
                if e.code == "launcher_outdated":
                    self.say(str(e), theme.BAD)
                    return self._require_update()
                return self.say(str(e), theme.BAD)
            if isinstance(e, ApiError) and e.code == "unknown_account":
                return self._signup_existing()
            if isinstance(e, ApiError) and e.code == "unreachable":
                if QMessageBox.question(self, "Can't reach the server", f"{friendly(e)}\n\nPlay offline instead? Online features are off and "
                                        "nothing is synced.") == QMessageBox.StandardButton.Yes:
                    return self.on_play_offline(azimuth_window=azimuth_window)
                return self.say(friendly(e), theme.BAD)
            if isinstance(e, launch.LaunchError):
                return self.say(str(e), theme.BAD)
            if isinstance(e, ApiError) and e.code == "banned":
                return self.say(f"This account is banned: {e.message}", theme.BAD)
            self.say(friendly(e), theme.BAD)
        run_async(work, ok, fail, progress=lambda t: self.say(t) if isinstance(t, str) else None)

    def on_session_only(self):
        lc = self.lc
        if self._busy:
            return
        self.set_busy(True, "Signing in...")

        def ok(res):
            acct, notes = res
            self.set_busy(False, f"Session written for {acct['gamertag']} ({acct['xuid']:#x}). Start the game yourself; the shim picks it up. "
                                 "Signing in again from here replaces it.")
            self.render()

        def fail(e):
            self.set_busy(False)
            self.say(str(e) if isinstance(e, PlayBlocked) else friendly(e), theme.BAD)
        run_async(lambda j: lc.prepare_session_only(), ok, fail)

    def _signup_existing(self):
        if self._wizard is not None:
            return self._open_wizard()
        QMessageBox.information(self, "New server", "This server doesn't know you yet. Pick a gamertag.")
        if self._open_wizard(existing_key=True):
            self.render()
            self.on_play()

    def _auto_restart(self):
        """The game never started its online setup (the shim says so): it does not recover, so start it again."""
        lc = self.lc
        self.set_busy(True, "The game didn't start its online features. Restarting it...")

        def ok(_res):
            import time
            self._launched_at = time.time()
            self.set_busy(False, "Game restarted. Have fun!")
            self.render()

        def fail(e):
            self.set_busy(False)
            self.render()
            self.say(str(e) if isinstance(e, (PlayBlocked, launch.LaunchError)) else friendly(e), theme.BAD)
        run_async(lambda j: lc.restart_game(), ok, fail)

    def _tick(self):
        lc = self.lc
        if not self._busy and lc.process is not None and lc.restart_requested():
            return self._auto_restart()
        if lc.process is not None and not lc.game_running():
            import time
            code = lc.process.returncode
            lc.process = None
            quick = time.time() - self._launched_at < 25
            run_async(lambda j: lc.after_game(), lambda _r: None, lambda e: None)
            if quick and launch.gpu_check_failure(lc.settings["azimuth_dir"], self._launched_at - 5):
                self.say(launch.gpu_check_advice(lc.settings), theme.BAD)
            elif quick and code not in (0, None):
                self.say(f"The game closed right away (exit code {code}). See Logs.", theme.BAD)
            else:
                self.say("")
            self.render()
            QTimer.singleShot(0, self._check_crash)

    def closeEvent(self, e):
        lc = self.lc
        if lc.game_running():
            if QMessageBox.question(self, "Game is running", "The game keeps running. Close the launcher?") != QMessageBox.StandardButton.Yes:
                e.ignore()
                return
        lc.clear_leftover_session()
        e.accept()
