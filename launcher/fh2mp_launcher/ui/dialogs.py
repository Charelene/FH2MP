import os
import sys

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtCore import QUrl
from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QFileDialog, QFormLayout, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                               QFrame, QLayout, QPlainTextEdit, QPushButton, QScrollArea, QVBoxLayout, QWidget)

from .. import azimuth, keystore, launch, paths
from ..api import ApiError
from ..core import friendly
from . import images, theme
from .widgets import Avatar, hrule
from .workers import run_async


def _repo_shim_dir():
    """shim/build of the source tree: $FH2MP_REPO, else the checkout this file sits in, else ./shim/build; '' when none is found."""
    for base in (os.environ.get("FH2MP_REPO", ""), os.path.join(os.path.dirname(__file__), "..", "..", ".."), os.getcwd()):
        d = os.path.abspath(os.path.join(base, "shim", "build")) if base else ""
        if d and os.path.isdir(d):
            return d
    return ""


def fixed(dlg, width):
    """A dialog that is exactly as big as its content (no clipping, no resizing) and has only a close button."""
    dlg.setWindowFlags(Qt.WindowType.Dialog | Qt.WindowType.CustomizeWindowHint | Qt.WindowType.WindowTitleHint | Qt.WindowType.WindowCloseButtonHint)
    lay = dlg.layout()
    m = lay.contentsMargins()
    strut = QWidget()                                   # gives the layout its width; SetFixedSize then makes the dialog exactly that
    strut.setFixedSize(width - m.left() - m.right(), 0)
    lay.addWidget(strut)
    lay.setSizeConstraint(QLayout.SizeConstraint.SetFixedSize)


def open_folder(path):
    QDesktopServices.openUrl(QUrl.fromLocalFile(path))


class NameCheck:
    """Debounced 'is this gamertag free?' feedback under a line edit."""

    def __init__(self, edit, label, lc, on_state=None):
        self.edit, self.label, self.lc, self.on_state = edit, label, lc, on_state
        self.ok = False
        self._seq = 0
        self.timer = QTimer(edit)
        self.timer.setSingleShot(True)
        self.timer.timeout.connect(self._go)
        edit.textChanged.connect(self._changed)

    def _set(self, ok, text, colour=theme.MUTED):
        self.ok = ok
        self.label.setText(text)
        self.label.setStyleSheet(f"color: {colour};")
        if self.on_state:
            self.on_state(ok)

    def _changed(self, _t):
        self.ok = False
        name = self.edit.text().strip()
        if not name:
            self._set(False, "3 to 15 characters: letters, digits, space . _ -")
            return
        self._set(False, "checking...")
        self.timer.start(400)

    def _go(self):
        name, self._seq = self.edit.text().strip(), self._seq + 1
        seq = self._seq

        def done(r):
            if seq != self._seq:
                return
            if r["valid"] and r["available"]:
                self._set(True, "available", theme.GOOD)
            else:
                self._set(False, r["reason"] or "not available", theme.BAD)

        def failed(e):
            if seq == self._seq:
                self._set(True, "could not check right now (the server will check again)", theme.WARN)   # do not block on a flaky check
        run_async(lambda j: self.lc.api().check_name(name), done, failed)


class RecoveryDialog(QDialog):
    """Shows the recovery code (screen only, never sent anywhere)."""

    def __init__(self, lc, parent=None, require_ack=False):
        super().__init__(parent)
        self.setWindowTitle("Recovery code")
        code = lc.identity.recovery_code()
        lay = QVBoxLayout(self)
        lay.setContentsMargins(28, 24, 28, 24)
        lay.setSpacing(12)
        t = QLabel("Your recovery code")
        t.setObjectName("h2")
        lay.addWidget(t)
        m = QLabel("It's the only way back into your account. Keep it private: anyone with it can play as you.")
        m.setWordWrap(True)
        m.setObjectName("muted")
        lay.addWidget(m)
        c = QLabel(code)
        c.setObjectName("code")
        c.setWordWrap(True)
        c.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        lay.addWidget(c)
        row = QHBoxLayout()
        cp = QPushButton("Copy")
        cp.clicked.connect(lambda: (QGuiApplication.clipboard().setText(code), cp.setText("Copied")))
        sv = QPushButton("Save to file...")
        sv.clicked.connect(lambda: self._save(code))
        row.addWidget(cp)
        row.addWidget(sv)
        row.addStretch(1)
        lay.addLayout(row)
        self.ack = QCheckBox("I saved it")
        self.ok_btn = QPushButton("Done")                    # not `done`: that would shadow QDialog.done() and accept() would fail
        self.ok_btn.setObjectName("primary")
        self.ok_btn.clicked.connect(self.accept)
        if require_ack:
            self.ok_btn.setEnabled(False)
            self.ack.toggled.connect(self.ok_btn.setEnabled)
            lay.addWidget(self.ack)
        bottom = QHBoxLayout()
        bottom.addStretch(1)
        bottom.addWidget(self.ok_btn)
        lay.addLayout(bottom)
        fixed(self, 560)

    def _save(self, code):
        p, _ = QFileDialog.getSaveFileName(self, "Save recovery code", os.path.expanduser("~/fh2mp-recovery-code.txt"), "Text (*.txt)")
        if p:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write("FH2MP recovery code. Keep it private.\n\n" + code + "\n")


class ProfileDialog(QDialog):
    """Change gamertag (once a week) and gamerpic. Both are applied on the server; the picture is also written for Azimuth."""

    def __init__(self, lc, token, avatar_png, parent=None):
        super().__init__(parent)
        self.lc, self.token = lc, token
        self.setWindowTitle("Profile")
        self.changed = False
        self._png = None
        lay = QVBoxLayout(self)
        lay.setContentsMargins(28, 24, 28, 24)
        lay.setSpacing(12)
        top = QHBoxLayout()
        top.setSpacing(18)
        self.avatar = Avatar(96)
        self.avatar.set_name(lc.account["gamertag"] if lc.account else "")
        self.avatar.set_png(avatar_png)
        top.addWidget(self.avatar)
        col = QVBoxLayout()
        pick = QPushButton("Choose picture...")
        pick.clicked.connect(self._pick)
        self.reset = QPushButton("Remove")
        self.reset.clicked.connect(self._remove)
        col.addWidget(pick)
        col.addWidget(self.reset)
        col.addStretch(1)
        top.addLayout(col)
        top.addStretch(1)
        lay.addLayout(top)
        lay.addWidget(hrule())
        lay.addWidget(QLabel("Gamertag"))
        self.name = QLineEdit(lc.account["gamertag"] if lc.account else "")
        self.name.setMaxLength(15)
        lay.addWidget(self.name)
        self.hint = QLabel("")
        self.hint.setObjectName("small")
        lay.addWidget(self.hint)
        self.check = NameCheck(self.name, self.hint, lc)
        self.msg = QLabel("")
        self.msg.setWordWrap(True)
        lay.addWidget(self.msg)
        bottom = QHBoxLayout()
        bottom.addStretch(1)
        cancel = QPushButton("Close")
        cancel.clicked.connect(self.reject)
        self.save = QPushButton("Save")
        self.save.setObjectName("primary")
        self.save.clicked.connect(self._save)
        bottom.addWidget(cancel)
        bottom.addWidget(self.save)
        lay.addLayout(bottom)
        self._local_png = None
        self._remove_pic = False
        fixed(self, 440)

    def _pick(self):
        p, _ = QFileDialog.getOpenFileName(self, "Choose a picture", os.path.expanduser("~"), "Images (*.png *.jpg *.jpeg *.webp *.bmp)")
        if not p:
            return
        res = images.prepare(p)
        if not res:
            self.msg.setText("Can't read that image.")
            return
        self._png, self._local_png = res
        self._remove_pic = False
        self.avatar.set_png(self._png)

    def _remove(self):
        self._png, self._local_png, self._remove_pic = None, None, True
        self.avatar.set_png(None)

    def _save(self):
        lc, token = self.lc, self.token
        new_name = self.name.text().strip()
        rename = bool(lc.account) and new_name != lc.account["gamertag"]
        png, local, remove = self._png, self._local_png, self._remove_pic
        self.save.setEnabled(False)
        self.msg.setStyleSheet("")
        self.msg.setText("")

        def work(job):
            api = lc.api()
            out = {}
            if rename:
                out["account"] = api.rename(token, new_name)
            if png:
                api.upload_gamerpic(token, png)
                out["pic"] = True
            if remove:
                api.clear_gamerpic(token)
                out["pic"] = False
            return out

        def ok(out):
            if "account" in out:
                lc.account = {**lc.account, **out["account"]}
                lc.settings["gamertag"] = lc.account["gamertag"]
                lc.settings.save()
            d = lc.settings["azimuth_dir"]
            if local and d and os.path.isdir(d):
                azimuth.write_gamerpic(d, local)
                with open(os.path.join(paths.config_dir(), "gamerpic.png"), "wb") as f:
                    f.write(local)
            self.changed = True
            self.accept()

        def failed(e):
            self.save.setEnabled(True)
            self.msg.setStyleSheet(f"color: {theme.BAD};")
            self.msg.setText(friendly(e))
        run_async(work, ok, failed)


class SettingsDialog(QDialog):
    def __init__(self, lc, parent=None):
        super().__init__(parent)
        self.lc = lc
        s = lc.settings
        dev = paths.is_dev()
        self.setWindowTitle("Settings")
        lay = QVBoxLayout(self)
        lay.setContentsMargins(28, 24, 28, 24)
        lay.setSpacing(10)

        lay.addWidget(QLabel("Azimuth folder"))
        row = QHBoxLayout()
        self.azdir = QLineEdit(s["azimuth_dir"])
        b = QPushButton("Browse...")
        b.clicked.connect(self._browse)
        row.addWidget(self.azdir, 1)
        row.addWidget(b)
        lay.addLayout(row)

        if sys.platform != "win32":
            lay.addWidget(hrule())
            f2 = QFormLayout()
            f2.setSpacing(10)
            self.mode = QComboBox()
            for label, v in (("Automatic", "auto"), ("Proton", "proton"), ("Wine", "wine"), ("Custom command", "custom")):
                self.mode.addItem(label, v)
            self.mode.setCurrentIndex(max(0, self.mode.findData(s["launch_mode"])))
            f2.addRow("Run with", self.mode)
            self.proton = QLineEdit(s["proton"])
            self.proton.setPlaceholderText(launch.find_proton() or "path to proton")
            f2.addRow("Proton", self.proton)
            self.prefix = QLineEdit(s["prefix"])
            detected = launch.detect_prefix(self.azdir.text().strip()) if self.azdir.text().strip() else ""
            self.prefix.setPlaceholderText(detected or "Proton prefix with your game files")
            f2.addRow("Prefix", self.prefix)
            self.custom = QLineEdit(s["custom_command"])
            self.custom.setPlaceholderText("steam steam://rungameid/...   ({exe} {dir})")
            f2.addRow("Command", self.custom)
            lay.addLayout(f2)
        self.extra = QLineEdit(s["extra_args"])
        self.extra.hide()
        self.channel = QComboBox()
        self.base = QLineEdit(s["api_base"])
        self.insecure = QCheckBox()
        self.check_updates = QCheckBox()
        self.check_updates.setChecked(s["check_updates"])

        if dev:
            lay.addWidget(hrule())
            t = QLabel("Developer")
            t.setObjectName("h2")
            lay.addWidget(t)
            self.channel.addItem("Stable", "stable")
            self.channel.addItem("Dev", "dev")
            self.channel.setCurrentIndex(0 if s["channel"] == "stable" else 1)
            fd = QFormLayout()
            fd.addRow("Channel", self.channel)
            lay.addLayout(fd)
            self.base.setPlaceholderText("Server address (empty = channel default)")
            lay.addWidget(self.base)
            self.insecure.setText("Allow http")
            self.insecure.setChecked(s["allow_insecure"])
            lay.addWidget(self.insecure)
            self.check_updates.setText("Check for updates")
            lay.addWidget(self.check_updates)
            self.extra.show()
            self.extra.setPlaceholderText("Extra Azimuth arguments")
            lay.addWidget(self.extra)
            preset = QComboBox()
            for label, url in (("Server preset...", None), ("Dev domain", ""), ("Dev via ssh tunnel (127.0.0.1:5180)", "http://127.0.0.1:5180"),
                               ("Prod via ssh tunnel (127.0.0.1:5080)", "http://127.0.0.1:5080")):
                preset.addItem(label, url)
            preset.currentIndexChanged.connect(lambda _i: preset.currentData() is not None and self.base.setText(preset.currentData()))
            lay.addWidget(preset)
            self.pin = QCheckBox("Keep my shim (no automatic updates)")
            self.pin.setChecked(s["pin_shim"])
            lay.addWidget(self.pin)
            row2 = QHBoxLayout()
            b1 = QPushButton("Install shim from file...")
            b1.clicked.connect(self._shim_from_file)
            row2.addWidget(b1)
            repo = _repo_shim_dir()
            if repo:
                for name in ("winmm-debug.dll", "winmm.dll"):
                    if os.path.isfile(os.path.join(repo, name)):
                        b = QPushButton(f"Repo: {name}")
                        b.clicked.connect(lambda _c=False, n=name: self._install_local(os.path.join(repo, n)))
                        row2.addWidget(b)
            row2.addStretch(1)
            lay.addLayout(row2)
            self.shim_msg = QLabel("")
            self.shim_msg.setObjectName("small")
            self.shim_msg.setWordWrap(True)
            lay.addWidget(self.shim_msg)
            lay.addWidget(QLabel("Game environment (KEY=VALUE per line)"))
            self.env = QPlainTextEdit(s["extra_env"])
            self.env.setPlaceholderText("FH2MP_NET_TRAFFIC=1")
            self.env.setFixedHeight(70)
            lay.addWidget(self.env)

        lay.addWidget(hrule())
        tools = QHBoxLayout()
        for text, fn in (("Data folder", lambda: open_folder(paths.config_dir())), ("Recovery code", self._recovery)):
            bt = QPushButton(text)
            bt.clicked.connect(fn)
            tools.addWidget(bt)
        tools.addStretch(1)
        lay.addLayout(tools)

        bottom = QHBoxLayout()
        bottom.addStretch(1)
        cancel = QPushButton("Cancel")
        cancel.clicked.connect(self.reject)
        save = QPushButton("Save")
        save.setObjectName("primary")
        save.clicked.connect(self._save)
        bottom.addWidget(cancel)
        bottom.addWidget(save)
        lay.addLayout(bottom)
        fixed(self, 560 if not dev else 620)

    def _browse(self):
        d = QFileDialog.getExistingDirectory(self, "Folder with azimuth.exe", self.azdir.text() or os.path.expanduser("~"))
        if d:
            self.azdir.setText(d)

    def _shim_from_file(self):
        p, _ = QFileDialog.getOpenFileName(self, "Shim DLL or zip", self.lc.settings["dev_shim_path"] or _repo_shim_dir() or os.path.expanduser("~"),
                                           "Shim (*.dll *.zip)")
        if p:
            self._install_local(p)

    def _install_local(self, path):
        d = self.azdir.text().strip() or self.lc.settings["azimuth_dir"]
        if not d or not os.path.isfile(os.path.join(d, "azimuth.exe")):
            self.shim_msg.setText("Set a valid Azimuth folder first.")
            return
        self.lc.settings["azimuth_dir"] = d
        try:
            did = self.lc.install_shim_file(path)
            self.shim_msg.setStyleSheet(f"color: {theme.GOOD};")
            self.shim_msg.setText("; ".join(did).capitalize() + ".")
        except Exception as e:  # noqa: BLE001
            self.shim_msg.setStyleSheet(f"color: {theme.BAD};")
            self.shim_msg.setText(str(e))

    def _recovery(self):
        if self.lc.identity:
            RecoveryDialog(self.lc, self).exec()

    def _save(self):
        s = self.lc.settings
        d = self.azdir.text().strip()
        if d and not os.path.isfile(os.path.join(d, "azimuth.exe")):
            QMessageBox.warning(self, "Azimuth folder", "azimuth.exe isn't in that folder.")
            return
        s["azimuth_dir"] = d
        if paths.is_dev():                          # players cannot change the channel or the server
            s["channel"] = self.channel.currentData()
            s["check_updates"] = self.check_updates.isChecked()
            s["extra_args"] = self.extra.text().strip()
            s["api_base"] = self.base.text().strip()
            s["allow_insecure"] = self.insecure.isChecked()
            s["pin_shim"] = self.pin.isChecked()
            s["extra_env"] = self.env.toPlainText().strip()
        if sys.platform != "win32":
            s["launch_mode"] = self.mode.currentData()
            s["proton"] = self.proton.text().strip()
            s["prefix"] = self.prefix.text().strip()
            s["custom_command"] = self.custom.text().strip()
        s.save()
        self.accept()
