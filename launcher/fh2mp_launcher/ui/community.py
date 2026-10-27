"""Native Qt friends panel, conversations and launcher lobby browser."""
import time

from PySide6.QtCore import QEvent, QObject, QPoint, QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QGuiApplication, QPainter, QPixmap
from PySide6.QtWidgets import (QApplication, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QListWidget,
                               QListWidgetItem, QRadioButton, QScrollArea, QSizePolicy,
                               QVBoxLayout, QWidget)
from shiboken6 import isValid

from ..core import friendly
from . import sfx, theme
from .widgets import FitLabel, HoverButton, Icon, IconButton, Tween, fade_in, font, hrule, menu
from .social_widgets import Button, MessageBubble, Overlay, PersonAvatar, Search
from .workers import run_async

MODES = {"freeroam": "Online Free Roam", "roadtrip": "Online Road Trip"}
STATES = {"playing": "In game", "online": "Online", "offline": "Offline"}


def label(text, name="", wrap=False):
    w = QLabel(text)
    w.setTextFormat(Qt.TextFormat.PlainText)
    w.setWordWrap(wrap)
    if name:
        w.setObjectName(name)
    return w


def clear(layout):
    while layout.count():
        item = layout.takeAt(0)
        if item.widget():
            item.widget().hide()
            item.widget().deleteLater()
        elif item.layout():
            clear(item.layout())


def scroll(parent_layout, colour=None):
    area = QScrollArea()
    area.setWidgetResizable(True)
    area.setFrameShape(QFrame.Shape.NoFrame)
    colour = colour or theme.BG
    area.setStyleSheet(f"QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {{background:{colour};border:0;}}")
    area.viewport().setObjectName("clear")
    body = QWidget()
    body.setObjectName("clear")
    layout = QVBoxLayout(body)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(6)
    area.setWidget(body)
    parent_layout.addWidget(area, 1)
    return layout


class CommunityController(QObject):
    changed = Signal()
    lobbies_changed = Signal()
    message = Signal(str)
    avatar_changed = Signal(object)

    def __init__(self, lc, window):
        super().__init__(window)
        self.lc, self.window = lc, window
        self.data, self.lobby_data = {}, {}
        self.error = ""
        self._loading = False
        self._lobby_loading = False
        self._dialogs = {}
        self.pictures, self._picture_times, self._picture_pending = {}, {}, set()
        self._picture_versions = {}
        self._picture_context = None
        self._social_epoch = 0
        self.notifications = sfx.SocialNotifications()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(5_000)
        self.clock = QTimer(self)
        self.clock.timeout.connect(self._countdown)
        self.clock.start(1000)

    def _countdown(self):
        if self.data.get("reservation"):
            self.lobbies_changed.emit()

    @property
    def hold(self):
        h = self.data.get("reservation")
        return h if h and h["expiresAt"] > time.time() else None

    def remaining(self):
        sec = max(0, int((self.hold or {}).get("expiresAt", 0) - time.time()))
        return f"{sec // 60}:{sec % 60:02}"

    def available(self):
        return self.lc.has_identity() and not self.lc.settings["offline_mode"] and not getattr(self.lc, "offline_run", False)

    def sound_context(self):
        s = self.lc.settings
        return s['api_base'], s['channel'], s['xuid']

    def avatar(self, xuid, version=None):
        if not isinstance(xuid, int) or xuid <= 0:
            return None
        context = self.sound_context()[:2]
        if context != self._picture_context:
            self._picture_context = context
            self.pictures.clear()
            self._picture_times.clear()
            self._picture_pending.clear()
            self._picture_versions.clear()
        if version is not None:
            if xuid in self._picture_versions and self._picture_versions[xuid] != version:
                self._picture_times.pop(xuid, None)
            self._picture_versions[xuid] = version
        if not self.available() or xuid in self._picture_pending or time.monotonic() - self._picture_times.get(xuid, -1000) < 60:
            return self.pictures.get(xuid)
        self._picture_pending.add(xuid)
        self._picture_times[xuid] = time.monotonic()
        requested_version = self._picture_versions.get(xuid)
        def loaded(data):
            if context != self._picture_context:
                return
            self._picture_pending.discard(xuid)
            if requested_version != self._picture_versions.get(xuid):
                self._picture_times.pop(xuid, None)
                self.avatar(xuid)
                return
            picture = QPixmap()
            self.pictures[xuid] = picture if picture.loadFromData(data) else None
            self.avatar_changed.emit(xuid)
        def failed(_):
            if context == self._picture_context:
                self._picture_pending.discard(xuid)
        run_async(lambda j: self.lc.api().gamerpic(xuid), loaded, failed)
        return self.pictures.get(xuid)

    def refresh(self):
        if not self.available():
            self.data = {}
            self.error = "Set up an account to use Friends." if not self.lc.has_identity() else "Friends are unavailable in offline mode."
            self.changed.emit()
            return
        if self._loading or self.window._busy:
            return
        self._loading = True
        epoch = self._social_epoch

        def done(data):
            self._loading = False
            if epoch != self._social_epoch:
                self.refresh()
                return
            if not self.available():
                self.refresh()
                return
            self.notifications.snapshot(self.sound_context(), data)
            self.data, self.error = data, ""
            for p in data.get('friends', []) + data.get('incoming', []):
                self.avatar(p['xuid'], p.get('gamerpicVersion'))
            self.changed.emit()
            self.lobbies_changed.emit()

        def failed(e):
            self._loading = False
            self.error = friendly(e)
            # Never present stale online status as current after a failed heartbeat.
            self.data = {}
            self.changed.emit()
        run_async(lambda j: self.lc.community(), done, failed)

    def action(self, path, method="POST", body=None, done=None, owner=None):
        def ok(data):
            self._social_epoch += 1
            if method == 'POST' and path.startswith('/friends/') and path.endswith('/accept'):
                self.notifications.accepted(self.sound_context(), int(path.split('/')[2]))
            if "reservation" in data:
                self.data["reservation"] = data["reservation"]
            elif path == "/reservation" and method == "DELETE":
                self.data["reservation"] = None
            self.refresh()
            self.lobbies_changed.emit()
            if done and (owner is None or isValid(owner)):
                done(data)

        def fail(e):
            if owner is not None and isValid(owner) and hasattr(owner, "failed"):
                owner.failed(e)
            else:
                self.message.emit(friendly(e))
        run_async(lambda j: self.lc.community(path, method, body), ok, fail)

    def load_lobbies(self):
        if self._lobby_loading:
            return
        self._lobby_loading = True

        def ok(data):
            self._lobby_loading = False
            self.lobby_data = data
            self.data["reservation"] = data.get("reservation")
            self.lobbies_changed.emit()

        def fail(e):
            self._lobby_loading = False
            self.lobby_data = {"error": friendly(e), "lobbies": []}
            self.lobbies_changed.emit()
        run_async(lambda j: self.lc.community("/lobbies"), ok, fail)

    def show_dialog(self, name, factory):
        old = self._dialogs.get(name)
        if old and isValid(old):
            old.show()
            old.raise_()
            old.activateWindow()
            return old
        dlg = factory()
        self._dialogs[name] = dlg
        dlg.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        dlg.show()
        return dlg

    def friends_dialog(self):
        return self.show_dialog("add", lambda: AddFriendDialog(self, self.window))

    def chats(self, ref=None):
        dlg = self.show_dialog("chats", lambda: ChatsDialog(self, self.window))
        if ref is not None:
            dlg.select(str(ref))
        return dlg

    def lobbies(self):
        return self.show_dialog("lobbies", lambda: LobbiesDialog(self, self.window))

    def reserve(self, row):
        return self.show_dialog("reserve", lambda: ReservationDialog(self, row, self.window))

    def remove_friend(self, profile):
        return self.show_dialog('remove-friend', lambda: RemoveFriendDialog(self, dict(profile), self.window))


class RequestRow(QFrame):
    def __init__(self, c, person):
        super().__init__()
        self.c, self.person = c, person
        self.setFixedHeight(52)
        self.setObjectName('clear')
        row = QHBoxLayout(self)
        row.setContentsMargins(8, 0, 8, 0)
        row.setSpacing(6)
        row.addWidget(PersonAvatar(person, controller=c))
        name = label(person['gamertag'])
        name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        row.addWidget(name, 1)
        self.accept = Button(icon='check', fn=lambda: self.respond(True), height=30)
        self.reject = Button(icon='close', fn=lambda: self.respond(False), height=30)
        self.accept.setAccessibleName('Accept friend request from ' + person['gamertag'])
        self.reject.setAccessibleName('Decline friend request from ' + person['gamertag'])
        self.accept.tone = 'held'
        for b in (self.accept, self.reject):
            b.setFixedWidth(30)
            row.addWidget(b)

    def respond(self, accept):
        self.accept.setEnabled(False)
        self.reject.setEnabled(False)
        path = f"/friends/{self.person['xuid']}/" + ('accept' if accept else 'request')
        def done(_):
            self.c.data['incoming'] = [p for p in self.c.data.get('incoming', []) if p['xuid'] != self.person['xuid']]
            self.c.changed.emit()
        self.c.action(path, 'POST' if accept else 'DELETE', done=done, owner=self)

    def failed(self, error):
        self.accept.setEnabled(True)
        self.reject.setEnabled(True)
        self.setToolTip(friendly(error))


class FriendsPanel(QFrame):
    def __init__(self, controller):
        super().__init__()
        self.c, self.opened = controller, True
        self.setFixedWidth(320)
        self.inner = QWidget(self)
        self.inner.setFixedWidth(320)
        self.inner.setObjectName('friendsInner')
        self.inner.setStyleSheet(f'QWidget#friendsInner {{background:{theme.MANTLE};}}')
        lay = QVBoxLayout(self.inner)
        lay.setContentsMargins(10, 14, 10, 18)
        lay.setSpacing(0)
        top = QHBoxLayout()
        top.setContentsMargins(8, 0, 8, 14)
        top.setSpacing(8)
        self.add = Button(icon='person-add', fn=controller.friends_dialog)
        self.add.setFixedWidth(40)
        self.add.setAccessibleName('Add friend and manage requests')
        top.addWidget(self.add)
        search = Search('Search friends')
        self.search = search.edit
        self.search.textChanged.connect(self.render)
        top.addWidget(search, 1)
        lay.addLayout(top)
        self.rows = scroll(lay, theme.MANTLE)
        self.empty = label('', 'small', True)
        self.empty.setParent(self.rows.parentWidget())
        self.empty.setContentsMargins(8, 12, 8, 0)
        self.empty.hide()
        self._people, self._headings, self._requests = {}, {}, {}
        foot = QHBoxLayout()
        foot.setContentsMargins(8, 14, 8, 0)
        self.chat_button = Button('CHATS & GROUPS', lambda: controller.chats(), icon='chat', height=44)
        self.chat_button.font_px, self.chat_button.text_spacing = 13, .1
        foot.addWidget(self.chat_button)
        lay.addLayout(foot)
        self.motion = Tween(self, ms=260, value=320.0, on_change=lambda v: self.setFixedWidth(round(v)))
        self.motion.anim.finished.connect(lambda: self.hide() if not self.opened else None)
        self._signature = None
        controller.changed.connect(self.render)
        self.render()

    def toggle(self):
        self.opened = not self.opened
        self.show()
        self.motion.to(320 if self.opened else 0)

    def resizeEvent(self, event):
        self.inner.setGeometry(self.width() - 320, 0, 320, self.height())
        super().resizeEvent(event)

    def render(self):
        c, needle = self.c, self.search.text().strip().casefold()
        friends = c.data.get('friends', [])
        self.add.setToolTip('Add friend' + (f" · {len(c.data['incoming'])} requests" if c.data.get('incoming') else ''))
        self.add.badge = len(c.data.get('incoming', []))
        self.add.update()
        self.chat_button.setEnabled(c.available())
        unread = sum(ch.get('unread', 0) for ch in c.data.get('chats', []))
        self.chat_button.setText('CHATS & GROUPS')
        self.chat_button.badge = unread
        self.chat_button.update()
        signature = repr((friends, needle, c.error, c.data.get('incoming'), c.data.get('chats')))
        if signature == self._signature:
            return
        self._signature = signature
        # Keep rows alive across presence polls: hover state and focus stay put.
        while self.rows.count():
            item = self.rows.takeAt(0)
            if item.widget(): item.widget().hide()
        for x in set(self._people) - {f['xuid'] for f in friends}:
            self._people.pop(x).deleteLater()
        incoming = c.data.get('incoming', [])
        for x in set(self._requests) - {p['xuid'] for p in incoming}:
            self._requests.pop(x).deleteLater()
        if incoming:
            heading = self._headings.get('requests')
            if heading is None:
                heading = label('', 'small')
                heading.setFont(font(12, 700, spacing=.08))
                heading.setFixedHeight(28)
                heading.setContentsMargins(8, 0, 0, 0)
                self._headings['requests'] = heading
            heading.setText(f'FRIEND REQUESTS — {len(incoming)}')
            self.rows.addWidget(heading)
            heading.show()
            for person in incoming:
                row = self._requests.get(person['xuid'])
                if row is None:
                    row = RequestRow(c, person)
                    self._requests[person['xuid']] = row
                self.rows.addWidget(row)
                row.show()
        for state, title in (('playing', 'PLAYING FH2MP'), ('online', 'ONLINE'), ('offline', 'OFFLINE')):
            group = [f for f in friends if f['state'] == state and needle in f['gamertag'].casefold()]
            if not group: continue
            heading = self._headings.get(state)
            if heading is None:
                heading = label('')
                heading.setFont(font(12, 700, spacing=.08))
                heading.setFixedHeight(28)
                heading.setStyleSheet(f'color:{theme.MUTED};background:transparent;padding-left:8px;')
                self._headings[state] = heading
            heading.setText(f'⌄  {title} — {len(group)}')
            self.rows.addWidget(heading)
            heading.show()
            for f in sorted(group, key=lambda p: p['gamertag'].casefold()):
                row = self._people.get(f['xuid'])
                if row is None:
                    row = FriendRow(c, f)
                    self._people[f['xuid']] = row
                else:
                    row.set_profile(f)
                row.set_unread(next((ch for ch in c.data.get('chats', []) if ch['id'] == str(f['xuid'])), {}))
                self.rows.addWidget(row)
                row.show()
        if not incoming and not any(not w.isHidden() for w in self._people.values()):
            self.empty.setText(c.error or ('No friends match your search.' if needle else 'Add a friend to get started.'))
            self.rows.addWidget(self.empty)
            self.empty.show()
        self.rows.addStretch(1)


class FriendRow(QFrame):
    def __init__(self, controller, profile):
        super().__init__()
        self.c, self.profile, self.popup = controller, profile, None
        self.setFixedHeight(52)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.hover = Tween(self, ms=150)
        row = QHBoxLayout(self)
        row.setContentsMargins(8, 0, 8, 0)
        row.setSpacing(8)
        self.avatar = PersonAvatar(profile, controller=controller)
        row.addWidget(self.avatar)
        text = QVBoxLayout()
        text.setSpacing(2)
        text.setContentsMargins(0, 0, 0, 0)
        self.name, self.status = label(''), label('')
        self.name.setFont(font(15, 500))
        self.status.setFont(font(12))
        text.addWidget(self.name)
        text.addWidget(self.status)
        row.addLayout(text, 1)
        self.unread = 0
        self.unread_badge = label('')
        self.unread_badge.setFixedHeight(20)
        self.unread_badge.setStyleSheet(f'background:{theme.ACCENT};color:{theme.CRUST};border-radius:10px;padding:0 6px;font-size:12px;font-weight:700;')
        self.unread_badge.hide()
        row.addWidget(self.unread_badge)
        self.wait = QTimer(self)
        self.wait.setSingleShot(True)
        self.wait.timeout.connect(self.show_profile)
        self.set_profile(profile)

    def set_profile(self, profile):
        self.profile = profile
        self.avatar.profile = profile
        self.avatar.update()
        offline = profile['state'] == 'offline'
        self.name.setText(profile['gamertag'])
        self.name.setStyleSheet(f'color:{theme.MUTED if offline else theme.WHITE};background:transparent;')
        self.status.setText(STATES[profile['state']])
        self.status.setStyleSheet(f'color:{theme.MUTED};background:transparent;')
        self.setAccessibleName(profile['gamertag'] + ', ' + self.status.text())

    def set_unread(self, chat):
        self.unread = chat.get('unread', 0)
        self.unread_badge.setVisible(bool(self.unread))
        self.unread_badge.setText(str(self.unread))
        self.name.setFont(font(15, 700 if self.unread else 500))
        self.status.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        if self.unread:
            self.status.setText('New message · ' + chat.get('last', ''))
            self.status.setStyleSheet(f'color:{theme.ACCENT};background:transparent;')
        self.setToolTip(f"{self.unread} unread messages" if self.unread else self.profile['gamertag'])

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(255, 255, 255, round(18 * self.hover.v)))
        p.drawRoundedRect(QRectF(self.rect()), 10, 10)

    def enterEvent(self, event):
        self.hover.to(1)
        self.wait.start(350)
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.hover.to(0)
        self.wait.stop()
        super().leaveEvent(event)

    def mousePressEvent(self, event):
        if event.button() != Qt.MouseButton.LeftButton:
            return super().mousePressEvent(event)
        if self.popup and isValid(self.popup):
            self.popup.close()
        self.wait.stop()
        self.c.chats(self.profile['xuid'])
        super().mousePressEvent(event)

    def keyPressEvent(self, event):
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Space): self.c.chats(self.profile['xuid'])
        else: super().keyPressEvent(event)

    def show_profile(self):
        if self.popup and isValid(self.popup): return
        self.popup = ProfilePopup(self.c, self.profile, self)
        self.popup.show()


class ProfilePopup(QFrame):
    def __init__(self, controller, profile, row):
        super().__init__(controller.window.centralWidget())
        self.c, self.row = controller, row
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.setObjectName('friendProfile')
        self.setStyleSheet(f'QFrame#friendProfile {{background:{theme.BG};border:1px solid #3a3e5c;border-radius:14px;}}')
        self.setFixedWidth(300)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(18, 18, 18, 18)
        lay.setSpacing(12)
        top = QHBoxLayout()
        top.setSpacing(10)
        top.addWidget(PersonAvatar(profile, 56, controller=controller))
        identity = QVBoxLayout()
        identity.setSpacing(2)
        name = label(profile['gamertag'])
        name.setFont(font(18, 700))
        identity.addWidget(name)
        club, lobby = profile.get('club'), profile.get('lobby')
        if club:
            tag = label(f"[{club['tag']}]")
            tag.setStyleSheet(f'color:{theme.PEACH};font-weight:700;')
            identity.addWidget(tag)
        status = label(STATES[profile['state']])
        status.setStyleSheet(f'color:{theme.GOOD if profile["state"] != "offline" else theme.MUTED};font-size:13px;')
        identity.addWidget(status)
        top.addLayout(identity, 1)
        lay.addLayout(top)
        lay.addWidget(hrule())
        fields = (('Club role', {1:'Member',2:'Co-leader',3:'Leader'}.get(club['role'], 'Member') if club else '—'),
                  ('Session', 'Lobby ' + lobby['id'][:8] if lobby else '—'))
        for key, value in fields:
            line = QHBoxLayout()
            line.addWidget(label(key, 'muted'), 1)
            line.addWidget(label(value))
            lay.addLayout(line)
        actions = QHBoxLayout()
        actions.setSpacing(8)
        actions.addWidget(Button('Message', lambda: (self.close(), controller.chats(profile['xuid'])), True), 1)
        reserve = Button('Reserve', lambda: (self.close(), controller.reserve(lobby)))
        reserve.setEnabled(bool(lobby))
        actions.addWidget(reserve, 1)
        more = IconButton('more', 40, 10, 18, label_text='Friend options')
        options = menu(controller.window)
        self.destroyed.connect(options.deleteLater)
        options.addAction('Copy player ID', lambda: QGuiApplication.clipboard().setText(f"{profile['xuid']:#x}"))
        options.addAction('Remove friend', lambda: self._remove(controller, profile))
        more.set_menu(options)
        actions.addWidget(more)
        lay.addLayout(actions)
        self.adjustSize()
        if isinstance(row, FriendRow):
            point = row.mapTo(self.parentWidget(), QPoint(-316, 0))
        else:
            point = QPoint(self.parentWidget().width() - 640, 200)
        self.move(max(12, point.x()), min(max(12, point.y()), self.parentWidget().height() - self.height() - 12))
        self.dismiss = QTimer(self)
        self.dismiss.timeout.connect(self._dismiss)
        self.dismiss.start(160)

    def showEvent(self, event):
        self.raise_()
        fade_in(self, 140)
        super().showEvent(event)

    def _dismiss(self):
        if QApplication.activePopupWidget(): return
        if not self.underMouse() and (not isValid(self.row) or not self.row.underMouse()): self.close()

    def pin(self):
        self.dismiss.stop()
        QApplication.instance().installEventFilter(self)

    def eventFilter(self, target, event):
        if event.type() == QEvent.Type.MouseButtonPress and hasattr(event, 'globalPosition'):
            inside = self.rect().contains(self.mapFromGlobal(event.globalPosition().toPoint()))
            if not inside and not QApplication.activePopupWidget():
                self.close()
        return super().eventFilter(target, event)

    def closeEvent(self, event):
        self.dismiss.stop()
        QApplication.instance().removeEventFilter(self)
        super().closeEvent(event)

    def _remove(self, c, p):
        self.dismiss.stop()
        self.close()
        c.remove_friend(p)


class SocialDialog(Overlay):
    pass


class RemoveFriendDialog(SocialDialog):
    def __init__(self, c, person, parent):
        super().__init__(c, 'Remove friend', 400, parent)
        self.person = person
        self.lay.addWidget(label(f"Remove {person['gamertag']} from your friends?", wrap=True))
        self.lay.addWidget(self.error)
        row = QHBoxLayout()
        row.addStretch(1)
        row.addWidget(Button('Cancel', self.close))
        self.confirm = Button('Remove friend', self.remove, True)
        row.addWidget(self.confirm)
        self.lay.addLayout(row)

    def remove(self):
        self.confirm.setEnabled(False)
        def done(_):
            x = self.person['xuid']
            self.c.data['friends'] = [p for p in self.c.data.get('friends', []) if p['xuid'] != x]
            self.c.data['chats'] = [ch for ch in self.c.data.get('chats', []) if ch['id'] != str(x)]
            self.c.changed.emit()
            self.close()
        self.c.action(f"/friends/{self.person['xuid']}", 'DELETE', done=done, owner=self)

    def failed(self, error):
        super().failed(error)
        self.confirm.setEnabled(True)


class AddFriendDialog(SocialDialog):
    def __init__(self, c, parent):
        super().__init__(c, 'Add friend', 460, parent)
        group = QVBoxLayout()
        group.setSpacing(8)
        group.addWidget(label('Gamertag or player ID'))
        self.player = QLineEdit()
        self.player.setFixedHeight(44)
        self.player.setMaxLength(80)
        self.player.setPlaceholderText('Gamertag or player ID')
        self.player.setStyleSheet('QLineEdit {font-size:15px;padding:0 14px;border-radius:12px;}')
        group.addWidget(self.player)
        hint = label('They get a request and appear in your list once they accept.', 'small', True)
        group.addWidget(hint)
        self.lay.addLayout(group)
        self.request_box = QWidget()
        request_layout = QVBoxLayout(self.request_box)
        request_layout.setContentsMargins(0, 0, 0, 0)
        request_layout.setSpacing(8)
        self.request_toggle = Button('', self.toggle_requests, height=32)
        request_layout.addWidget(self.request_toggle)
        self.request_area = QScrollArea()
        self.request_area.setWidgetResizable(True)
        self.request_area.setFrameShape(QFrame.Shape.NoFrame)
        self.request_area.setFixedHeight(132)
        requests_body = QWidget()
        self.requests = QVBoxLayout(requests_body)
        self.requests.setContentsMargins(0, 0, 0, 0)
        self.request_area.setWidget(requests_body)
        self.request_area.hide()
        request_layout.addWidget(self.request_area)
        self.lay.addWidget(self.request_box)
        self.lay.addWidget(self.error)
        foot = QHBoxLayout()
        foot.setSpacing(12)
        foot.addStretch(1)
        foot.addWidget(Button('Cancel', self.close, height=44))
        self.send = Button('Send request', self.submit, True, height=44)
        self.send.setEnabled(False)
        self.player.textChanged.connect(lambda t: self.send.setEnabled(bool(t.strip())))
        self.player.returnPressed.connect(self.submit)
        foot.addWidget(self.send)
        self.lay.addLayout(foot)
        c.changed.connect(self.render_requests)
        self.render_requests()

    def showEvent(self, event):
        super().showEvent(event)
        self.player.setFocus()

    def toggle_requests(self):
        self.request_area.setVisible(self.request_area.isHidden())
        self.place()

    def submit(self):
        if not self.player.text().strip() or not self.send.isEnabled(): return
        self.send.setEnabled(False)
        def sent(_):
            self.player.clear()
            self.error.setStyleSheet(f'color:{theme.GOOD};font-size:13px;')
            self.error.setText('Friend request sent.')
            self.error.show()
            self.place()
        self.c.action('/friends', body={'player': self.player.text().strip()}, done=sent, owner=self)

    def failed(self, error):
        super().failed(error)
        self.send.setEnabled(bool(self.player.text().strip()))

    def render_requests(self):
        clear(self.requests)
        outgoing = self.c.data.get('outgoing', [])
        self.request_box.setVisible(bool(outgoing))
        self.request_toggle.setText(f'Sent requests · {len(outgoing)}')
        for person in outgoing:
            row = QHBoxLayout()
            row.addWidget(label(person['gamertag']), 1)
            row.addWidget(Button('Cancel', lambda _=False, p=person: self.c.action(f"/friends/{p['xuid']}/request", 'DELETE', owner=self), height=32))
            self.requests.addLayout(row)
        if self.isVisible(): self.place()


class ReservationDialog(SocialDialog):
    def __init__(self, c, lobby, parent):
        super().__init__(c, 'Reserve a spot' if lobby else 'Request a lobby', 480, parent)
        self.lobby = lobby
        if lobby:
            card = QFrame()
            card.setObjectName('card')
            card.setStyleSheet(f'QFrame#card {{background:{theme.MANTLE};border:1px solid {theme.LINE};border-radius:12px;}}')
            row = QHBoxLayout(card)
            row.setContentsMargins(16, 14, 16, 14)
            text = QVBoxLayout()
            text.setSpacing(2)
            tag = label(MODES[lobby['kind']].upper(), 'small')
            tag.setFont(font(12, 700, spacing=.08))
            text.addWidget(tag)
            title = label('Lobby ' + lobby['id'][:8])
            title.setFont(font(17, 700))
            text.addWidget(title)
            text.addWidget(label(lobby['status'], 'small'))
            row.addLayout(text, 1)
            row.addWidget(occupancy(lobby))
            self.lay.addWidget(card)
            notice = QHBoxLayout()
            notice.setSpacing(14)
            clock = Icon('clock', theme.ACCENT, 22)
            clock.setFixedSize(44, 44)
            clock.setStyleSheet('background:#2e2a48;border-radius:12px;')
            notice.addWidget(clock, 0, Qt.AlignmentFlag.AlignTop)
            text = QVBoxLayout()
            text.setSpacing(6)
            title = label('We hold one slot for 5 minutes')
            title.setFont(font(16, 700))
            text.addWidget(title)
            text.addWidget(label('Press PLAY, then enter ' + MODES[lobby['kind']] + '. The slot is released if you miss the timer. One hold at a time.', 'muted', True))
            notice.addLayout(text, 1)
            self.lay.addLayout(notice)
        else:
            self.lay.addWidget(label("Pick a mode. A spot in the new lobby is held for you for 5 minutes.", 'muted', True))
            self.roam = QRadioButton('Online Free Roam\nOpen world exploration, pick any event.')
            self.trip = QRadioButton('Online Road Trip\nVote for destination, travel to events.')
            self.roam.setChecked(True)
            for choice in (self.roam, self.trip):
                choice.setMinimumHeight(66)
                choice.setStyleSheet(f'QRadioButton {{background:{theme.MANTLE};border:1px solid {theme.LINE};border-radius:12px;padding:4px 16px;}} QRadioButton:checked {{border-color:{theme.ACCENT};}}')
                self.lay.addWidget(choice)
        self.lay.addWidget(self.error)
        foot = QHBoxLayout()
        foot.setSpacing(12)
        foot.addStretch(1)
        foot.addWidget(Button('Cancel', self.close, height=44))
        self.submit = Button('Reserve for 5 min' if lobby else 'Request lobby', self.reserve, True, height=44)
        foot.addWidget(self.submit)
        self.lay.addLayout(foot)

    def reserve(self):
        self.submit.setEnabled(False)
        body = {'lobby': self.lobby['id']} if self.lobby else {'kind': 'freeroam' if self.roam.isChecked() else 'roadtrip'}
        self.c.action('/reservation', body=body, owner=self, done=lambda _: (self.close(), self.c.load_lobbies()))

    def failed(self, error):
        super().failed(error)
        self.submit.setEnabled(True)


def occupancy(lobby):
    full = lobby.get('occupied', lobby['players']) >= lobby['capacity']
    badge = label(f"{lobby['players']} / {lobby['capacity']}")
    badge.setFont(font(12, 700))
    badge.setFixedHeight(24)
    badge.setStyleSheet(f'background:{theme.BLUE if full else theme.GOOD};color:#10231a;border-radius:10px;padding:0 10px;')
    badge.setToolTip(f"{lobby.get('held', 0)} launcher seats held")
    return badge


class LobbiesDialog(SocialDialog):
    def __init__(self, c, parent):
        super().__init__(c, 'Online lobbies', 900, parent)
        self.lay.setContentsMargins(24, 20, 24, 24)
        self.lay.setSpacing(10)
        top = QHBoxLayout()
        top.setSpacing(14)
        tag = label('ONLINE LOBBIES', 'small')
        tag.setFont(font(12, 700, spacing=.08))
        top.addWidget(tag)
        self.hold_box = QFrame()
        self.hold_box.setStyleSheet(f'QFrame {{background:#2e2a48;border:1px solid {theme.ACCENT};border-radius:10px;}} QLabel {{border:0;background:transparent;}}')
        hold_row = QHBoxLayout(self.hold_box)
        hold_row.setContentsMargins(10, 0, 6, 0)
        hold_row.setSpacing(8)
        self.hold_box.setFixedHeight(32)
        hold_row.addWidget(Icon('clock', theme.ACCENT, 16))
        self.hold_label = label('')
        self.hold_label.setFont(font(13))
        hold_row.addWidget(self.hold_label)
        self.cancel = Button('Cancel', lambda: c.action('/reservation', 'DELETE', done=lambda _: c.load_lobbies(), owner=self), height=24)
        hold_row.addWidget(self.cancel)
        top.addWidget(self.hold_box)
        top.addStretch(1)
        top.addWidget(Button('Request new lobby', lambda: c.show_dialog('request', lambda: ReservationDialog(c, None, parent)), icon='plus', height=32))
        self.lay.addLayout(top)
        self.table = QWidget()
        self.grid = QGridLayout(self.table)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setSpacing(14)
        self.lay.addWidget(self.table, 1)
        self.lay.addWidget(self.error)
        self.timer = QTimer(self)
        self.timer.timeout.connect(c.load_lobbies)
        self.timer.start(10_000)
        self.finished.connect(lambda _: self.timer.stop())
        c.lobbies_changed.connect(self.render)
        self._signature = None
        c.load_lobbies()
        self.render()

    def render(self):
        hold = self.c.hold
        self.hold_box.setVisible(bool(hold))
        self.hold_label.setText(f"Spot held · {hold['id'][:8]} · {self.c.remaining()}" if hold else '')
        data = self.c.lobby_data
        signature = repr(data.get('lobbies')) + str((hold or {}).get('id')) + str(data.get('error'))
        if signature == self._signature: return
        self._signature = signature
        clear(self.grid)
        self.error.setText(data.get('error', ''))
        self.error.setVisible(bool(data.get('error')))
        max_rows = 1
        for column, (kind, title) in enumerate(MODES.items()):
            group = [r for r in data.get('lobbies', []) if r['kind'] == kind]
            max_rows = max(max_rows, len(group))
            section = QFrame()
            section.setObjectName('lobbySection')
            section.setStyleSheet(f'QFrame#lobbySection {{background:{theme.MANTLE};border:1px solid {theme.LINE};border-radius:12px;}}')
            lay = QVBoxLayout(section)
            lay.setContentsMargins(0, 0, 0, 0)
            lay.setSpacing(0)
            header = QFrame()
            header.setStyleSheet('background:#2a2e4a;border-top-left-radius:12px;border-top-right-radius:12px;')
            header.setFixedHeight(44)
            row = QHBoxLayout(header)
            row.setContentsMargins(16, 0, 16, 0)
            text = label(title)
            text.setFont(font(16, 700))
            row.addWidget(text, 1)
            count = label(f'{len(group)} active')
            count.setStyleSheet('background:#8ea8f5;color:#14182b;border-radius:10px;padding:3px 10px;font-size:12px;font-weight:700;')
            count.setFixedHeight(24)
            row.addWidget(count)
            lay.addWidget(header)
            area = QScrollArea()
            area.setWidgetResizable(True)
            area.setFrameShape(QFrame.Shape.NoFrame)
            area.setStyleSheet(f'QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {{background:{theme.MANTLE};border:0;}}')
            rows = QWidget()
            contents = QVBoxLayout(rows)
            contents.setContentsMargins(0, 0, 0, 0)
            contents.setSpacing(0)
            for lobby in group:
                frame = QWidget()
                frame.setObjectName('clear')
                frame.setFixedHeight(58)
                line = QHBoxLayout(frame)
                line.setContentsMargins(16, 0, 16, 0)
                line.setSpacing(8)
                summary = QVBoxLayout()
                summary.setSpacing(0)
                name = label('Lobby ' + lobby['id'][:8])
                name.setFont(font(14))
                name.setToolTip(name.text())
                name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
                summary.addWidget(name)
                state = lobby['status'] + (f" · Event {lobby['event']} of {lobby['events']}" if lobby.get('event') else '')
                subtitle = label(state, 'small')
                subtitle.setToolTip(state)
                subtitle.setFixedHeight(16)
                subtitle.setMinimumWidth(0)
                subtitle.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
                summary.addWidget(subtitle)
                line.addLayout(summary, 1)
                line.addWidget(occupancy(lobby))
                players = IconButton('friends', 34, 9, 16, label_text='Show players')
                players.clicked.connect(lambda _=False, r=lobby: self.show_players(r))
                line.addWidget(players)
                mine = hold and hold['id'] == lobby['id']
                full = lobby.get('occupied', lobby['players']) >= lobby['capacity']
                action = Button('Spot held' if mine else 'Full' if full else 'Reserve spot', lambda _=False, r=lobby: self.c.reserve(r), not mine and not full, height=34)
                action.setFixedWidth(122)
                action.setEnabled(not mine and not full)
                if mine: action.tone = 'held'
                line.addWidget(action)
                contents.addWidget(frame)
                if lobby is not group[-1]: contents.addWidget(hrule())
            if not group:
                empty = label('No active lobbies', 'small')
                empty.setContentsMargins(16, 12, 16, 12)
                contents.addWidget(empty)
            contents.addStretch(1)
            area.setWidget(rows)
            area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
            lay.addWidget(area, 1)
            self.grid.addWidget(section, 0, column)
            self.grid.setColumnStretch(column, 1)
        table_height = 46 + min(max_rows, 5) * 59
        self.table.setMaximumHeight(table_height)
        # Scroll the rows when the host is short. A fixed table height used to
        # extend below the panel after its initial size hint had been cached.
        margins = self.lay.contentsMargins()
        header = self.lay.itemAt(0).sizeHint().height()
        height = 56 + margins.top() + margins.bottom() + header + self.lay.spacing() + table_height
        if not self.error.isHidden():
            height += self.error.sizeHint().height() + self.lay.spacing()
        self.set_panel_size(900, height)
        self.place()

    def show_players(self, lobby):
        dlg = SocialDialog(self.c, 'Players · Lobby ' + lobby['id'][:8], 400, self)
        for person in lobby.get('members', []):
            row = QHBoxLayout()
            row.addWidget(PersonAvatar(person, controller=self.c))
            row.addWidget(label(person['gamertag']), 1)
            if person['xuid'] != self.c.lc.settings['xuid'] and not any(f['xuid'] == person['xuid'] for f in self.c.data.get('friends', [])):
                row.addWidget(Button('Add friend', lambda _=False, p=person: self.c.action('/friends', body={'player':str(p['xuid'])}, owner=dlg), height=32))
            dlg.lay.addLayout(row)
        if not lobby.get('members'): dlg.lay.addWidget(label('No players connected yet.', 'muted'))
        dlg.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        dlg.show()


class ChatItem(HoverButton):
    def __init__(self, channel, profile, clicked, controller):
        super().__init__()
        self.channel, self.profile = channel, profile
        self.selected = False
        self.setFixedHeight(60)
        self.clicked.connect(clicked)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        row = QHBoxLayout(self)
        row.setContentsMargins(8, 0, 8, 0)
        row.setSpacing(10)
        if channel['id'] == 'club':
            mark = Icon('shield', theme.PEACH, 22)
            mark.setFixedSize(38, 38)
            mark.setStyleSheet('background:#3b2f3a;border-radius:12px;')
        else:
            mark = PersonAvatar(profile, 36, controller=controller)
        mark.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        row.addWidget(mark)
        text = QVBoxLayout()
        text.setSpacing(2)
        name = FitLabel(channel['title'])
        name.setFont(font(15, 500))
        name.setStyleSheet('background:transparent;color:white;')
        preview = FitLabel(channel.get('last') or 'No messages yet')
        preview.setFont(font(13))
        preview.setStyleSheet(f'background:transparent;color:{theme.MUTED};')
        for w in (name, preview): w.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        for w in (name, preview):
            w.setWordWrap(False)
            w.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        text.addWidget(name)
        text.addWidget(preview)
        row.addLayout(text, 1)
        if channel.get('unread'):
            unread = label(str(channel['unread']))
            unread.setStyleSheet(f'background:{theme.ACCENT};color:{theme.CRUST};border-radius:8px;padding:2px 5px;font-size:11px;')
            unread.setFixedHeight(18)
            row.addWidget(unread)

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        colour = QColor('#2c3050') if self.selected else QColor(255, 255, 255, round(16 * self.h.v))
        p.setBrush(colour)
        p.drawRoundedRect(QRectF(self.rect()), 10, 10)


class ChatsDialog(SocialDialog):
    def __init__(self, c, parent):
        super().__init__(c, 'Chats', 1000, parent)
        self.set_panel_size(1000, 620)
        self.lay.setContentsMargins(0, 0, 0, 0)
        self.lay.setSpacing(0)
        self.ref, self.last_id, self._loading = None, 0, False
        self._generation = 0
        self._messages, self._rendered_ids = [], []
        self._read_ids, self._drafts = {}, {}
        self._sending = False
        self._read_inflight = {}
        self._new_count = 0
        self._channel_signature = None
        body = QHBoxLayout()
        body.setSpacing(0)
        left = QFrame()
        left.setObjectName('chatSidebar')
        left.setFixedWidth(290)
        left.setStyleSheet(f'QFrame#chatSidebar {{background:{theme.MANTLE};border-right:1px solid {theme.LINE};border-bottom-left-radius:16px;}}')
        sidebar = QVBoxLayout(left)
        sidebar.setContentsMargins(10, 16, 10, 12)
        sidebar.setSpacing(12)
        search_row = QHBoxLayout()
        search_row.setContentsMargins(6, 0, 6, 0)
        search = Search('Search chats')
        self.search = search.edit
        self.search.textChanged.connect(self.render_channels)
        search_row.addWidget(search)
        sidebar.addLayout(search_row)
        self.channels = QListWidget()
        self.channels.setStyleSheet(f'QListWidget {{background:{theme.MANTLE};border:0;padding:0;outline:0;}} QListWidget::item {{background:transparent;border:0;padding:0;}} QListWidget::item:selected {{background:transparent;}}')
        self.channels.currentItemChanged.connect(self._selected)
        sidebar.addWidget(self.channels, 1)
        body.addWidget(left)
        right = QWidget()
        right.setObjectName('clear')
        conversation = QVBoxLayout(right)
        conversation.setContentsMargins(0, 0, 0, 0)
        conversation.setSpacing(0)
        header = QFrame()
        header.setObjectName('chatHeader')
        header.setFixedHeight(64)
        header.setStyleSheet(f'QFrame#chatHeader {{background:transparent;border-bottom:1px solid {theme.LINE};}}')
        title_row = QHBoxLayout(header)
        title_row.setContentsMargins(20, 0, 20, 0)
        title_row.setSpacing(12)
        self.chat_mark = Icon('chat', theme.PEACH, 22)
        self.chat_mark.setFixedSize(38, 38)
        self.chat_mark.setStyleSheet('background:#3b2f3a;border-radius:12px;')
        title_row.addWidget(self.chat_mark)
        self.header_avatar = PersonAvatar({'xuid':0, 'gamertag':'?', 'state':'offline'}, 36, controller=c)
        self.header_avatar.hide()
        title_row.addWidget(self.header_avatar)
        titles = QVBoxLayout()
        titles.setSpacing(2)
        self.title = label('Select a conversation')
        self.title.setFont(font(16, 700))
        self.subtitle = label('Direct messages and club chat', 'small')
        self.subtitle.setFont(font(13))
        titles.addWidget(self.title)
        titles.addWidget(self.subtitle)
        title_row.addLayout(titles, 1)
        more = IconButton('more', 36, 10, 18, label_text='Chat options')
        self.options = menu(more)
        self.options.aboutToShow.connect(self.fill_options)
        more.set_menu(self.options)
        title_row.addWidget(more)
        conversation.addWidget(header)
        self.messages = QScrollArea()
        self.messages.setWidgetResizable(True)
        self.messages.setFrameShape(QFrame.Shape.NoFrame)
        self.messages.setStyleSheet(f'QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget {{background:{theme.BG};border:0;}}')
        message_body = QWidget()
        self.message_rows = QVBoxLayout(message_body)
        self.message_rows.setContentsMargins(20, 20, 20, 20)
        self.message_rows.setSpacing(14)
        self.message_rows.setAlignment(Qt.AlignmentFlag.AlignBottom)
        self.messages.setWidget(message_body)
        self.messages.viewport().installEventFilter(self)
        self.messages.verticalScrollBar().valueChanged.connect(self.scrolled)
        conversation.addWidget(self.messages, 1)
        self.new_messages = Button('', self.scroll_to_latest, height=30)
        self.new_messages.hide()
        conversation.addWidget(self.new_messages)
        compose = QHBoxLayout()
        compose.setContentsMargins(20, 0, 20, 18)
        compose.setSpacing(10)
        self.edit = QLineEdit()
        self.edit.setFixedHeight(44)
        self.edit.setMaxLength(2000)
        self.edit.setPlaceholderText('Write a message')
        self.edit.setStyleSheet(f'QLineEdit {{background:{theme.MANTLE};border:1px solid {theme.LINE};border-radius:12px;padding:0 14px;font-size:15px;}} QLineEdit:focus {{border-color:{theme.ACCENT};}}')
        self.edit.returnPressed.connect(self.send)
        compose.addWidget(self.edit, 1)
        self.send_button = Button(icon='send', fn=self.send, primary=True, height=44)
        self.send_button.setFixedWidth(44)
        self.send_button.setAccessibleName('Send message')
        compose.addWidget(self.send_button)
        conversation.addLayout(compose)
        self.error.setContentsMargins(20, 0, 20, 12)
        conversation.addWidget(self.error)
        body.addWidget(right, 1)
        self.lay.addLayout(body, 1)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.load)
        self.timer.start(2500)
        self.finished.connect(lambda _: self.timer.stop())
        c.changed.connect(self.render_channels)
        self.render_channels()

    def render_channels(self):
        channels = self.c.data.get('chats', [])
        needle = self.search.text().casefold()
        signature = repr(channels) + needle
        if signature == self._channel_signature: return
        self._channel_signature = signature
        self.channels.blockSignals(True)
        self.channels.clear()
        selected = None
        for club, heading in ((True, 'CLUB'), (False, 'DIRECT MESSAGES')):
            group = [ch for ch in channels if (ch['id'] == 'club') == club and needle in ch['title'].casefold()]
            if not group: continue
            title = QListWidgetItem(heading)
            title.setFont(font(12, 700, spacing=.08))
            title.setForeground(QColor(theme.MUTED))
            title.setFlags(Qt.ItemFlag.NoItemFlags)
            title.setSizeHint(QSize(0, 28))
            self.channels.addItem(title)
            for ch in group:
                item = QListWidgetItem()
                item.setSizeHint(QSize(0, 60))
                item.setData(Qt.ItemDataRole.UserRole, ch)
                self.channels.addItem(item)
                profile = next((f for f in self.c.data.get('friends', []) if str(f['xuid']) == ch['id']),
                               {'xuid':ch['id'], 'gamertag':ch['title'], 'state':'offline'})
                row = ChatItem(ch, profile, lambda _=False, ref=ch['id']: self.select(ref), self.c)
                row.selected = ch['id'] == self.ref
                self.channels.setItemWidget(item, row)
                if row.selected: selected = item
        if selected: self.channels.setCurrentItem(selected)
        self.channels.blockSignals(False)
        if self.ref and not any(ch['id'] == self.ref for ch in channels):
            self.ref, self.last_id = None, 0
            self._generation += 1
            self._messages, self._rendered_ids = [], []
            clear(self.message_rows)
            self.title.setText('Select a conversation')
        self.send_button.setEnabled(bool(self.ref) and not self._sending)
        self.edit.setEnabled(bool(self.ref))
        if self.ref is None:
            first_unread = next((ch['id'] for ch in channels if ch.get('unread')), None)
            if first_unread is not None:
                self.select(first_unread)
                return
            for i in range(self.channels.count()):
                if self.channels.item(i).data(Qt.ItemDataRole.UserRole):
                    self.channels.setCurrentRow(i)
                    break

    def select(self, ref):
        for i in range(self.channels.count()):
            ch = self.channels.item(i).data(Qt.ItemDataRole.UserRole)
            if ch and ch['id'] == ref:
                self.channels.setCurrentRow(i)
                return

    def _selected(self, item, _previous):
        if not item:
            return
        ch = item.data(Qt.ItemDataRole.UserRole)
        if not ch: return
        for i in range(self.channels.count()):
            row = self.channels.itemWidget(self.channels.item(i))
            if row:
                row.selected = row.channel['id'] == ch['id']
                row.update()
        if self.ref:
            self._drafts[self.ref] = self.edit.text()
        self.ref, self.last_id = ch['id'], 0
        self.edit.setText(self._drafts.get(self.ref, ''))
        self._messages, self._rendered_ids = [], []
        self._generation += 1
        self.title.setText(ch['title'])
        self.chat_mark.set('shield' if ch['id'] == 'club' else 'chat', theme.PEACH)
        self.chat_mark.setVisible(ch['id'] == 'club')
        self.header_avatar.setVisible(ch['id'] != 'club')
        if ch['id'] != 'club':
            self.header_avatar.profile = self.current_person()
            self.c.avatar(int(ch['id']))
            self.header_avatar.update()
        self._new_count = 0
        self.new_messages.hide()
        self.edit.setPlaceholderText('Message ' + ch['title'])
        club = (self.c.data.get('me') or {}).get('club')
        self.subtitle.setText(f"Club chat · {club['members']} members" if self.ref == 'club' and club else "Direct message")
        self.edit.setEnabled(True)
        self.send_button.setEnabled(not self._sending)
        clear(self.message_rows)
        self.error.hide()
        self.load()

    def load(self):
        if not self.ref or self._loading:
            return
        self._loading = True
        ref, generation = self.ref, self._generation

        def ok(data):
            if not isValid(self):
                return
            self._loading = False
            if generation != self._generation:
                return self.load()
            rows = data['messages']
            self.c.notifications.messages(self.c.sound_context(), ref, rows, self.c.lc.settings['xuid'])
            if rows:
                self._messages = (self._messages + rows)[-500:]
                self.last_id = rows[-1]['id']
                self.render_messages()
            elif not self._messages:
                clear(self.message_rows)
                self.message_rows.addWidget(label("No messages yet. Say hello!", "muted"))
            QTimer.singleShot(0, self.mark_visible_read)
            self.error.hide()

        def fail(e):
            if isValid(self):
                self._loading = False
                if generation == self._generation:
                    self.failed(e)
                else:
                    self.load()
        path = f"/chat/{ref}?after={self.last_id}"
        run_async(lambda j: self.c.lc.community(path), ok, fail)

    def render_messages(self):
        bar = self.messages.verticalScrollBar()
        bottom, old = bar.maximum() - bar.value() < 30, bar.value()
        ids = [m['id'] for m in self._messages]
        append = ids[:len(self._rendered_ids)] == self._rendered_ids
        if not append:
            clear(self.message_rows)
            self._rendered_ids = []
        elif not self._rendered_ids:
            clear(self.message_rows)
        me = self.c.lc.settings['xuid']
        for m in self._messages[len(self._rendered_ids):]:
            mine = m['sender'] == me
            group = QWidget()
            group.setStyleSheet('background:transparent;')
            stack = QVBoxLayout(group)
            stack.setContentsMargins(0, 0, 0, 0)
            stack.setSpacing(4)
            if not mine:
                name = label(m['gamertag'], 'small')
                name.setFont(font(12, 500))
                stack.addWidget(name)
            bubble = MessageBubble(m['body'], mine, self.bubble_width())
            stack.addWidget(bubble)
            stamp = label(time.strftime('%I:%M %p', time.localtime(m['created'])).lstrip('0'), 'small')
            if mine: stamp.setAlignment(Qt.AlignmentFlag.AlignRight)
            stack.addWidget(stamp)
            self.message_rows.addWidget(group, 0, Qt.AlignmentFlag.AlignRight if mine else Qt.AlignmentFlag.AlignLeft)
            if not bottom and not mine:
                self._new_count += 1
            if append and self._rendered_ids: fade_in(group, 180)
        self._rendered_ids = ids
        self.new_messages.setVisible(bool(self._new_count))
        self.new_messages.setText(f'{self._new_count} new messages ↓')
        QTimer.singleShot(0, lambda: bar.setValue(bar.maximum() if bottom else old) if isValid(bar) else None)

    def current_person(self):
        return next((p for p in self.c.data.get('friends', []) if str(p['xuid']) == self.ref),
                    {'xuid':int(self.ref or 0) if self.ref != 'club' else 0, 'gamertag':self.title.text(),
                     'state':'offline', 'club':None, 'lobby':None, 'level':1})

    def fill_options(self):
        self.options.clear()
        if self.ref and self.ref != 'club':
            person = dict(self.current_person())
            self.options.addAction('View profile', lambda: self.show_person(person))
            self.options.addAction('Copy player ID', lambda: QGuiApplication.clipboard().setText(f"{person['xuid']:#x}"))
            self.options.addAction('Remove friend', lambda: self.c.remove_friend(person))
        self.options.addAction('Close chats', self.close)

    def show_person(self, person):
        self.person_popup = ProfilePopup(self.c, person, self.header_avatar)
        self.person_popup.pin()
        self.person_popup.show()

    def bubble_width(self):
        return max(64, min(480, int((self.messages.viewport().width() - 40) * .7)))

    def eventFilter(self, target, event):
        if hasattr(self, 'messages') and target is self.messages.viewport() and event.type() == QEvent.Type.Resize:
            for bubble in self.messages.findChildren(MessageBubble):
                bubble.set_max_width(self.bubble_width())
        return super().eventFilter(target, event)

    def scroll_to_latest(self):
        self.messages.verticalScrollBar().setValue(self.messages.verticalScrollBar().maximum())
        self.mark_visible_read()

    def scrolled(self, _=None):
        if not hasattr(self, 'new_messages'):
            return
        bar = self.messages.verticalScrollBar()
        if bar.maximum() - bar.value() < 16:
            self._new_count = 0
            self.new_messages.hide()
            self.mark_visible_read()

    def mark_visible_read(self):
        if not isValid(self) or not self.isVisible() or self._closing or not self.window().isActiveWindow() or not self.ref:
            return
        panels = [w for w in self.host.findChildren(Overlay, options=Qt.FindChildOption.FindDirectChildrenOnly) if w.isVisible() and not w._closing]
        if panels and panels[-1] is not self:
            return
        bar = self.messages.verticalScrollBar()
        if bar.maximum() - bar.value() >= 16:
            return
        ref, last = self.ref, self.last_id
        if last <= max(self._read_ids.get(ref, 0), self._read_inflight.get(ref, 0)):
            return
        self._read_inflight[ref] = last
        def read(_):
            self._read_ids[ref] = last
            self._read_inflight.pop(ref, None)
            for ch in self.c.data.get('chats', []):
                if ch['id'] == ref:
                    ch['unread'] = 0
            self.c.changed.emit()
        self.c.action(f'/chat/{ref}', 'PATCH', {'lastId':last}, done=read, owner=self)

    def send(self):
        text = self.edit.text().strip()
        if not text or not self.ref or self._sending:
            return
        self._sending = True
        self.send_button.setEnabled(False)
        ref = self.ref

        def sent(_):
            self._sending = False
            if self._drafts.get(ref, '').strip() == text:
                self._drafts.pop(ref, None)
            self.send_button.setEnabled(bool(self.ref))
            if self.ref == ref and self.edit.text().strip() == text:
                self.edit.clear()
            self.load()
        self.c.action(f"/chat/{ref}", body={'text': text}, done=sent, owner=self)

    def failed(self, e):
        super().failed(e)
        self._read_inflight.clear()
        self._sending = False
        self.send_button.setEnabled(bool(self.ref))
