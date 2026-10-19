"""First-run setup: find Azimuth, make (or restore) the account, keep the recovery code."""
import os

from PySide6.QtCore import Qt
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtCore import QUrl
from PySide6.QtWidgets import (QButtonGroup, QCheckBox, QDialog, QFileDialog, QHBoxLayout, QLabel, QLineEdit, QListWidget, QProgressBar,
                               QPushButton, QRadioButton, QStackedWidget, QVBoxLayout, QWidget)

from .. import azimuth, identity
from ..api import ApiError
from ..constants import AZIMUTH_PAGE
from ..core import friendly
from . import theme
from .dialogs import NameCheck
from .widgets import Banner
from .workers import run_async


class Wizard(QDialog):
    WELCOME, AZIMUTH, ACCOUNT, WORKING, RECOVERY = range(5)

    def __init__(self, lc, parent=None, existing_key=False):
        """existing_key: this computer already has a key but the server has no account for it: only ask for a gamertag."""
        super().__init__(parent)
        self.lc = lc
        self.existing_key = existing_key
        self.setWindowTitle("FH2MP")
        self.setWindowFlags(Qt.WindowType.Dialog | Qt.WindowType.CustomizeWindowHint | Qt.WindowType.WindowTitleHint | Qt.WindowType.WindowCloseButtonHint)
        self.setFixedSize(620, 500)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        self.stack = QStackedWidget()
        outer.addWidget(self.stack)
        self.stack.addWidget(self._welcome())
        self.stack.addWidget(self._azimuth())
        self.stack.addWidget(self._account())
        self.stack.addWidget(self._working())
        self.stack.addWidget(self._recovery())
        self._job = None
        self.restored = False
        if existing_key:
            self.mode_restore.hide()
            self.restore_box.hide()
            self.mode_new.hide()
            self.stack.setCurrentIndex(self.ACCOUNT)
            self.acct_back.hide()

    # ---- pages
    def _page(self, title, sub=None):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(40, 34, 40, 28)
        lay.setSpacing(12)
        t = QLabel(title)
        t.setObjectName("title")
        lay.addWidget(t)
        if sub:
            s = QLabel(sub)
            s.setObjectName("muted")
            s.setWordWrap(True)
            lay.addWidget(s)
        return w, lay

    def _nav(self, lay, back=None, nxt=None, next_text="Next"):
        row = QHBoxLayout()
        row.addStretch(1)
        b = None
        if back is not None:
            b = QPushButton("Back")
            b.clicked.connect(lambda: self.stack.setCurrentIndex(back))
            row.addWidget(b)
        n = QPushButton(next_text)
        n.setObjectName("primary")
        if nxt:
            n.clicked.connect(nxt)
        row.addWidget(n)
        lay.addLayout(row)
        return b, n

    def _welcome(self):
        w, lay = self._page("Welcome to FH2MP", "Forza Horizon 2 online. No e-mail, no password: your account is a key on this computer, "
                                                 "with a recovery code to move it.")
        lay.addStretch(1)
        self._nav(lay, None, lambda: self._goto_azimuth(), "Get started")
        return w

    def _azimuth(self):
        w, lay = self._page("Azimuth", "Pick the folder with azimuth.exe.")
        self.az_list = QListWidget()
        self.az_list.setMaximumHeight(130)
        self.az_list.itemSelectionChanged.connect(self._az_selected)
        lay.addWidget(self.az_list)
        row = QHBoxLayout()
        br = QPushButton("Browse...")
        br.clicked.connect(self._az_browse)
        dl = QPushButton("Get Azimuth")
        dl.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(AZIMUTH_PAGE)))
        row.addWidget(br)
        row.addWidget(dl)
        row.addStretch(1)
        lay.addLayout(row)
        self.az_banner = Banner()
        lay.addWidget(self.az_banner)
        lay.addStretch(1)
        _b, self.az_next = self._nav(lay, self.WELCOME, lambda: self.stack.setCurrentIndex(self.ACCOUNT))
        self.az_next.setEnabled(False)
        self.az_path = ""
        return w

    def _account(self):
        w, lay = self._page("Account")
        self.mode_new = QRadioButton("Create a new account")
        self.mode_restore = QRadioButton("I have a recovery code")
        self.mode_new.setChecked(True)
        grp = QButtonGroup(self)
        grp.addButton(self.mode_new)
        grp.addButton(self.mode_restore)
        lay.addWidget(self.mode_new)

        self.new_box = QWidget()
        nb = QVBoxLayout(self.new_box)
        nb.setContentsMargins(26, 0, 0, 0)
        nb.addWidget(QLabel("Gamertag"))
        self.tag = QLineEdit()
        self.tag.setMaxLength(15)
        self.tag.setPlaceholderText("Gamertag")
        nb.addWidget(self.tag)
        self.tag_hint = QLabel("")
        self.tag_hint.setObjectName("small")
        nb.addWidget(self.tag_hint)
        self.invite = QLineEdit()
        self.invite.setPlaceholderText("Invite code")
        self.invite.hide()
        nb.addWidget(self.invite)
        lay.addWidget(self.new_box)

        lay.addWidget(self.mode_restore)
        self.restore_box = QWidget()
        rb = QVBoxLayout(self.restore_box)
        rb.setContentsMargins(26, 0, 0, 0)
        self.code = QLineEdit()
        self.code.setPlaceholderText("FH2R1-XXXXX-XXXXX-...")
        rb.addWidget(self.code)
        lay.addWidget(self.restore_box)
        self.restore_box.setEnabled(False)

        self.acct_banner = Banner()
        lay.addWidget(self.acct_banner)
        lay.addStretch(1)
        self.tagcheck = NameCheck(self.tag, self.tag_hint, self.lc, on_state=lambda _ok: self._acct_valid())
        self.mode_new.toggled.connect(self._mode_changed)
        self.code.textChanged.connect(lambda _t: self._acct_valid())
        self.acct_back, self.acct_next = self._nav(lay, self.AZIMUTH, self._start_account, "Continue")
        self.acct_next.setEnabled(False)
        return w

    def _working(self):
        w, lay = self._page("Setting things up")
        lay.addStretch(1)
        self.work_label = QLabel("")
        self.work_label.setObjectName("muted")
        self.work_label.setWordWrap(True)
        lay.addWidget(self.work_label)
        bar = QProgressBar()
        bar.setRange(0, 0)
        lay.addWidget(bar)
        self.work_error = Banner()
        lay.addWidget(self.work_error)
        self.work_back = QPushButton("Back")
        self.work_back.clicked.connect(lambda: self.stack.setCurrentIndex(self.ACCOUNT))
        self.work_back.hide()
        lay.addWidget(self.work_back, 0, Qt.AlignmentFlag.AlignRight)
        lay.addStretch(2)
        return w

    def _recovery(self):
        w, lay = self._page("Recovery code", "The only way back into your account. Keep it private.")
        self.rec_code = QLabel("")
        self.rec_code.setObjectName("code")
        self.rec_code.setWordWrap(True)
        self.rec_code.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        lay.addWidget(self.rec_code)
        row = QHBoxLayout()
        cp = QPushButton("Copy")
        cp.clicked.connect(lambda: (QGuiApplication.clipboard().setText(self.rec_code.text()), cp.setText("Copied")))
        sv = QPushButton("Save to a file...")
        sv.clicked.connect(self._save_code)
        row.addWidget(cp)
        row.addWidget(sv)
        row.addStretch(1)
        lay.addLayout(row)
        self.ack = QCheckBox("I saved it")
        lay.addWidget(self.ack)
        lay.addStretch(1)
        _b, self.finish = self._nav(lay, None, self.accept, "Finish")
        self.finish.setEnabled(False)
        self.ack.toggled.connect(self.finish.setEnabled)
        return w

    # ---- behaviour
    def _goto_azimuth(self):
        self.stack.setCurrentIndex(self.AZIMUTH)
        self.az_list.clear()
        cur = self.lc.settings["azimuth_dir"]
        cands = ([cur] if cur else []) + [c for c in azimuth.find_candidates() if c != cur]
        cands.sort(key=lambda c: not azimuth.inspect(c, self.lc.builds()).supported)          # supported builds first
        for c in cands:
            self.az_list.addItem(c)
        if cands:
            self.az_list.setCurrentRow(0)
        else:
            self.az_banner.show_message("Azimuth not found. Use Browse.", "warn")
        # the server tells us which builds are supported; fetch it quietly for a better answer
        run_async(lambda j: self.lc.refresh_server(), lambda _r: self._az_selected(), lambda _e: None)

    def _az_browse(self):
        d = QFileDialog.getExistingDirectory(self, "Select the folder that contains azimuth.exe", os.path.expanduser("~"))
        if d:
            self.az_list.addItem(d)
            self.az_list.setCurrentRow(self.az_list.count() - 1)

    def _az_selected(self):
        it = self.az_list.currentItem()
        if not it:
            return
        info = azimuth.inspect(it.text(), self.lc.builds())
        self.az_path = it.text() if info.found else ""
        if not info.found:
            self.az_banner.show_message("No azimuth.exe in that folder.", "bad")
        elif info.supported:
            self.az_banner.show_message(f"Azimuth {info.version}" + (" · game found" if azimuth.find_game_installs() or info.game_folder else " · game files not found yet"), "info")
        else:
            self.az_banner.show_message(f"Azimuth {info.version or '?'} is untested. 0.9.10 is supported.", "warn")
        self.az_next.setEnabled(info.found)

    def _mode_changed(self, new):
        self.new_box.setEnabled(new)
        self.restore_box.setEnabled(not new)
        self._acct_valid()

    def _acct_valid(self):
        if self.mode_new.isChecked():
            self.acct_next.setEnabled(self.tagcheck.ok)
        else:
            self.acct_next.setEnabled(len(self.code.text().strip()) >= 20)

    def _start_account(self):
        lc = self.lc
        lc.settings["azimuth_dir"] = self.az_path
        lc.settings.save()
        self.stack.setCurrentIndex(self.WORKING)
        self.work_error.hide()
        self.work_back.hide()
        new = self.mode_new.isChecked()
        tag, invite, code = self.tag.text().strip(), self.invite.text().strip(), self.code.text().strip()
        self.work_label.setText("Creating account..." if new else "Restoring...")

        def work(job):
            if new:
                if not self.existing_key:
                    lc.create_identity()
                try:
                    return lc.sign_up(tag, invite, progress=lambda n: job.progress(n), cancelled=lambda: job.cancelled)
                except BaseException:
                    if not self.existing_key:
                        lc.forget_identity()              # do not keep a key that has no account behind it
                    raise
            lc.restore_identity(code)
            try:
                return lc.sign_in()
            except ApiError:
                lc.forget_identity()
                raise

        def ok(_acct):
            self.restored = not new
            if new and self.existing_key:
                self.accept()
            elif new:
                self.rec_code.setText(lc.identity.recovery_code())
                self.stack.setCurrentIndex(self.RECOVERY)
            else:
                self.accept()

        def failed(e):
            if isinstance(e, identity.RecoveryError):
                msg = str(e)
            elif isinstance(e, ApiError) and e.code == "unknown_account":
                msg = "No account for that code."
            else:
                msg = friendly(e)
            if isinstance(e, ApiError) and e.code == "invite_required":
                self.invite.show()
            self.work_error.show_message(msg, "bad")
            self.work_back.show()
        self._job = run_async(work, ok, failed, progress=lambda n: self.work_label.setText(
            "Creating account..."))

    def _save_code(self):
        p, _ = QFileDialog.getSaveFileName(self, "Save recovery code", os.path.expanduser("~/fh2mp-recovery-code.txt"), "Text (*.txt)")
        if p:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write("FH2MP recovery code. Keep it private: whoever has it can play as you.\n\n" + self.rec_code.text() + "\n")

    def reject(self):
        if self._job:
            self._job.cancelled = True
        super().reject()
