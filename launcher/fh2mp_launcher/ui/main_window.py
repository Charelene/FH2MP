import os
import sys

from PySide6.QtCore import QTimer, QUrl, Qt
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (QApplication, QHBoxLayout, QLabel, QMainWindow, QMessageBox, QProgressBar, QPushButton, QVBoxLayout,
                               QWidget)

from .. import azimuth, keystore, launch, paths, shim, updater
from ..api import ApiError
from ..constants import AZIMUTH_PAGE, GITHUB_REPO
from ..core import Launcher, PlayBlocked, friendly
from ..version import __version__
from . import images, theme
from .dialogs import ProfileDialog, RecoveryDialog, SettingsDialog, open_folder
from .widgets import Avatar, Banner, Brand, Card, StatusRow, hrule
from .wizard import Wizard
from .workers import run_async


def open_url(u):
    if u:
        QDesktopServices.openUrl(QUrl(u))


class MainWindow(QMainWindow):
    def __init__(self, lc: Launcher):
        super().__init__()
        self.lc = lc
        self.setWindowTitle("FH2MP Dev" if paths.is_dev() else "FH2MP")
        self.setMinimumSize(760, 540)
        self._busy = False
        self._avatar_png = None
        self._launched_at = 0
        self._build()
        self.poll = QTimer(self)
        self.poll.timeout.connect(self._tick)
        self.poll.start(1000)
        self.status_timer = QTimer(self)
        self.status_timer.timeout.connect(self.refresh_server)
        self.status_timer.start(30_000)

    # ---- layout
    def _build(self):
        root = QWidget()
        self.setCentralWidget(root)
        outer = QVBoxLayout(root)
        outer.setContentsMargins(28, 22, 28, 18)
        outer.setSpacing(14)

        head = QHBoxLayout()
        head.addWidget(Brand(36))
        if paths.is_dev():
            dev = QLabel("DEV")
            dev.setStyleSheet(f"color: {theme.WARN}; font-weight: 700;")
            head.addWidget(dev, 0, Qt.AlignmentFlag.AlignBottom)
        head.addStretch(1)
        self.online = QLabel("")
        self.online.setObjectName("muted")
        head.addWidget(self.online)
        self.patreon = QPushButton("Support on Patreon")
        self.patreon.setObjectName("patreon")
        self.patreon.clicked.connect(lambda: open_url(self.lc.links["patreon"]))
        self.patreon.hide()
        head.addWidget(self.patreon)
        site = QPushButton("Website")
        site.setObjectName("link")
        site.clicked.connect(lambda: open_url(self.lc.links["website"]))
        head.addWidget(site)
        outer.addLayout(head)

        self.banner = Banner()
        self.banner.action.connect(self._banner_action)
        self._banner_kind = ""
        self._update_asked = False
        outer.addWidget(self.banner)
        self.motd = Banner()
        outer.addWidget(self.motd)

        body = QHBoxLayout()
        body.setSpacing(16)
        # profile
        pc = Card()
        pc.setFixedWidth(250)
        self.avatar = Avatar(120)
        pc.lay.addWidget(self.avatar, 0, Qt.AlignmentFlag.AlignHCenter)
        self.gt = QLabel("")
        self.gt.setObjectName("h2")
        self.gt.setAlignment(Qt.AlignmentFlag.AlignCenter)
        pc.lay.addWidget(self.gt)
        self.xuid = QLabel("")
        self.xuid.setObjectName("small")
        self.xuid.setAlignment(Qt.AlignmentFlag.AlignCenter)
        pc.lay.addWidget(self.xuid)
        pc.lay.addSpacing(6)
        self.btn_profile = QPushButton("Edit profile")
        self.btn_profile.clicked.connect(self.open_profile)
        pc.lay.addWidget(self.btn_profile)
        self.btn_setup = QPushButton("Set up account")
        self.btn_setup.setObjectName("primary")
        self.btn_setup.clicked.connect(self.run_wizard)
        pc.lay.addWidget(self.btn_setup)
        pc.lay.addStretch(1)
        body.addWidget(pc)

        # status
        sc = Card()
        t = QLabel("Status")
        t.setObjectName("h2")
        sc.lay.addWidget(t)
        self.row_az = StatusRow("Azimuth")
        self.row_az.clicked.connect(self._azimuth_clicked)
        self.row_shim = StatusRow("FH2MP shim")
        self.row_shim.clicked.connect(self.install_shim)
        self.row_srv = StatusRow("Server")
        self.row_srv.clicked.connect(self.refresh_all)
        self.row_app = StatusRow("Launcher")
        self.row_app.clicked.connect(self.update_launcher)
        for i, r in enumerate((self.row_az, self.row_shim, self.row_srv, self.row_app)):
            box = QWidget()
            box.setObjectName("rowbox")
            box.setStyleSheet("#rowbox { background: transparent; }")
            bl = QVBoxLayout(box)
            bl.setContentsMargins(0, 0, 0, 0)
            bl.setSpacing(0)
            if i:
                bl.addWidget(hrule())
            bl.addWidget(r)
            r._box = box
            sc.lay.addWidget(box)
        sc.lay.addStretch(1)
        body.addWidget(sc, 1)
        outer.addLayout(body, 1)

        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.hide()
        outer.addWidget(self.progress)
        self.message = QLabel("")
        self.message.setObjectName("muted")
        self.message.setWordWrap(True)
        outer.addWidget(self.message)

        foot = QHBoxLayout()
        self.version = QLabel(f"v{__version__}")
        self.version.setObjectName("small")
        foot.addWidget(self.version)
        self.channel = QLabel("")
        self.channel.setObjectName("small")
        foot.addWidget(self.channel)
        foot.addStretch(1)
        logs = QPushButton("Logs")
        logs.setObjectName("link")
        logs.clicked.connect(self.open_logs)
        foot.addWidget(logs)
        st = QPushButton("Settings")
        st.setObjectName("link")
        st.clicked.connect(self.open_settings)
        foot.addWidget(st)
        az = QPushButton("Azimuth settings")
        az.setObjectName("link")
        az.clicked.connect(lambda: self.on_play(azimuth_window=True))
        foot.addWidget(az)
        if paths.is_dev():
            self.session_btn = QPushButton("Sign in only")
            self.session_btn.setToolTip("Sign in and write the session next to the shim without starting the game (start it yourself: rig, Steam, debugger)")
            self.session_btn.clicked.connect(self.on_session_only)
            foot.addWidget(self.session_btn)
        self.play = QPushButton("PLAY")
        self.play.setObjectName("play")
        self.play.clicked.connect(lambda: self.on_play())
        foot.addWidget(self.play)
        outer.addLayout(foot)

    # ---- rendering
    def render(self):
        lc, s = self.lc, self.lc.settings
        has_acct = lc.has_identity() and bool(s["gamertag"])
        name = (lc.account or {}).get("gamertag") or s["gamertag"]
        self.gt.setText(name if has_acct else "Not signed up")
        self.avatar.set_name(name)
        self.avatar.set_png(self._avatar_png)
        xu = (lc.account or {}).get("xuid") or s["xuid"]
        self.xuid.setText(f"ID {xu:#x}" if has_acct and xu else "")
        self.btn_profile.setVisible(has_acct)
        self.btn_setup.setVisible(not has_acct)
        self.channel.setText("  ·  DEV LAUNCHER, channel " + s["channel"] + (f", server {s['api_base']}" if s["api_base"] else "") if paths.is_dev()
                             else ("  ·  DEV CHANNEL" if s["channel"] == "dev" else ""))
        self.channel.setStyleSheet(f"color: {theme.WARN};")
        l = lc.links
        self.patreon.setVisible(bool(l["patreon"]))
        n = lc.online
        self.online.setText(f"● {n} online" if isinstance(n, int) else "")
        self.online.setStyleSheet(f"color: {theme.GOOD};" if isinstance(n, int) else "")

        info = lc.azimuth_info()
        if not s["azimuth_dir"]:
            self.row_az.set("bad", "No folder chosen.", "Choose...")
        elif not info.found:
            self.row_az.set("bad", "azimuth.exe not found.", "Change...")
        elif not info.game_folder:
            self.row_az.set("warn", "Game files not found.", "Azimuth setup")
        elif info.supported:
            self.row_az.set("good", f"Version {info.version or info.supported_version}", "Change...")
        else:
            self.row_az.set("warn", f"Version {info.version or '?'} is untested. Use {lc.builds()[0]['version']}.", "Change...")
        kind, ver = lc.shim_state()
        up = lc.update_status()
        if not s["azimuth_dir"] or not info.found:
            self.row_shim.set("idle", "Needs Azimuth.")
        elif kind == "none":
            self.row_shim.set("bad", "Not installed.", "Install")
        elif kind == "foreign":
            self.row_shim.set("warn", "Other winmm.dll found. It will be kept.", "Install")
        elif lc.shim_pinned(kind, ver):
            self.row_shim.set("good", f"Version {ver} (pinned)", "Install release" if lc.manifest else None)
        elif up["shim"] and lc.manifest:
            self.row_shim.set("warn", f"Version {ver}, update available.", "Update")
        else:
            self.row_shim.set("good", "")
        srv = lc.server
        if not srv:
            self.row_srv.set("warn", "Checking...", "Retry")
        elif srv.get("maintenance"):
            self.row_srv.set("warn", srv["maintenance"])
        else:
            reg = {"invite": " · invite only", "closed": " · registration closed"}.get(srv.get("registration"), "")
            self.row_srv.set("good", f"Online{reg}")
        if up["launcher"]:
            self.row_app.set("warn", f"Version {__version__}, {up['launcher']} available.", "Update")
        elif not up["minLauncherOk"]:
            self.row_app.set("bad", f"Version {__version__} is too old.", "Update")
        else:
            self.row_app.set("good", "")
        self.row_shim._box.setVisible(not (kind == "ours" and not up["shim"]) or (paths.is_dev() and kind == "ours"))
        self.row_app._box.setVisible(bool(up["launcher"]) or not up["minLauncherOk"])
        m = (srv or {}).get("motd", "")
        if m:
            self.motd.show_message(m, "info")
        else:
            self.motd.hide()
        self._render_play()

    def _render_play(self):
        lc = self.lc
        ready = bool(lc.settings["azimuth_dir"]) and lc.has_identity() and bool(lc.settings["gamertag"])
        running = lc.game_running()
        self.play.setText("IN GAME" if running else "PLAY")
        self.play.setEnabled(ready and not self._busy and not running)

    def say(self, text, colour=None):
        self.message.setText(text)
        self.message.setStyleSheet(f"color: {colour};" if colour else "")

    def set_busy(self, busy, text=""):
        self._busy = busy
        if text:
            self.say(text)
        self._render_play()

    # ---- startup and refresh
    def start(self):
        lc = self.lc
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
        lc.ensure_game()
        self.render()
        if not lc.has_identity() or not lc.settings["azimuth_dir"]:
            QTimer.singleShot(0, self.run_wizard)
        else:
            self.refresh_all()
            self._load_avatar()

    def run_wizard(self):
        w = Wizard(self.lc, self)
        if w.exec():
            self.lc.ensure_game()
            self.say("")
            self.refresh_all()
            self._load_avatar()
        self.render()

    def _require_update(self):
        """The server no longer accepts this launcher: the only ways on are to update or to quit."""
        if self._update_asked:
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
        def ok(_c):
            self.render()
            if self.lc.launcher_outdated():
                QTimer.singleShot(0, self._require_update)

        def fail(e):
            self.lc.server = {}
            self.render()
            self.say(friendly(e), theme.WARN)
        run_async(lambda j: self.lc.refresh_server(), ok, fail)

    def refresh_all(self):
        self.refresh_server()
        if not self.lc.settings["check_updates"]:
            return

        def ok(_m):
            self.render()
            up = self.lc.update_status()
            if up["launcher"] or not up["minLauncherOk"]:
                self._banner_kind = "launcher"
                self.banner.show_message(f"FH2MP launcher {up['launcher'] or 'update'} is available.", "warn" if up["minLauncherOk"] else "bad",
                                         "Update now")

        def fail(e):
            if "no release key" not in str(e):
                self.say(f"Could not check for updates: {friendly(e)}", theme.WARN)
        run_async(lambda j: self.lc.check_updates(), ok, fail)

    def _load_avatar(self):
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
            self.render()
            self.refresh_all()

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

    def _banner_action(self):
        if self._banner_kind == "launcher":
            self.update_launcher()

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
    def on_play(self, force=False, azimuth_window=False):
        lc = self.lc
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
            return lc.play(force_unsupported=force, azimuth_window=azimuth_window)

        def ok(res):
            import time
            _acct, notes = res
            self._launched_at = time.time()
            self.set_busy(False, "Game started. Have fun!")
            self.render()

        def fail(e):
            self.set_busy(False)
            self.render()
            if isinstance(e, PlayBlocked):
                if e.code == "no_game":
                    self.say("Couldn't find your Forza Horizon 2 files. Open Azimuth setup and pick the folder.", theme.BAD)
                    return self.render()
                if e.code == "azimuth_unsupported":
                    if QMessageBox.question(self, "Untested Azimuth", f"{e}\nStart anyway?") == QMessageBox.StandardButton.Yes:
                        return self.on_play(force=True, azimuth_window=azimuth_window)
                    return self.say("Cancelled.")
                if e.code in ("no_shim", "shim_outdated"):
                    self.say("Installing the shim...")
                    return self._shim_job(False)
                if e.code == "launcher_outdated":
                    self.say(str(e), theme.BAD)
                    return self._require_update()
                return self.say(str(e), theme.BAD)
            if isinstance(e, ApiError) and e.code == "unknown_account":
                return self._signup_existing()
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
        QMessageBox.information(self, "New server", "This server doesn't know you yet. Pick a gamertag.")
        w = Wizard(self.lc, self, existing_key=True)
        if w.exec():
            self.render()
            self.on_play()

    def _tick(self):
        lc = self.lc
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

    def closeEvent(self, e):
        lc = self.lc
        if lc.game_running():
            if QMessageBox.question(self, "Game is running", "The game keeps running. Close the launcher?") != QMessageBox.StandardButton.Yes:
                e.ignore()
                return
        e.accept()
