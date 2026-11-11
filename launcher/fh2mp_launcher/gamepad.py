"""Gamepad control of the launcher (user request 2026-11-11), with the same buttons as the in-game Guide:

  d-pad / left stick  move the focus to the nearest control in that direction (held: again after 400 ms, then every 80 ms)
  A                   press the focused control (a text field gets the on-screen keyboard)
  B                   back: Escape to the focused control (closes a dialog, panel or menu)
  X                   the focused row's context action (Menu key)
  Y                   friends panel
  LB / RB             previous / next tab, or the home page's previous / next cards
  Start               Play
  right stick         scroll

No new dependencies: XInput through ctypes on Windows, /dev/input event devices on Linux. The pad is only read while the launcher is the
active application and no game runs (in game the Guide owns the pad). FH2MP_NO_GAMEPAD=1 turns it off."""
import ctypes
import glob
import logging
import math
import os
import struct
import sys
import time
from dataclasses import dataclass, field

log = logging.getLogger("fh2mp.gamepad")

DIRECTIONS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}
REPEAT_DELAY_MS = 400
REPEAT_EVERY_MS = 80
STICK_MOVE = 0.55          # left stick past this counts as a direction
STICK_SCROLL = 0.25        # right stick past this scrolls
SCROLL_PX = 22             # per 16 ms tick at full deflection


@dataclass(frozen=True)
class PadState:
    """One reading: pressed buttons and the sticks (-1..1, x right, y up)."""
    buttons: frozenset = field(default_factory=frozenset)
    lx: float = 0.0
    ly: float = 0.0
    rx: float = 0.0
    ry: float = 0.0


IDLE = PadState()

# ---------------------------------------------------------------- Windows: XInput

XINPUT_MASKS = (("up", 0x0001), ("down", 0x0002), ("left", 0x0004), ("right", 0x0008), ("start", 0x0010), ("back", 0x0020),
                ("ls", 0x0040), ("rs", 0x0080), ("lb", 0x0100), ("rb", 0x0200), ("guide", 0x0400), ("a", 0x1000), ("b", 0x2000),
                ("x", 0x4000), ("y", 0x8000))


def _axis(v, dead=7849):
    """A thumbstick value (-32768..32767) to -1..1 with XInput's own dead zone."""
    if abs(v) <= dead:
        return 0.0
    return max(-1.0, min(1.0, (v - math.copysign(dead, v)) / (32767 - dead)))


def decode_xinput(buttons, lx, ly, rx, ry):
    return PadState(frozenset(name for name, m in XINPUT_MASKS if buttons & m), _axis(lx), _axis(ly), _axis(rx), _axis(ry))


class _XGamepad(ctypes.Structure):
    _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte), ("bRightTrigger", ctypes.c_ubyte),
                ("sThumbLX", ctypes.c_short), ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short), ("sThumbRY", ctypes.c_short)]


class _XState(ctypes.Structure):
    _fields_ = [("dwPacketNumber", ctypes.c_uint), ("Gamepad", _XGamepad)]


class XInputBackend:
    """Reads the first connected pad. Empty slots are slow to ask, so they are looked at only every 2 s."""

    def __init__(self):
        self.dll = None
        for name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
            try:
                self.dll = ctypes.WinDLL(name)
                break
            except OSError:
                continue
        if self.dll is None:
            raise OSError("no XInput DLL")
        self.get = self.dll.XInputGetState
        self.get.argtypes = [ctypes.c_uint, ctypes.POINTER(_XState)]
        self.get.restype = ctypes.c_uint
        self.slot, self.next_scan = None, 0.0

    def poll(self):
        st = _XState()
        if self.slot is not None:
            if self.get(self.slot, ctypes.byref(st)) == 0:
                g = st.Gamepad
                return decode_xinput(g.wButtons, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY)
            log.info("gamepad: the pad in XInput slot %d is gone", self.slot)
            self.slot = None
        now = time.monotonic()
        if now >= self.next_scan:
            self.next_scan = now + 2.0
            for i in range(4):
                if self.get(i, ctypes.byref(st)) == 0:
                    self.slot = i
                    log.info("gamepad: using the pad in XInput slot %d", i)
                    break
        return None

    def close(self):
        pass

# ---------------------------------------------------------------- Linux: evdev

EV_KEY, EV_ABS = 0x01, 0x03
EVDEV_KEYS = {0x130: "a", 0x131: "b", 0x133: "x", 0x134: "y", 0x136: "lb", 0x137: "rb", 0x13a: "back", 0x13b: "start", 0x13c: "guide",
              0x13d: "ls", 0x13e: "rs", 0x220: "up", 0x221: "down", 0x222: "left", 0x223: "right"}
ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y = 0x00, 0x01, 0x03, 0x04, 0x10, 0x11
EVENT = struct.Struct("llHHi")     # struct input_event on 64-bit Linux: timeval, type, code, value


def parse_events(data):
    """Whole input_event records in data -> [(type, code, value)]."""
    n = len(data) // EVENT.size
    return [EVENT.unpack_from(data, i * EVENT.size)[2:] for i in range(n)]


class EvdevState:
    """Folds evdev events into a PadState. ranges: {abs code: (min, max)} from EVIOCGABS; sticks default to -32768..32767."""

    def __init__(self, ranges=None):
        self.ranges = ranges or {}
        self.keys, self.axes, self.hat = set(), {}, [0, 0]

    def _norm(self, code, v):
        lo, hi = self.ranges.get(code, (-32768, 32767))
        if hi <= lo:
            return 0.0
        x = (2.0 * (v - lo) / (hi - lo)) - 1.0
        return 0.0 if abs(x) < 0.24 else max(-1.0, min(1.0, x))

    def apply(self, etype, code, value):
        if etype == EV_KEY and code in EVDEV_KEYS:
            (self.keys.add if value else self.keys.discard)(EVDEV_KEYS[code])
        elif etype == EV_ABS:
            if code == ABS_HAT0X:
                self.hat[0] = (value > 0) - (value < 0)
            elif code == ABS_HAT0Y:
                self.hat[1] = (value > 0) - (value < 0)
            elif code in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
                self.axes[code] = self._norm(code, value)

    def state(self):
        b = set(self.keys)
        b |= {"left"} if self.hat[0] < 0 else {"right"} if self.hat[0] > 0 else set()
        b |= {"up"} if self.hat[1] < 0 else {"down"} if self.hat[1] > 0 else set()
        a = self.axes
        return PadState(frozenset(b), a.get(ABS_X, 0.0), -a.get(ABS_Y, 0.0), a.get(ABS_RX, 0.0), -a.get(ABS_RY, 0.0))   # evdev y grows down


def _eviocgabs(code):
    return (2 << 30) | (24 << 16) | (ord("E") << 8) | (0x40 + code)


class EvdevBackend:
    """The first joystick event device (udev's /dev/input/by-id/*-event-joystick), read without blocking each tick."""

    def __init__(self):
        self.fd, self.st, self.next_scan = None, None, 0.0

    def _open(self):
        for path in sorted(glob.glob("/dev/input/by-id/*-event-joystick")):
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except OSError:
                continue
            ranges = {}
            try:
                import fcntl
                for code in (ABS_X, ABS_Y, ABS_RX, ABS_RY):
                    buf = bytearray(24)
                    fcntl.ioctl(fd, _eviocgabs(code), buf)
                    _value, lo, hi = struct.unpack_from("iii", buf)
                    ranges[code] = (lo, hi)
            except OSError:
                pass
            self.fd, self.st = fd, EvdevState(ranges)
            log.info("gamepad: using %s", path)
            return

    def poll(self):
        if self.fd is None:
            now = time.monotonic()
            if now < self.next_scan:
                return None
            self.next_scan = now + 2.0
            self._open()
            if self.fd is None:
                return None
        try:
            while True:
                data = os.read(self.fd, EVENT.size * 64)
                if not data:
                    break
                for ev in parse_events(data):
                    self.st.apply(*ev)
        except BlockingIOError:
            pass
        except OSError:
            log.info("gamepad: the pad is gone")
            self.close()
            return None
        return self.st.state()

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = None


def default_backend():
    try:
        if sys.platform == "win32":
            return XInputBackend()
        if sys.platform.startswith("linux"):
            return EvdevBackend()
    except OSError as e:
        log.info("gamepad: no pad input (%s)", e)
    return None

# ---------------------------------------------------------------- input -> actions


def held_direction(state):
    """The direction the pad holds now: d-pad first, else the left stick's stronger axis past STICK_MOVE."""
    for d in ("up", "down", "left", "right"):
        if d in state.buttons:
            return d
    if max(abs(state.lx), abs(state.ly)) < STICK_MOVE:
        return None
    if abs(state.lx) >= abs(state.ly):
        return "right" if state.lx > 0 else "left"
    return "up" if state.ly > 0 else "down"


class Repeater:
    """Turns readings into actions: each button once per press, a held direction again after REPEAT_DELAY_MS then every REPEAT_EVERY_MS."""
    BUTTONS = ("a", "b", "x", "y", "lb", "rb", "start", "back")

    def __init__(self):
        self.prev = IDLE
        self.dir, self.next_at = None, 0

    def feed(self, state, now_ms):
        out = [b for b in self.BUTTONS if b in state.buttons and b not in self.prev.buttons]
        d = held_direction(state)
        if d != self.dir:
            self.dir = d
            if d is not None:
                out.append(d)
                self.next_at = now_ms + REPEAT_DELAY_MS
        elif d is not None and now_ms >= self.next_at:
            out.append(d)
            self.next_at = now_ms + REPEAT_EVERY_MS
        self.prev = state
        return out


def pick_neighbour(cur, candidates, direction):
    """The candidate rect nearest to cur in direction. Rects are (x, y, w, h); candidates [(key, rect)]. Only candidates whose centre lies
    beyond cur's centre in that direction count; the score is the gap along the direction plus three times the sideways gap (0 when the
    two overlap sideways), then the centre distance."""
    dx, dy = DIRECTIONS[direction]
    cx, cy = cur[0] + cur[2] / 2, cur[1] + cur[3] / 2
    best, best_score = None, None
    for key, r in candidates:
        rx, ry = r[0] + r[2] / 2, r[1] + r[3] / 2
        if dx and (rx - cx) * dx <= 1:
            continue
        if dy and (ry - cy) * dy <= 1:
            continue
        if dx:
            gap = max(0.0, (r[0] - (cur[0] + cur[2])) if dx > 0 else (cur[0] - (r[0] + r[2])))
            side = max(0.0, max(r[1], cur[1]) - min(r[1] + r[3], cur[1] + cur[3]))
        else:
            gap = max(0.0, (r[1] - (cur[1] + cur[3])) if dy > 0 else (cur[1] - (r[1] + r[3])))
            side = max(0.0, max(r[0], cur[0]) - min(r[0] + r[2], cur[0] + cur[2]))
        score = (gap + 3 * side, math.hypot(rx - cx, ry - cy))
        if best_score is None or score < best_score:
            best, best_score = key, score
    return best

# ---------------------------------------------------------------- Qt side

try:
    from PySide6.QtCore import QEvent, QObject, QPoint, QRect, Qt, QTimer, Signal
    from PySide6.QtGui import QColor, QGuiApplication, QKeyEvent, QPainter, QPen
    from PySide6.QtWidgets import (QAbstractButton, QAbstractItemView, QAbstractScrollArea, QAbstractSlider, QAbstractSpinBox, QApplication,
                                   QComboBox, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit, QMenu, QPlainTextEdit, QPushButton,
                                   QScrollArea, QScrollBar, QTabBar, QTabWidget, QTextEdit, QVBoxLayout, QWidget)
except ImportError:          # the pure parts above stay importable without Qt
    QObject = object
    Signal = None


if Signal is not None:
    from .ui import theme

    def _accent():
        return QColor(getattr(theme, "ACCENT", "#c9a0f7"))

    class FocusRing(QWidget):
        """A rounded outline over the focused control, shown while the pad is in use (custom-painted controls draw no focus of their own)."""

        def __init__(self):
            super().__init__(None)
            self.target = None
            self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
            self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        def follow(self, w):
            if w is None or not w.isVisible():
                self.target = None
                self.hide()
                return
            win = w.window()
            if self.parent() is not win:
                self.setParent(win)
            self.target = w
            top_left = w.mapTo(win, QPoint(0, 0))
            self.setGeometry(QRect(top_left, w.size()).adjusted(-3, -3, 3, 3))
            self.raise_()
            self.show()

        def paintEvent(self, _e):
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            p.setPen(QPen(_accent(), 2))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), 9, 9)

    KEY_ROWS = ("1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_@.")

    class PadKeyboard(QFrame):
        """The on-screen keyboard (360 style, design canvas artboard 6) for a text field: A types the focused key, X backspace, Y space,
        LB/RB move the cursor, Start or Done closes it, B closes it too. The field keeps what was typed."""

        def __init__(self, target):
            win = target.window()
            super().__init__(win)
            self.target = target
            self.caps = False
            self.setProperty("padScope", True)
            self.setObjectName("padKeyboard")
            self.setStyleSheet(
                f"QFrame#padKeyboard {{background:{theme.MANTLE};border:1px solid {theme.BORDER};border-radius:14px;}}"
                f"QPushButton {{background:{theme.SURFACE0};color:{theme.TEXT};border:2px solid transparent;border-radius:8px;"
                f"font-size:17px;min-width:44px;min-height:40px;padding:0 8px;}}"
                f"QPushButton:focus {{border-color:{theme.ACCENT};background:{theme.SURFACE1};}}"
                f"QLabel {{color:{theme.MUTED};font-size:13px;}}")
            lay = QVBoxLayout(self)
            lay.setContentsMargins(16, 14, 16, 16)
            lay.setSpacing(8)
            self.preview = QLabel()
            lay.addWidget(self.preview)
            grid = QGridLayout()
            grid.setSpacing(6)
            self.letter_keys = []
            for r, row in enumerate(KEY_ROWS):
                for c, ch in enumerate(row):
                    b = QPushButton(ch)
                    b.clicked.connect(lambda _=False, b=b: self.type(b.text()))
                    grid.addWidget(b, r, c)
                    self.letter_keys.append(b)
            lay.addLayout(grid)
            bottom = QHBoxLayout()
            bottom.setSpacing(6)
            for text, fn in (("Caps", self.toggle_caps), ("←", lambda: self.cursor(-1)), ("Space", lambda: self.type(" ")),
                             ("→", lambda: self.cursor(1)), ("Backspace", self.backspace), ("Done", self.done)):
                b = QPushButton(text)
                b.clicked.connect(fn)
                bottom.addWidget(b, 2 if text == "Space" else 1)
            lay.addLayout(bottom)
            self.adjustSize()
            self._place()
            self._refresh()
            self.show()
            self.raise_()
            self.letter_keys[10].setFocus()

        def _place(self):
            win = self.window()
            w = min(620, win.width() - 32)
            self.resize(w, self.sizeHint().height())
            self.move((win.width() - w) // 2, max(16, win.height() - self.height() - 24))

        def _text(self):
            t = self.target
            return t.text() if isinstance(t, QLineEdit) else t.toPlainText()

        def _refresh(self):
            shown = self._text()
            if isinstance(self.target, QLineEdit) and self.target.echoMode() != QLineEdit.EchoMode.Normal:
                shown = "•" * len(shown)
            self.preview.setText((shown[-60:] or " ") + "   (X backspace · Y space · LB/RB move · Start done)")

        def type(self, ch):
            if isinstance(self.target, QLineEdit):
                self.target.insert(ch)
            else:
                self.target.insertPlainText(ch)
            self._refresh()

        def backspace(self):
            if isinstance(self.target, QLineEdit):
                self.target.backspace()
            else:
                self.target.textCursor().deletePreviousChar()
            self._refresh()

        def cursor(self, step):
            t = self.target
            if isinstance(t, QLineEdit):
                t.setCursorPosition(max(0, min(len(t.text()), t.cursorPosition() + step)))
            else:
                c = t.textCursor()
                c.movePosition(c.MoveOperation.Right if step > 0 else c.MoveOperation.Left)
                t.setTextCursor(c)

        def toggle_caps(self):
            self.caps = not self.caps
            for b in self.letter_keys:
                b.setText(b.text().upper() if self.caps else b.text().lower())

        def done(self):
            target = self.target
            self.hide()
            self.deleteLater()
            if target is not None:
                target.setFocus()

        def pad_action(self, name):
            """Shortcuts while the keyboard is open; True when handled here."""
            if name == "x":
                self.backspace()
            elif name == "y":
                self.type(" ")
            elif name in ("lb", "rb"):
                self.cursor(-1 if name == "lb" else 1)
            elif name in ("start", "b"):
                self.done()
            else:
                return False
            return True

    def _editable(w):
        if isinstance(w, QLineEdit):
            return not w.isReadOnly()
        if isinstance(w, (QPlainTextEdit, QTextEdit)):
            return not w.isReadOnly()
        return False

    class Navigator(QObject):
        """Reads the pad every 16 ms while the launcher is active and turns it into focus moves and key presses (module docstring)."""
        play = Signal()
        friends = Signal()
        shoulder = Signal(int)

        def __init__(self, app, backend=None, suppressed=lambda: False, home=None):
            super().__init__(app)
            self.app, self.backend, self.suppressed, self.home = app, backend, suppressed, home
            self.rep = Repeater()
            self.ring = FocusRing()
            self.pad_mode = False
            self._sending = False
            self.timer = QTimer(self)
            self.timer.setInterval(16)
            self.timer.timeout.connect(self.tick)
            app.applicationStateChanged.connect(self._state_changed)
            app.focusChanged.connect(lambda _old, new: self.ring.follow(new) if self.pad_mode else None)
            app.installEventFilter(self)
            self._state_changed(QGuiApplication.applicationState())

        def _state_changed(self, st):
            if self.backend is not None and st == Qt.ApplicationState.ApplicationActive:
                self.timer.start()
            else:
                self.timer.stop()
                self.rep = Repeater()

        def eventFilter(self, obj, ev):
            if self.pad_mode and not self._sending and ev.type() in (QEvent.Type.MouseMove, QEvent.Type.MouseButtonPress, QEvent.Type.KeyPress):
                self.pad_mode = False
                self.ring.follow(None)
            return False

        def tick(self):
            if self.suppressed():
                self.rep = Repeater()
                return
            state = self.backend.poll() if self.backend is not None else None
            if state is not None:
                self.feed(state, int(time.monotonic() * 1000))

        def feed(self, state, now_ms):
            for name in self.rep.feed(state, now_ms):
                self.act(name)
            if abs(state.ry) >= STICK_SCROLL:
                self.scroll(-state.ry)
            if self.pad_mode and self.ring.target is not None:
                self.ring.follow(self.ring.target)          # follow scrolling and animations

        # -- where the focus may go

        def scope(self):
            """The window or panel the pad works in: a popup or modal first, else the active window's top visible pad scope (on-screen
            keyboard, panels marked padScope), else the window."""
            for top in (QApplication.activePopupWidget(), QApplication.activeModalWidget()):
                if top is not None:
                    return top
            win = QApplication.activeWindow()
            if win is None:
                return None
            scopes = [w for w in win.findChildren(QWidget) if w.property("padScope") and w.isVisible()]
            return scopes[-1] if scopes else win

        def candidates(self, scope):
            out = []
            for w in scope.findChildren(QWidget):
                if (w.isVisible() and w.isEnabled() and w.focusPolicy() & Qt.FocusPolicy.TabFocus and not isinstance(w, QScrollBar)
                        and not w.visibleRegion().isEmpty() and w is not self.ring):
                    out.append(w)
            return out

        @staticmethod
        def _rect(w):
            p = w.mapToGlobal(QPoint(0, 0))
            return (p.x(), p.y(), w.width(), w.height())

        def _focus(self, w):
            w.setFocus(Qt.FocusReason.TabFocusReason)
            area = w.parentWidget()
            while area is not None and not isinstance(area, QScrollArea):
                area = area.parentWidget()
            if area is not None:
                area.ensureWidgetVisible(w, 24, 24)
            self.ring.follow(w)

        def _key(self, target, key, mods=Qt.KeyboardModifier.NoModifier):
            self._sending = True
            try:
                QApplication.sendEvent(target, QKeyEvent(QEvent.Type.KeyPress, key, mods))
                QApplication.sendEvent(target, QKeyEvent(QEvent.Type.KeyRelease, key, mods))
            finally:
                self._sending = False

        # -- actions

        def act(self, name):
            scope = self.scope()
            if scope is None:
                return
            first_use = not self.pad_mode
            self.pad_mode = True
            focus = QApplication.focusWidget()
            if focus is not None and not (scope is focus or scope.isAncestorOf(focus)):
                focus = None
            if first_use and focus is not None and name in DIRECTIONS:
                self.ring.follow(focus)                                 # the first touch shows where the focus is; the next one moves it
                return
            if hasattr(scope, "pad_action") and scope.pad_action(name):
                return
            popup = QApplication.activePopupWidget()
            if popup is not None:                                       # menus and combo lists take keys themselves
                keys = {"up": Qt.Key.Key_Up, "down": Qt.Key.Key_Down, "left": Qt.Key.Key_Left, "right": Qt.Key.Key_Right,
                        "a": Qt.Key.Key_Return, "b": Qt.Key.Key_Escape}
                if name in keys:
                    self._key(QApplication.focusWidget() or popup, keys[name])
                return
            if focus is None:
                if name in DIRECTIONS or name == "a" or first_use:
                    home = self.home if self.home is not None and self.home.isVisible() and scope.isAncestorOf(self.home) else None
                    pool = self.candidates(scope)
                    if home is None and pool:
                        home = min(pool, key=lambda w: (self._rect(w)[1], self._rect(w)[0]))
                    if home is not None:
                        self._focus(home)
                    return
            if name in DIRECTIONS:
                self.move(focus, name, scope)
            elif name == "a":
                self.press(focus)
            elif name == "b":
                self._key(focus or scope, Qt.Key.Key_Escape)
            elif name == "x" and focus is not None:
                self._key(focus, Qt.Key.Key_Menu)
            elif name == "y":
                self.friends.emit()
            elif name == "start":
                self.play.emit()
            elif name in ("lb", "rb"):
                step = -1 if name == "lb" else 1
                tabs = focus
                while tabs is not None and not isinstance(tabs, QTabWidget):
                    tabs = tabs.parentWidget()
                if tabs is not None and tabs.count():
                    tabs.setCurrentIndex((tabs.currentIndex() + step) % tabs.count())
                else:
                    self.shoulder.emit(step)
            elif first_use and focus is not None:
                self.ring.follow(focus)

        def move(self, focus, direction, scope):
            if focus is None:
                return
            vertical = direction in ("up", "down")
            keys = {"up": Qt.Key.Key_Up, "down": Qt.Key.Key_Down, "left": Qt.Key.Key_Left, "right": Qt.Key.Key_Right}
            inside = None
            if isinstance(focus, QAbstractItemView) and vertical:
                inside = focus.currentIndex()
            elif isinstance(focus, QTabBar) and not vertical:
                inside = focus.currentIndex()
            elif isinstance(focus, QAbstractSlider) and not isinstance(focus, QScrollBar) and not vertical:
                inside = focus.value()
            elif isinstance(focus, QAbstractSpinBox) and vertical:
                inside = focus.text()
            if inside is not None:
                self._key(focus, keys[direction])
                now = (focus.currentIndex() if isinstance(focus, (QAbstractItemView, QTabBar)) else
                       focus.value() if isinstance(focus, QAbstractSlider) else focus.text())
                if now != inside:
                    self.ring.follow(focus)
                    return
            pool = [(w, self._rect(w)) for w in self.candidates(scope) if w is not focus]
            nxt = pick_neighbour(self._rect(focus), pool, direction)
            if nxt is not None:
                self._focus(nxt)
            else:
                self.ring.follow(focus)

        def press(self, focus):
            if focus is None:
                return
            if _editable(focus):
                PadKeyboard(focus)
            elif isinstance(focus, QComboBox):
                focus.showPopup()
            elif isinstance(focus, QAbstractButton):
                focus.animateClick()
            elif isinstance(focus, QAbstractItemView):
                self._key(focus, Qt.Key.Key_Return)
            else:
                self._key(focus, Qt.Key.Key_Return)

        def scroll(self, amount):
            focus = QApplication.focusWidget()
            area = focus
            while area is not None and not isinstance(area, QAbstractScrollArea):
                area = area.parentWidget()
            if area is None:
                return
            bar = area.verticalScrollBar()
            bar.setValue(bar.value() - int(amount * SCROLL_PX))

    def install(app, window, home=None, suppressed=lambda: False, backend=None):
        """Starts pad control for the launcher; None when FH2MP_NO_GAMEPAD=1 or this system has no pad API. The caller connects the
        navigator's play / friends / shoulder signals to its window."""
        if os.environ.get("FH2MP_NO_GAMEPAD") == "1":
            log.info("gamepad: off (FH2MP_NO_GAMEPAD=1)")
            return None
        backend = backend if backend is not None else default_backend()
        if backend is None:
            return None
        nav = Navigator(app, backend, suppressed, home)
        window.destroyed.connect(lambda *_: nav.timer.stop())
        return nav
