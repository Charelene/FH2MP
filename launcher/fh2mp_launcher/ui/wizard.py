"""Launcher installation wizard and the shared account registration/recovery flow."""
import os

from PySide6.QtCore import Qt, QTimer, QEvent
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtCore import QUrl
from PySide6.QtWidgets import (QButtonGroup, QCheckBox, QDialog, QFrame, QHBoxLayout, QLabel, QLineEdit, QProgressBar,
                               QPushButton, QRadioButton, QSizePolicy, QLayout, QStackedWidget, QVBoxLayout, QWidget)

from .. import azimuth, identity
from ..api import ApiError
from ..constants import AZIMUTH_PAGE, AZIMUTH_VERSION
from ..core import friendly
from .dialogs import NameCheck
from .widgets import Banner, pick_dir, save_file
from .workers import run_async


class AccountWizard(QDialog):
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
        n.nav_row = row
        return b, n

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
                if not self.existing_key and not lc.has_identity():       # (a key kept after an unclear failure is reused on retry)
                    lc.create_identity()
                try:
                    return lc.sign_up(tag, invite, progress=lambda n: job.progress(n), cancelled=lambda: job.cancelled)
                except BaseException as e:
                    # Do not keep a key that has no account behind it, but only when that is certain: if the request was sent and its
                    # answer never arrived, the account may exist and this key is the only way into it.
                    if not self.existing_key and not getattr(e, "uncertain", False):
                        lc.forget_identity()
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
        p = save_file(self, "Save recovery code", os.path.expanduser("~/fh2mp-recovery-code.txt"), "Text (*.txt)")
        if p:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write("FH2MP recovery code. Keep it private: whoever has it can play as you.\n\n" + self.rec_code.text() + "\n")

    def reject(self):
        if self._job:
            self._job.cancelled = True
        super().reject()


class Wizard(AccountWizard):
    """Installation setup also runs once for existing accounts, independently of identity."""
    ACQUIRE, DESTINATION, DEPENDENCIES, MULTIPLAYER, LINUX, DONE = range(5, 11)

    def __init__(self, lc, parent=None, existing_key=False):
        from .. import setup
        self.packages = setup.Packages(lc)
        self.game_path = ''
        self.az_path = lc.settings['azimuth_dir']
        self.archive = ''
        self._validated_archive = ''
        self._dep_ready = {'platform': False, 'extra': False}
        self.update_folder = ''
        self.butler = None
        self.itch_profile = None
        self._busy_setup = False
        self._setup_job = None
        self._closing = False
        self._resize_pending = False
        super().__init__(lc, parent, existing_key)
        self.setWindowTitle('Set up FH2MP')
        from . import theme
        self.setObjectName('setup-wizard')
        self.setStyleSheet(f"""
            QLabel#setup-heading {{ font-size: 24px; font-weight: 700; color: {theme.WHITE}; }}
            QLabel#setup-credit {{ font-size: 12px; color: {theme.MUTED}; }}
            QFrame#setup-package {{ background: {theme.SURFACE}; border: 1px solid {theme.BORDER}; border-radius: 12px; }}
            QFrame#setup-package QLabel {{ background: transparent; border: none; }}
        """)
        self.stack.addWidget(self._acquire_page())
        self.stack.addWidget(self._destination_page())
        self.stack.addWidget(self._dependencies_page())
        self.stack.addWidget(self._multiplayer_page())
        self.stack.addWidget(self._linux_page())
        self.stack.addWidget(self._done_page())
        self.acct_back.clicked.disconnect()
        self.acct_back.clicked.connect(lambda: self.stack.setCurrentIndex(self.LINUX if os.name != 'nt' else self.MULTIPLAYER))
        self.game_edit.setText(lc.settings['azimuth_dir'] and azimuth.fh2_title(azimuth.read_ini(lc.settings['azimuth_dir'])).get('folder', '') or '')
        if os.name != 'nt' and self.game_edit.text().upper().startswith('Z:'):
            self.game_edit.setText(self.game_edit.text()[2:].replace('\\', '/'))
        self.az_edit.setText(lc.settings['azimuth_dir'])
        self.stack.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Ignored)
        self.stack.currentChanged.connect(self._size_for_page)
        self._size_for_page(self.stack.currentIndex())

    def _page(self, title, sub=None):
        page = QWidget()
        page.setMinimumSize(0, 0)
        page.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Ignored)
        root = QVBoxLayout(page)
        root.setSizeConstraint(QLayout.SizeConstraint.SetNoConstraint)
        root.setContentsMargins(24, 24, 24, 24)
        root.setSpacing(0)
        header = QWidget(page)
        title_box = QVBoxLayout(header)
        title_box.setContentsMargins(0, 0, 0, 0)
        title_box.setSpacing(10)
        heading = QLabel(title)
        heading.setWordWrap(True)
        heading.setObjectName('setup-heading')
        title_box.addWidget(heading)
        page.subtitle = QLabel(sub or '')
        page.subtitle.setObjectName('muted')
        page.subtitle.setWordWrap(True)
        if sub:
            title_box.addWidget(page.subtitle)
        else:
            page.subtitle.hide()
        root.addWidget(header)
        root.addSpacing(24)
        body = QWidget(page)
        content = QVBoxLayout(body)
        content.setContentsMargins(0, 0, 0, 0)
        content.setSpacing(24)
        content.setup_page = page
        root.addWidget(body)
        page.installEventFilter(self)
        return page, content

    def eventFilter(self, watched, event):
        if event.type() == QEvent.Type.LayoutRequest and watched is self.stack.currentWidget():
            self._schedule_size()
        return super().eventFilter(watched, event)

    def _nav(self, lay, back=None, nxt=None, next_text='Next'):
        page = lay.setup_page
        footer = QWidget(page)
        row = QHBoxLayout(footer)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(8)
        row.addStretch(1)
        previous = None
        if back is not None:
            previous = QPushButton('Back')
            previous.clicked.connect(lambda: self.stack.setCurrentIndex(back))
            row.addWidget(previous)
        advance = QPushButton(next_text)
        advance.setObjectName('primary')
        if nxt:
            advance.clicked.connect(nxt)
        row.addWidget(advance)
        page.layout().addSpacing(24)
        page.layout().addWidget(footer)
        advance.nav_row = row
        return previous, advance

    def _schedule_size(self):
        if self._resize_pending or self._closing:
            return
        self._resize_pending = True
        def update():
            self._resize_pending = False
            self._size_for_page()
        QTimer.singleShot(0, self, update)

    def _size_for_page(self, index=None):
        index = self.stack.currentIndex() if index is None else index
        page = self.stack.widget(index)
        if page is None:
            return
        screen = self.screen().availableGeometry()
        width = min(600, screen.width() - 40)
        layout = page.layout()
        layout.invalidate()
        # Measure this page alone: hidden pages must not inflate the dialog.
        height = layout.totalHeightForWidth(width)
        if height < 0:
            height = layout.sizeHint().height()
        height = min(max(160, height), screen.height() - 80)
        if self.width() != width or self.height() != height:
            self.setFixedSize(width, height)
        self._keep_on_screen()

    def showEvent(self, e):
        super().showEvent(e)
        QTimer.singleShot(0, self, self._keep_on_screen)      # after the platform has placed the frame (centred on a parent that may be large)

    def _keep_on_screen(self):
        """A page that grows pushes the bottom of the dialog off the screen, and a taller one than the parent's centre allows its top: both are
        moved back inside the available area (Windows showed the dialog with its top above the screen)."""
        if not self.isVisible():
            return
        area = self.screen().availableGeometry()
        frame = self.frameGeometry()
        x = min(max(frame.x(), area.left()), max(area.left(), area.right() - frame.width() + 1))
        y = min(max(frame.y(), area.top()), max(area.top(), area.bottom() - frame.height() + 1))
        if (x, y) != (frame.x(), frame.y()):
            self.move(x, y)

    def _footer_install(self, next_button, callback):
        button = QPushButton('Install')
        button.setObjectName('primary')
        button.clicked.connect(callback)
        next_button.nav_row.insertWidget(next_button.nav_row.count() - 1, button)
        button.hide()
        next_button.hide()
        return button

    def _package_card(self, lay, name, version, author=None, url=None, description=''):
        frame = QFrame()
        frame.setObjectName('setup-package')
        row = QHBoxLayout(frame)
        row.setContentsMargins(16, 14, 16, 14)
        row.setSpacing(18)
        text = QVBoxLayout()
        text.setSpacing(4)
        title = QLabel(f'{name} <span style="font-weight:400; font-size:12px">{version}</span>')
        title.setStyleSheet('font-weight: 600; font-size: 16px;')
        text.addWidget(title)
        if author:
            from . import theme
            credit = QLabel(f'By <a style="color:{theme.ACCENT}" href="{url}">{author}</a>')
            credit.setObjectName('setup-credit')
            credit.setOpenExternalLinks(True)
            text.addWidget(credit)
        elif description:
            credit = QLabel(description)
            credit.setObjectName('setup-credit')
            text.addWidget(credit)
        row.addLayout(text, 1)
        status = QLabel('')
        status.setObjectName('setup-credit')
        row.addWidget(status)
        get = QPushButton('Get')
        row.addWidget(get)
        lay.addWidget(frame)
        return get, status

    def _text(self, lay, text):
        from . import theme
        text = text.replace('<a ', f'<a style="color:{theme.ACCENT}" ')
        label = QLabel(text)
        label.setWordWrap(True)
        label.setOpenExternalLinks(True)
        lay.addWidget(label)
        return label

    def _folder(self, lay, title, initial=''):
        row = QHBoxLayout()
        row.setSpacing(8)
        edit = QLineEdit(initial)
        edit.setPlaceholderText('Choose a folder…')
        browse = QPushButton('Browse…')
        browse.clicked.connect(lambda: self._pick_folder(edit, title))
        row.addWidget(edit, 1)
        row.addWidget(browse)
        lay.addLayout(row)
        return edit

    def _pick_folder(self, edit, title):
        folder = pick_dir(self, title, edit.text() or os.path.expanduser('~'))
        if folder:
            edit.setText(folder)

    def _welcome(self):
        w, lay = self._page("Let's set up your launcher",
                           'You need your own licensed copy of Forza Horizon 2 for Xbox One. We cannot provide game files.')
        field = QWidget()
        field_box = QVBoxLayout(field)
        field_box.setContentsMargins(0, 0, 0, 0)
        field_box.setSpacing(6)
        self.game_folder_label = self._text(field_box, 'Game folder (the one containing Mount)')
        self.game_edit = self._folder(field_box, 'Select your Forza Horizon 2 game folder')
        self.game_folder_label.setBuddy(self.game_edit)
        lay.addWidget(field)
        self.game_banner = Banner()
        lay.addWidget(self.game_banner)
        lay.addStretch(1)
        self._nav(lay, None, self._game_next)
        return w

    def _game_next(self):
        from .. import setup
        try:
            self.game_path = setup.game_folder(self.game_edit.text())
        except Exception as e:
            self.game_banner.show_message(str(e), 'bad')
            return
        self.stack.setCurrentIndex(self.AZIMUTH)

    def _azimuth(self):
        w, lay = self._page('Do you already have Azimuth?')
        self.have_az = QRadioButton('Yes, use an existing installation')
        self.need_az = QRadioButton('No, get Azimuth')
        self.have_az.setChecked(True)
        choices = QWidget()
        choice_box = QVBoxLayout(choices)
        choice_box.setContentsMargins(0, 0, 0, 0)
        choice_box.setSpacing(10)
        choice_box.addWidget(self.have_az)
        choice_box.addWidget(self.need_az)
        lay.addWidget(choices)
        self.az_folder_box = QWidget()
        box = QVBoxLayout(self.az_folder_box)
        box.setContentsMargins(0, 0, 0, 0)
        box.setSpacing(8)
        self.az_edit = self._folder(box, 'Select the folder containing azimuth.exe')
        lay.addWidget(self.az_folder_box)
        self.have_az.toggled.connect(self.az_folder_box.setVisible)
        self.have_az.toggled.connect(lambda _: self._schedule_size())
        self.az_banner = Banner()
        lay.addWidget(self.az_banner)
        lay.addStretch(1)
        self._nav(lay, self.WELCOME, self._az_next)
        return w

    def _az_next(self):
        from .. import setup
        self.update_folder = ''
        if self.need_az.isChecked():
            self.stack.setCurrentIndex(self.ACQUIRE)
            return
        folder = os.path.abspath(os.path.expanduser(self.az_edit.text()))
        if not os.path.isfile(os.path.join(folder, 'azimuth.exe')):
            self.az_banner.show_message('Select the folder containing azimuth.exe.', 'bad')
            return
        if not azimuth.inspect(folder, self.lc.builds()).supported:
            self.update_folder = folder
            self.acquire_notice.setText(f'This installation needs Azimuth {AZIMUTH_VERSION}. Your settings and saves will be kept.')
            self.stack.setCurrentIndex(self.ACQUIRE)
            return
        self._use_azimuth(folder)

    def _use_azimuth(self, folder):
        from .. import shim
        from ..version import __version__
        # Called only after a successful install, or an explicit existing-folder choice.
        azimuth.configure_game(folder, self.game_path if os.name == 'nt' else 'Z:' + self.game_path.replace('/', '\\'), preserve_content=True)
        self.lc.settings['azimuth_dir'] = folder
        self.az_path = folder
        self.lc.settings['extra_options'] = True
        self.lc.settings.save()
        self.packages.dependencies.clear()
        self.packages.shim_package = None
        self._dep_ready = {'platform': shim.platform_ok(folder), 'extra': shim.extra_options_complete(folder)}
        for kind, button in self.dep_get.items():
            button.setEnabled(True)
            button.setVisible(not self._dep_ready[kind])
            self.dep_status[kind].setText('Installed' if self._dep_ready[kind] else '')
        self.dep_install.hide()
        ready = all(self._dep_ready.values())
        self.dep_next.setEnabled(ready)
        self.dep_next.setVisible(ready)
        self.mod_get.setEnabled(True)
        mod_ready = shim.installed(folder) == ('ours', __version__) and self.packages.shim_current()
        self.mod_get.setVisible(not mod_ready)
        self.mod_status.setText('Installed' if mod_ready else '')
        self.mod_install.hide()
        self.mod_next.setEnabled(mod_ready)
        self.mod_next.setVisible(mod_ready)
        if ready:
            self._enter_multiplayer()
        else:
            self.stack.setCurrentIndex(self.DEPENDENCIES)

    def _enter_multiplayer(self):
        self.stack.setCurrentIndex(self.MULTIPLAYER)
        if self.mod_next.isEnabled():
            self._after_multiplayer()

    def _acquire_page(self):
        w, lay = self._page(f'Get Azimuth {AZIMUTH_VERSION}', 'Choose how to get Azimuth.')
        self.acquire_notice = w.subtitle
        choices = QHBoxLayout()
        self.auto_az = QRadioButton('Download in launcher')
        self.manual_az = QRadioButton('Use a ZIP file')
        self.auto_az.setChecked(True)
        choices.addWidget(self.auto_az)
        choices.addWidget(self.manual_az)
        choices.addStretch(1)
        lay.addLayout(choices)
        self.auto_box = QWidget()
        automatic = QVBoxLayout(self.auto_box)
        automatic.setContentsMargins(0, 0, 0, 0)
        automatic.setSpacing(8)
        self.itch_status = self._text(automatic, f'The launcher downloads Azimuth {AZIMUTH_VERSION} from the FH2MP server and checks it.')
        row = QHBoxLayout()
        self.itch_login = QPushButton('Login with itch.io')              # kept for old tests and tools; the download no longer needs it
        self.itch_login.clicked.connect(self._itch_login)
        self.itch_login.hide()
        self.itch_get = QPushButton('Download Azimuth')
        self.itch_get.setObjectName('primary')
        self.itch_get.clicked.connect(self._download_azimuth)
        row.addWidget(self.itch_login)
        row.addWidget(self.itch_get)
        row.addStretch(1)
        automatic.addLayout(row)
        lay.addWidget(self.auto_box)
        self.manual_box = QWidget()
        manual = QVBoxLayout(self.manual_box)
        manual.setContentsMargins(0, 0, 0, 0)
        manual.setSpacing(8)
        self._text(manual, f'<a href="{AZIMUTH_PAGE}">Download from itch.io</a>, then select the ZIP.')
        row = QHBoxLayout()
        self.zip_pick = QPushButton('Choose ZIP…')
        self.zip_pick.clicked.connect(self._zip_browse)
        self.zip_name = QLabel('No ZIP selected')
        self.zip_name.setObjectName('setup-credit')
        row.addWidget(self.zip_pick)
        row.addWidget(self.zip_name, 1)
        manual.addLayout(row)
        lay.addWidget(self.manual_box)
        self.acquire_progress = QProgressBar()
        self.acquire_progress.hide()
        lay.addWidget(self.acquire_progress)
        self.acquire_banner = Banner()
        lay.addWidget(self.acquire_banner)
        lay.addStretch(1)
        credit = self._text(lay, '<a href="https://almost-there-games.itch.io/">Azimuth by Almost There Games</a>. '
                            '<a href="https://ko-fi.com/F1L5254Z5I">Support Azimuth</a><br>FH2MP is not affiliated with Almost There Games.')
        credit.setObjectName('setup-credit')
        _, self.acquire_next = self._nav(lay, self.AZIMUTH, self._archive_next)
        self.acquire_next.setEnabled(False)
        self.auto_az.toggled.connect(self._acquire_mode)
        self._acquire_mode()
        return w

    def _acquire_mode(self):
        automatic = self.auto_az.isChecked()
        self.auto_box.setVisible(automatic)
        self.manual_box.setVisible(not automatic)
        self.itch_login.hide()
        self.itch_get.setVisible(True)
        self.itch_get.setEnabled(True)
        self.acquire_banner.hide()
        self.archive = ''
        self._validated_archive = ''
        self.acquire_next.setEnabled(False)
        self.zip_name.setText('No ZIP selected')
        self._schedule_size()

    def _zip_browse(self):
        from .widgets import open_file
        path = open_file(self, f'Select Azimuth {AZIMUTH_VERSION} ZIP', os.path.expanduser('~'), 'ZIP files (*.zip)')
        if path:
            self._select_archive(path)

    def _select_archive(self, path):
        from .. import setup
        self.archive = ''
        self._validated_archive = ''
        self.acquire_next.setEnabled(False)
        self.zip_name.setText('Checking ZIP…')
        def ok(_version):
            self.archive = self._validated_archive = path
            self.zip_name.setText(os.path.basename(path) + f' (Azimuth {AZIMUTH_VERSION})')
            if self.auto_az.isChecked():
                self.itch_status.setText(f'Azimuth {AZIMUTH_VERSION} is ready to install.')
                self.itch_get.hide()
            self.acquire_next.setEnabled(True)
            if self.auto_az.isChecked():
                self._archive_next()
        def failed(error):
            self.zip_name.setText('Choose a different ZIP')
            self.acquire_banner.show_message(friendly(error), 'bad')
        self._operation(lambda job: setup.validate_archive(path), ok, self.acquire_banner, self.acquire_progress, failed=failed)

    def _itch_login(self):
        from .. import itch
        from .workers import dispatcher
        def work(job):
            if self.butler:
                self.butler.close()
            self.butler = itch.Butler(job.progress, lambda: job.cancelled)
            def challenge(value):
                # Restrict URLs sent by the daemon to the official login host.
                url = QUrl(value.get('url', ''))
                if url.scheme() != 'https' or url.host() != 'itch.io':
                    raise ValueError('Unexpected itch.io login address.')
                dispatcher().call.emit(lambda: (self.itch_status.setText('Complete the itch.io sign-in in your browser, then return here.'),
                                                QDesktopServices.openUrl(url)))
            return self.butler.login(challenge)
        def ok(profile):
            self.itch_profile = profile
            self.itch_status.setText('Signed in to itch.io.')
            self.itch_login.hide()
            self.itch_get.show()
            self.itch_get.setEnabled(True)
        self._operation(work, ok, self.acquire_banner, self.acquire_progress)

    def _download_azimuth(self):
        def ok(path):
            self._select_archive(path)
        self._operation(lambda job: self.packages.get_azimuth(job.progress, lambda: job.cancelled), ok, self.acquire_banner, self.acquire_progress)

    def _itch_download(self):
        import tempfile
        from .. import paths
        def work(job):
            destination = tempfile.mkdtemp(prefix='azimuth-download-', dir=paths.cache_dir())
            self.butler.cancelled = lambda: job.cancelled
            return self.butler.download(self.itch_profile, destination, job.progress)
        def ok(path):
            self._select_archive(path)
        self._operation(work, ok, self.acquire_banner, self.acquire_progress)

    def _archive_next(self):
        if not self.archive or self.archive != self._validated_archive:
            self.acquire_banner.show_message(f'Select an Azimuth {AZIMUTH_VERSION} ZIP first.', 'warn')
            return
        if self.update_folder:
            self._install_azimuth(self.update_folder, self.acquire_banner, self.acquire_progress)
        else:
            self.stack.setCurrentIndex(self.DESTINATION)

    def _destination_page(self):
        w, lay = self._page('Choose an installation folder')
        self.destination_edit = self._folder(lay, 'Select an empty folder for Azimuth', os.path.expanduser('~/Games/Azimuth'))
        self.destination_banner = Banner()
        lay.addWidget(self.destination_banner)
        self.destination_progress = QProgressBar()
        self.destination_progress.hide()
        lay.addWidget(self.destination_progress)
        lay.addStretch(1)
        self._nav(lay, self.ACQUIRE, self._fresh_install, 'Install Azimuth')
        return w

    def _fresh_install(self):
        folder = self.destination_edit.text().strip()
        if not folder:
            self.destination_banner.show_message('Choose an installation folder.', 'bad')
            return
        folder = os.path.abspath(os.path.expanduser(folder))
        if os.path.exists(folder) and os.listdir(folder):
            if os.path.isfile(os.path.join(folder, 'azimuth.exe')):
                if azimuth.inspect(folder, self.lc.builds()).supported:
                    self._use_azimuth(folder)
                    return
            else:
                self.destination_banner.show_message('This folder contains other files. Choose an Azimuth installation or an empty folder.', 'bad')
                return
        self._install_azimuth(folder, self.destination_banner, self.destination_progress)

    def _install_azimuth(self, folder, banner, bar):
        from .. import setup
        self._operation(lambda job: setup.install_azimuth(self.archive, folder, lambda: job.cancelled),
                        self._use_azimuth, banner, bar)

    def _dependencies_page(self):
        from ..constants import UPSTREAM
        w, lay = self._page('Install the add-ons', 'These two add-ons are required for FH2MP.')
        self.dep_get, self.dep_status = {}, {}
        for kind, pin, url in [('platform', UPSTREAM['platform'], 'https://github.com/Trinculo54/fh2-mod-platform'),
                               ('extra', UPSTREAM['extraOptions'], 'https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions')]:
            button, status = self._package_card(lay, pin['name'], pin['version'], pin['author'], url)
            button.clicked.connect(lambda _checked=False, k=kind: self._get_dependency(k))
            self.dep_get[kind], self.dep_status[kind] = button, status
        self.dep_bar = QProgressBar()
        self.dep_bar.hide()
        lay.addWidget(self.dep_bar)
        self.dep_banner = Banner()
        lay.addWidget(self.dep_banner)
        lay.addStretch(1)
        _, self.dep_next = self._nav(lay, self.AZIMUTH, lambda: self.stack.setCurrentIndex(self.MULTIPLAYER))
        self.dep_next.setEnabled(False)
        self.dep_install = self._footer_install(self.dep_next, self._install_dependencies)
        return w

    def _get_dependency(self, kind):
        def ok(_):
            from . import theme
            self.dep_status[kind].setText('Downloaded')
            self.dep_status[kind].setStyleSheet(f'color: {theme.ACCENT}; font-size: 12px;')
            self.dep_get[kind].hide()
            self.dep_install.setVisible(all(self._dep_ready[k] or k in self.packages.dependencies for k in self.dep_get))
        self._operation(lambda job: self.packages.get_dependency(kind, job.progress, lambda: job.cancelled), ok, self.dep_banner, self.dep_bar)

    def _install_dependencies(self):
        def ok(_):
            from . import theme
            self.dep_install.hide()
            for label in self.dep_status.values():
                label.setText('Installed')
                label.setStyleSheet(f'color: {theme.GOOD}; font-size: 12px;')
            self.dep_next.setEnabled(True)
            self.dep_next.show()
            self._enter_multiplayer()
        self._operation(lambda job: self.packages.install_dependencies(), ok, self.dep_banner, self.dep_bar)

    def _multiplayer_page(self):
        from ..version import __version__
        w, lay = self._page('Install FH2MP')
        self.mod_get, self.mod_status = self._package_card(lay, 'FH2MP', __version__, description='Multiplayer for Forza Horizon 2')
        self.mod_get.clicked.connect(self._get_multiplayer)
        self.mod_bar = QProgressBar()
        self.mod_bar.hide()
        lay.addWidget(self.mod_bar)
        self.mod_banner = Banner()
        lay.addWidget(self.mod_banner)
        lay.addStretch(1)
        _, self.mod_next = self._nav(lay, self.DEPENDENCIES, self._after_multiplayer)
        self.mod_next.setEnabled(False)
        self.mod_install = self._footer_install(self.mod_next, self._install_multiplayer)
        return w

    def _get_multiplayer(self):
        def ok(version):
            from . import theme
            self.mod_status.setText('Downloaded')
            self.mod_status.setStyleSheet(f'color: {theme.ACCENT}; font-size: 12px;')
            self.mod_get.hide()
            self.mod_install.show()
        self._operation(lambda job: self.packages.get_shim(job.progress, lambda: job.cancelled), ok, self.mod_banner, self.mod_bar)

    def _install_multiplayer(self):
        def ok(_):
            from . import theme
            self.mod_status.setText('Installed')
            self.mod_status.setStyleSheet(f'color: {theme.GOOD}; font-size: 12px;')
            self.mod_install.hide()
            self.mod_next.setEnabled(True)
            self.mod_next.show()
            self._after_multiplayer()
        self._operation(lambda job: self.packages.install_shim(), ok, self.mod_banner, self.mod_bar)

    def _after_multiplayer(self):
        if os.name == 'nt':
            self._account_or_done()
            return
        from .. import launch, linux_support, paths
        proton = self.lc.settings['proton'] or launch.find_proton()
        prefix = launch.detect_prefix(self.lc.settings['azimuth_dir']) or self.lc.settings['prefix'] or os.path.join(paths.config_dir(), 'prefix')
        self.proton_edit.setText(proton)
        self.prefix_edit.setText(prefix)
        try:
            valid_proton = launch.resolve_proton(proton)
        except launch.LaunchError:
            valid_proton = ''
        if valid_proton and linux_support.ready(prefix):
            self._save_linux_settings(valid_proton, prefix)
            self._account_or_done()
        else:
            self.stack.setCurrentIndex(self.LINUX)

    def _linux_page(self):
        from .. import paths
        w, lay = self._page('Set up Linux support', 'Your launcher will configure Proton and the graphics components Azimuth needs.')
        self.linux_advanced = QCheckBox('Change Proton settings')
        lay.addWidget(self.linux_advanced)
        self.linux_options = QWidget()
        options = QVBoxLayout(self.linux_options)
        options.setContentsMargins(0, 0, 0, 0)
        options.setSpacing(12)
        self._text(options, 'Proton')
        self.proton_edit = self._folder(options, 'Select your Proton folder')
        self._text(options, 'Proton prefix (the recommended folder is filled in)')
        self.prefix_edit = self._folder(options, 'Select your Proton prefix', os.path.join(paths.config_dir(), 'prefix'))
        lay.addWidget(self.linux_options)
        self.linux_options.hide()
        self.linux_advanced.toggled.connect(self.linux_options.setVisible)
        self.linux_advanced.toggled.connect(lambda _: self._size_for_page(self.LINUX))
        self.linux_banner = Banner()
        lay.addWidget(self.linux_banner)
        self.linux_progress = QProgressBar()
        self.linux_progress.hide()
        lay.addWidget(self.linux_progress)
        lay.addStretch(1)
        self._nav(lay, self.MULTIPLAYER, self._linux_next, 'Set up')
        return w

    def _save_linux_settings(self, proton, prefix):
        self.lc.settings['proton'] = proton
        self.lc.settings['prefix'] = prefix
        self.lc.settings['launch_mode'] = 'proton'
        self.lc.settings.save()

    def _linux_next(self):
        from .. import launch, linux_support, paths
        try:
            proton = launch.resolve_proton(self.proton_edit.text() or launch.find_proton())
            if not proton:
                raise ValueError('Proton was not found. Install Proton through Steam, or choose it under Change Proton settings.')
            prefix = os.path.abspath(os.path.expanduser(self.prefix_edit.text().strip() or os.path.join(paths.config_dir(), 'prefix')))
        except Exception as e:
            self.linux_banner.show_message(str(e), 'bad')
            return
        def work(job):
            return linux_support.prepare(self.lc.settings, proton, prefix, job.progress, lambda: job.cancelled)
        def ok(_):
            self._save_linux_settings(proton, prefix)
            self._account_or_done()
        self._operation(work, ok, self.linux_banner, self.linux_progress)

    def _refresh_acquire_controls(self):
        ready = bool(self.archive) and self.archive == self._validated_archive
        automatic = self.auto_az.isChecked()
        self.itch_login.setVisible(automatic and not self.itch_profile and not ready)
        self.itch_get.setVisible(automatic and bool(self.itch_profile) and not ready)
        self.acquire_next.setEnabled(ready and not self._busy_setup)

    def _account_or_done(self):
        if not self.lc.has_identity():
            self.stack.setCurrentIndex(self.ACCOUNT)
            return
        banner = self.linux_banner if self.stack.currentIndex() == self.LINUX else self.mod_banner
        def ok(_):
            self.stack.setCurrentIndex(self.DONE)
        def failed(error):
            if isinstance(error, ApiError) and error.code == 'unknown_account':
                self.existing_key = True
                self.stack.setCurrentIndex(self.ACCOUNT)
            else:
                banner.show_message(friendly(error), 'bad')
        self._operation(lambda job: self.lc.sign_in(), ok, banner, failed=failed)

    def _done_page(self):
        w, lay = self._page("You're all set!", 'Your launcher is ready. You can now play FH2MP.')
        lay.addStretch(1)
        _, self.done_finish = self._nav(lay, None, self._finish_setup, 'Finish')
        return w

    def _finish_setup(self):
        from .. import setup
        self.lc.settings['setup_revision'] = setup.REVISION
        self.lc.settings.save()
        self._close_butler()
        super().accept()

    def accept(self):
        # AccountWizard's registration/recovery flow ends here; setup still has a final page.
        if self.existing_key and self.lc.settings['setup_revision']:
            super().accept()
        else:
            self.stack.setCurrentIndex(self.DONE)

    def _operation(self, work, ok, banner, bar=None, failed=None):
        if self._busy_setup or self.lc.game_running():
            if self.lc.game_running():
                banner.show_message('Close the game before changing your installation.', 'warn')
            return
        self._busy_setup = True
        banner.hide()
        page = self.stack.currentWidget()
        controls = [(control, control.isEnabled()) for control in page.findChildren(QWidget)
                    if isinstance(control, (QPushButton, QLineEdit, QRadioButton))]
        for control, _enabled in controls:
            control.setEnabled(False)
        if bar:
            bar.setRange(0, 0)
            bar.show()
        def finished(result=None, error=None):
            self._busy_setup = False
            self._setup_job = None
            if self._closing:
                self._close_butler()
                QDialog.reject(self)
                return
            for control, enabled in controls:
                control.setEnabled(enabled)
            if bar:
                bar.hide()
            if error:
                if failed:
                    failed(error)
                else:
                    banner.show_message(friendly(error), 'bad')
            else:
                try:
                    ok(result)
                except Exception as e:
                    banner.show_message(friendly(e), 'bad')
            self._refresh_acquire_controls()
            self._schedule_size()
        def progress(got, total):
            if bar:
                bar.setRange(0, 100 if total else 0)
                bar.setValue(int(100 * got / total) if total else 0)
        self._setup_job = run_async(work, lambda r: finished(r), lambda e: finished(error=e), progress)

    def _close_butler(self):
        if self.butler:
            self.butler.close()
            self.butler = None

    def reject(self):
        if self._busy_setup:
            self._closing = True
            if self._setup_job:
                self._setup_job.cancelled = True
            return
        self._close_butler()
        super().reject()
