import math
import sys

from PySide6.QtCore import QEasingCurve, QEvent, QPoint, QPointF, QPropertyAnimation, QRectF, QSize, Qt, QTimer, QVariantAnimation, Signal
from PySide6.QtGui import (QBrush, QColor, QFont, QFontMetrics, QGuiApplication, QIcon, QKeySequence, QLinearGradient, QPainter, QPainterPath,
                           QPalette, QPen, QPixmap, QRadialGradient, QShortcut)
from PySide6.QtWidgets import (QAbstractButton, QApplication, QFrame, QGraphicsOpacityEffect, QGridLayout, QHBoxLayout, QLabel, QMenu, QPushButton,
                               QFileDialog, QSizePolicy, QTextBrowser, QVBoxLayout, QWidget)

from . import theme

# Qt's native file dialog on Linux goes through GTK, which on Wayland fails to attach to a parented Qt window
# (gdk_x11_window_get_xid assertion): the dialog never appears and the parent stays blocked. Qt's own dialog works everywhere.
_FD = QFileDialog.Option.DontUseNativeDialog if sys.platform.startswith("linux") else QFileDialog.Option(0)


def open_file(parent, title, start, filt):
    return QFileDialog.getOpenFileName(parent, title, start, filt, options=_FD)[0]


def save_file(parent, title, start, filt):
    return QFileDialog.getSaveFileName(parent, title, start, filt, options=_FD)[0]


def pick_dir(parent, title, start):
    return QFileDialog.getExistingDirectory(parent, title, start, _FD)

# Stroke icons on a 24-unit grid (QtSvg is not shipped): each is a list of ("line", x1, y1, ...) polylines, ("loop", ...) closed
# polylines, ("circle", cx, cy, r), ("dot", cx, cy) and ("cog", teeth, outer r, root r, hole r).
ICONS = {
    "pc": [("loop", 3, 4, 21, 4, 21, 16, 3, 16), ("line", 12, 16, 12, 21), ("line", 7, 21, 17, 21)],
    "globe": [("circle", 12, 12, 9), ("ellipse", 12, 12, 4.5, 9), ("line", 3, 12, 21, 12)],
    "arrow-right": [("line", 4, 12, 20, 12), ("line", 14, 6, 20, 12, 14, 18)],
    "arrow-left": [("line", 20, 12, 4, 12), ("line", 10, 6, 4, 12, 10, 18)],
    "gear": [("cog", 8, 10.2, 7.6, 3.2)],
    "down": [("line", 5, 9, 12, 16, 19, 9)],
    "up": [("line", 5, 15, 12, 8, 19, 15)],
    "left": [("line", 14.5, 5, 7.5, 12, 14.5, 19)],
    "right": [("line", 9.5, 5, 16.5, 12, 9.5, 19)],
    "external": [("line", 13.7, 4.3, 19.7, 4.3, 19.7, 10.3), ("line", 19.7, 4.3, 11.1, 12.9),
                 ("line", 17.1, 14.6, 17.1, 18, 15.4, 19.7, 6, 19.7, 4.3, 18, 4.3, 8.6, 6, 6.9, 9.4, 6.9)],
    "warn": [("loop", 12, 3.5, 21.5, 20, 2.5, 20), ("line", 12, 10, 12, 14.5), ("dot", 12, 17.3)],
    "info": [("circle", 12, 12, 9), ("line", 12, 11, 12, 16.5), ("dot", 12, 7.6)],
    "close": [("line", 6, 6, 18, 18), ("line", 18, 6, 6, 18)],
}


def _cog(teeth, outer, root):
    """A toothed wheel around (12, 12): flat-topped teeth, straight flanks, round between them."""
    path, step = QPainterPath(), 2 * math.pi / teeth

    def at(r, a):
        return QPointF(12 + r * math.cos(a), 12 + r * math.sin(a))
    for i in range(teeth):
        a = i * step - math.pi / 2
        pts = (at(root, a - step * 0.30), at(outer, a - step * 0.17), at(outer, a + step * 0.17), at(root, a + step * 0.30))
        if i == 0:
            path.moveTo(pts[0])
        else:
            path.lineTo(pts[0])
        for q in pts[1:]:
            path.lineTo(q)
        nxt = at(root, a + step * 0.70)
        mid = at(root / math.cos(step * 0.2), a + step * 0.5)           # a gentle arc to the next tooth
        path.quadTo(mid, nxt)
    path.closeSubpath()
    return path


def _cloud_off():
    # Google Material Icons, Apache-2.0 (assets/LICENSE-Material-Icons.txt).
    # https://github.com/google/material-design-icons/blob/master/src/file/cloud_off/materialicons/24px.svg
    path = QPainterPath()
    path.moveTo(19.35, 10.04)
    path.cubicTo(18.67, 6.59, 15.64, 4, 12, 4)
    path.cubicTo(10.52, 4, 9.15, 4.43, 7.99, 5.17)
    path.lineTo(9.45, 6.63)
    path.cubicTo(10.21, 6.23, 11.08, 6, 12, 6)
    path.cubicTo(15.04, 6, 17.5, 8.46, 17.5, 11.5)
    path.lineTo(17.5, 12)
    path.lineTo(19, 12)
    path.cubicTo(20.66, 12, 22, 13.34, 22, 15)
    path.cubicTo(22, 16.13, 21.36, 17.11, 20.44, 17.62)
    path.lineTo(21.89, 19.07)
    path.cubicTo(23.16, 18.16, 24, 16.68, 24, 15)
    path.cubicTo(24, 12.36, 21.95, 10.22, 19.35, 10.04)
    path.closeSubpath()
    path.moveTo(3, 5.27)
    path.lineTo(5.75, 8.01)
    path.cubicTo(2.56, 8.15, 0, 10.77, 0, 14)
    path.cubicTo(0, 17.31, 2.69, 20, 6, 20)
    path.lineTo(17.73, 20)
    path.lineTo(19.73, 22)
    path.lineTo(21, 20.73)
    path.lineTo(4.27, 4)
    path.lineTo(3, 5.27)
    path.closeSubpath()
    path.moveTo(7.73, 10)
    path.lineTo(15.73, 18)
    path.lineTo(6, 18)
    path.cubicTo(3.79, 18, 2, 16.21, 2, 14)
    path.cubicTo(2, 11.79, 3.79, 10, 6, 10)
    path.lineTo(7.73, 10)
    path.closeSubpath()
    return path


def paint_icon(p, name, rect, colour, width=1.8, angle=0.0):
    s = min(rect.width(), rect.height()) / 24.0
    ox, oy = rect.x() + (rect.width() - 24 * s) / 2, rect.y() + (rect.height() - 24 * s) / 2
    pen = QPen(QColor(colour), width * 24 / 22, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    p.save()
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.translate(ox, oy)
    p.scale(s, s)
    if angle:
        p.translate(12, 12)
        p.rotate(angle)
        p.translate(-12, -12)
    if name == "cloud_off":
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(colour))
        p.drawPath(_cloud_off())
        p.restore()
        return
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)
    for kind, *v in ICONS[name]:
        if kind == "circle":
            p.drawEllipse(QPointF(v[0], v[1]), v[2], v[2])
        elif kind == "ellipse":
            p.drawEllipse(QPointF(v[0], v[1]), v[2], v[3])
        elif kind == "cog":
            p.drawPath(_cog(v[0], v[1], v[2]))
            p.drawEllipse(QPointF(12, 12), v[3], v[3])
        elif kind == "dot":
            p.setBrush(QColor(colour))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawEllipse(QPointF(v[0], v[1]), 1.2, 1.2)
            p.setPen(pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
        else:
            path = QPainterPath(QPointF(v[0], v[1]))
            for i in range(2, len(v), 2):
                path.lineTo(QPointF(v[i], v[i + 1]))
            if kind == "loop":
                path.closeSubpath()
            p.drawPath(path)
    p.restore()


def icon(name, colour=theme.TEXT, px=20, width=1.8):
    """A QIcon for buttons, drawn at 2x so it stays sharp on scaled screens."""
    pm = QPixmap(px * 2, px * 2)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    paint_icon(p, name, QRectF(0, 0, px * 2, px * 2), colour, width)
    p.end()
    pm.setDevicePixelRatio(2)
    return QIcon(pm)


def mix(a, b, t):
    """Colour between a and b (t = 0 .. 1)."""
    a, b = QColor(a), QColor(b)
    t = max(0.0, min(1.0, t))
    return QColor.fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                           a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t)


def alpha(c, a):
    c = QColor(c)
    c.setAlphaF(max(0.0, min(1.0, a)))
    return c


class Tween:
    """A number that eases to a target (hover, press, page position) and repaints `w` as it moves."""

    def __init__(self, w, ms=170, value=0.0, curve=QEasingCurve.Type.OutCubic, on_change=None):
        self.w, self.v = w, value
        self.anim = QVariantAnimation(w)
        self.anim.setDuration(ms)
        self.anim.setEasingCurve(curve)
        self.anim.valueChanged.connect(self._set)
        self.on_change = on_change

    def _set(self, v):
        self.v = float(v)
        if self.on_change:
            self.on_change(self.v)
        self.w.update()

    def to(self, target):
        if self.anim.state() == QVariantAnimation.State.Running and self.anim.endValue() == float(target):
            return
        self.anim.stop()
        self.anim.setStartValue(self.v)
        self.anim.setEndValue(float(target))
        self.anim.start()


def fade_in(w, ms=220):
    """Fades a widget in (then drops the effect, which would otherwise render it off-screen on every paint)."""
    eff = QGraphicsOpacityEffect(w)
    eff.setOpacity(0.0)
    w.setGraphicsEffect(eff)
    a = QPropertyAnimation(eff, b"opacity", w)
    a.setDuration(ms)
    a.setStartValue(0.0)
    a.setEndValue(1.0)
    a.setEasingCurve(QEasingCurve.Type.OutCubic)
    a.finished.connect(lambda: w.setGraphicsEffect(None))
    a.start(QPropertyAnimation.DeletionPolicy.DeleteWhenStopped)


class Icon(QWidget):
    def __init__(self, name, colour=theme.TEXT, px=18, parent=None):
        super().__init__(parent)
        self.name, self.colour, self.angle = name, colour, 0.0
        self.setFixedSize(px, px)

    def set(self, name, colour):
        self.name, self.colour = name, colour
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        paint_icon(p, self.name, QRectF(self.rect()), self.colour, 1.9, self.angle)


def font(px, weight=QFont.Weight.Normal, italic=False, spacing=0.0):
    """Roboto at a pixel size; spacing is letter spacing in em, as the design gives it."""
    f = QFont("Roboto")
    f.setPixelSize(px)
    f.setWeight(weight)
    f.setItalic(italic)
    if spacing:
        f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, px * spacing)
    return f


def label(text, px=14, weight=QFont.Weight.Normal, colour=None, spacing=0.0, wrap=False):
    lb = QLabel(text)
    lb.setFont(font(px, weight, spacing=spacing))
    if colour:
        lb.setStyleSheet(f"color: {colour};")
    lb.setWordWrap(wrap)
    return lb


def link_colour():
    """Links in rich text (the detail view's markdown) take the application palette's colour, which the style sheet does not set."""
    app = QApplication.instance()
    pal = app.palette()
    for role in (QPalette.ColorRole.Link, QPalette.ColorRole.LinkVisited):
        pal.setColor(role, QColor(theme.ACCENT))
    app.setPalette(pal)


def menu(parent):
    """A drop-down with rounded corners (the window behind it is transparent)."""
    m = QMenu(parent)
    m.setWindowFlags(m.windowFlags() | Qt.WindowType.FramelessWindowHint | Qt.WindowType.NoDropShadowWindowHint)
    m.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
    return m


def popup(m, anchor, above=False, right=False):
    """Opens menu m under (or above) anchor, aligned to its left (or right) edge, fading in where the platform allows it."""
    m.ensurePolished()
    sz = m.sizeHint()
    pos = anchor.mapToGlobal(QPoint(anchor.width() - sz.width() if right else 0, -sz.height() - 6 if above else anchor.height() + 6))
    if QGuiApplication.platformName() != "wayland":          # Wayland has no per-window opacity
        m.setWindowOpacity(0.0)
        a = QPropertyAnimation(m, b"windowOpacity", m)
        a.setDuration(140)
        a.setStartValue(0.0)
        a.setEndValue(1.0)
        a.start(QPropertyAnimation.DeletionPolicy.DeleteWhenStopped)
    m.popup(pos)


class HoverButton(QAbstractButton):
    """A painted button with an eased hover (h) and press (d) value; subclasses draw it."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("clear")            # painted here: no style-sheet fill behind the round corners
        self.h, self.d = Tween(self), Tween(self, 90)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.pressed.connect(lambda: self.d.to(1))
        self.released.connect(lambda: self.d.to(0))
        self.menu, self.menu_above, self.menu_right = None, False, False
        self.clicked.connect(self._pop)

    def set_menu(self, m, above=False, right=False):
        self.menu, self.menu_above, self.menu_right = m, above, right

    def _pop(self):
        if self.menu:
            popup(self.menu, self, self.menu_above, self.menu_right)

    def enterEvent(self, e):
        self.h.to(1)
        super().enterEvent(e)

    def leaveEvent(self, e):
        self.h.to(0)
        super().leaveEvent(e)


class IconButton(HoverButton):
    """A rounded square (or round) button with one icon; spin turns the icon a tooth's width on hover (the settings cog)."""

    def __init__(self, name, size=44, radius=12, icon_px=22, spin=0, label_text="", parent=None):
        super().__init__(parent)
        self.name, self.radius, self.icon_px, self.spin = name, radius, icon_px, spin
        self.setFixedSize(size, size)
        self.setAccessibleName(label_text)
        self.setToolTip(label_text)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        bg = mix(mix(theme.SURFACE0, theme.SURFACE1, self.h.v), theme.MANTLE, self.d.v * 0.5)
        if not self.isEnabled():
            p.setOpacity(0.35)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(bg)
        p.drawRoundedRect(QRectF(self.rect()), self.radius, self.radius)
        r = QRectF((self.width() - self.icon_px) / 2, (self.height() - self.icon_px) / 2, self.icon_px, self.icon_px)
        paint_icon(p, self.name, r, mix(theme.TEXT, theme.WHITE, self.h.v), 1.7, self.spin * self.h.v)


class LinkButton(HoverButton):
    """Text with the open-in-browser arrow after it; brightens on hover."""

    def __init__(self, text, parent=None):
        super().__init__(parent)
        self.setText(text)
        self.setFont(font(16))
        fm = QFontMetrics(self.font())
        self.setFixedSize(fm.horizontalAdvance(text) + 6 + 14 + 4, 40)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        c = mix(theme.TEXT, theme.WHITE, self.h.v)
        p.setFont(self.font())
        p.setPen(c)
        fm = QFontMetrics(self.font())
        tw = fm.horizontalAdvance(self.text())
        p.drawText(QRectF(0, 0, tw + 2, self.height()), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, self.text())
        lift = 2 * self.h.v                                             # the arrow nudges up and right
        paint_icon(p, "external", QRectF(tw + 6 + lift, (self.height() - 14) / 2 - lift, 14, 14), c, 1.6)


class Avatar(QWidget):
    """A round picture; before one is loaded (or when there is none) a gradient disc with the first letter of the gamertag."""

    def __init__(self, size=112, parent=None):
        super().__init__(parent)
        self._size, self._pix, self._text = size, None, "?"
        self.setFixedSize(size, size)

    def set_png(self, data):
        pm = QPixmap()
        self._pix = pm if data and pm.loadFromData(data) else None
        self.update()

    def set_name(self, name):
        self._text = (name or "?")[:1].upper()
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        r = QRectF(1, 1, self._size - 2, self._size - 2)
        path = QPainterPath()
        path.addEllipse(r)
        p.setClipPath(path)
        if self._pix:
            dpr = self.devicePixelRatioF()
            pm = self._pix.scaled(int(self._size * dpr), int(self._size * dpr), Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                  Qt.TransformationMode.SmoothTransformation)
            p.drawPixmap(r, pm, QRectF(0, 0, pm.width(), pm.height()))
        else:
            g = QLinearGradient(0, 0, self._size, self._size)
            g.setColorAt(0, QColor(theme.PEACH))
            g.setColorAt(1, QColor(theme.GRAPE))
            p.fillPath(path, QBrush(g))
            f = QFont(self.font())
            f.setPixelSize(int(self._size * 0.46))
            f.setBold(True)
            p.setFont(f)
            p.setPen(QColor(theme.CRUST))
            p.drawText(r, Qt.AlignmentFlag.AlignCenter, self._text)
        p.setClipping(False)
        p.setPen(QColor(theme.BORDER))
        p.drawEllipse(r)


class FitLabel(QLabel):
    """Word-wrapped text that asks for one line's width: it stays on one line while there is room and wraps when there is not."""
    MAX = 520

    def __init__(self, text="", parent=None):
        super().__init__(text, parent)
        self.setWordWrap(True)

    def sizeHint(self):
        w = min(self.MAX, self.fontMetrics().horizontalAdvance(self.text()) + 4)
        return QSize(w, self.heightForWidth(w))

    def minimumSizeHint(self):
        return self.sizeHint()


class Banner(QFrame):
    """A one-message box: kind is info | warn | bad, with an icon and an optional action button. Hidden until shown; fades in when
    its message changes. name "banner" (dialogs, message of the day) or "pill" (the status next to PLAY); both look the same."""
    action = Signal()

    def __init__(self, parent=None, name="banner"):
        super().__init__(parent)
        self.setObjectName(name)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(14, 7, 8, 7)
        lay.setSpacing(10)
        self.icon = Icon("info", theme.PILL["info"][2], 18)
        self.label = FitLabel("")
        self.button = QPushButton("")
        self.button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.button.clicked.connect(self.action)
        self.extra = QHBoxLayout()            # room for a progress bar
        self.extra.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self.icon, 0, Qt.AlignmentFlag.AlignVCenter)
        lay.addWidget(self.label, 1)
        lay.addLayout(self.extra)
        lay.addWidget(self.button, 0, Qt.AlignmentFlag.AlignVCenter)
        self.setMinimumHeight(40)
        self._shown = None
        self.hide()

    def show_message(self, text, kind="info", button=None):
        kind = kind if kind in theme.PILL else "info"
        if self._shown == (text, kind, button) and self.isVisible():
            return
        self._shown = (text, kind, button)
        self.setProperty("kind", kind)
        self.style().unpolish(self)
        self.style().polish(self)
        for w in (self.label, self.button):
            w.style().unpolish(w)
            w.style().polish(w)
        self.icon.set("info" if kind == "info" else "warn", theme.PILL[kind][2])
        self.label.setText(text)
        self.button.setVisible(bool(button))
        self.button.setText(button or "")
        self.show()
        fade_in(self)

    def hideEvent(self, e):
        self._shown = None
        super().hideEvent(e)


def hrule():
    f = QFrame()
    f.setObjectName("rule")
    f.setFrameShape(QFrame.Shape.HLine)
    return f


def spacer():
    w = QWidget()
    w.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
    return w


class GradientText(QWidget):
    """Roboto Black Italic filled with the peach -> grape gradient of the wordmark, wrapped at max_width, sized to the ink."""

    def __init__(self, text, px=34, max_width=0, parent=None):
        super().__init__(parent)
        f = font(px, QFont.Weight.Black, italic=True)
        f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, -px * 0.02)
        fm = QFontMetrics(f)
        lines, cur = [], ""
        for word in text.split():
            trial = f"{cur} {word}".strip()
            if cur and max_width and fm.horizontalAdvance(trial) > max_width:
                lines.append(cur)
                cur = word
            else:
                cur = trial
        lines.append(cur)
        path = QPainterPath()
        for i, line in enumerate(lines):
            path.addText(0, i * px * 1.05, f, line)
        path.setFillRule(Qt.FillRule.WindingFill)             # the font's glyphs have overlapping contours: odd-even fill would cut holes
        br = path.boundingRect()
        self._path = path.translated(-br.left(), -br.top())
        self._w, self._h = int(br.width()) + 2, int(br.height()) + 2
        self.setFixedSize(self._w, self._h)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        g = QLinearGradient(0, 0, self._w, 0)
        g.setColorAt(0, QColor(theme.PEACH))
        g.setColorAt(1, QColor(theme.GRAPE))
        p.translate(1, 1)
        p.fillPath(self._path, QBrush(g))


class Brand(GradientText):
    """The FH2MP wordmark as on forza.scuffed.af."""

    def __init__(self, px=34, parent=None):
        super().__init__("FH2MP", px, parent=parent)


class OnlineCount(QWidget):
    """"57 online" with a softly pulsing green dot."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("clear")            # painted here: no style-sheet fill behind the round corners
        self.text = ""
        self.setFont(font(16))
        self.pulse = Tween(self, 1400, curve=QEasingCurve.Type.InOutSine)
        self.pulse.anim.setStartValue(0.0)
        self.pulse.anim.setEndValue(1.0)
        self.pulse.anim.setLoopCount(-1)

    def set_count(self, n):
        self.text = f"{n} online" if isinstance(n, int) else ""
        self.setVisible(isinstance(n, int))
        self.setFixedSize(QFontMetrics(self.font()).horizontalAdvance(self.text) + 22, 40)
        if self.text and self.pulse.anim.state() != QVariantAnimation.State.Running:
            self.pulse.anim.start()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        c = QPointF(4, self.height() / 2)
        ring = math.sin(self.pulse.v * math.pi)               # 0 -> 1 -> 0 over a loop
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(alpha(theme.GOOD, 0.35 * (1 - self.pulse.v)))
        p.drawEllipse(c, 4 + 5 * self.pulse.v, 4 + 5 * self.pulse.v)
        p.setBrush(mix(theme.GOOD, theme.WHITE, 0.15 * ring))
        p.drawEllipse(c, 4, 4)
        p.setFont(self.font())
        p.setPen(QColor(theme.GOOD))
        p.drawText(QRectF(16, 0, self.width() - 16, self.height()), Qt.AlignmentFlag.AlignVCenter, self.text)


class AccountButton(HoverButton):
    """Picture, gamertag and a status line; opens the account menu (the chevron turns while it is open)."""

    def __init__(self, parent=None):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(6, 4, 8, 4)
        lay.setSpacing(10)
        self.avatar = Avatar(36)
        self.name = label("", 15, QFont.Weight.Medium, theme.WHITE)
        self.state = label("", 12)
        col = QVBoxLayout()
        col.setSpacing(0)
        col.addWidget(self.name)
        col.addWidget(self.state)
        self.chev = Icon("down", theme.MUTED, 12)
        lay.addWidget(self.avatar)
        lay.addLayout(col)
        lay.addWidget(self.chev)
        for w in (self.avatar, self.name, self.state, self.chev):
            w.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.turn = Tween(self.chev, 200, on_change=lambda v: setattr(self.chev, "angle", 180 * v))
        self.setMinimumHeight(46)

    def set_menu(self, m, above=False, right=True):
        super().set_menu(m, above, right)
        m.aboutToShow.connect(lambda: self.turn.to(1))
        m.aboutToHide.connect(lambda: self.turn.to(0))

    def set(self, name, state, colour):
        self.name.setText(name)
        self.avatar.set_name(name)
        self.state.setText(state)
        self.state.setStyleSheet(f"color: {colour};")
        self._fit()

    def _fit(self):
        self.layout().invalidate()
        self.setFixedWidth(self.layout().sizeHint().width())

    def event(self, e):
        if e.type() == QEvent.Type.LayoutRequest:          # a label changed size (new text, or its font after polishing)
            self._fit()
        return super().event(e)

    def paintEvent(self, _e):
        if self.h.v <= 0.01:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(alpha(theme.SURFACE0, self.h.v * (1 - 0.3 * self.d.v)))
        p.drawRoundedRect(QRectF(self.rect()), 10, 10)


class _Zone(HoverButton):
    """One clickable half of PlaySplit; the split draws it."""

    def __init__(self, split):
        super().__init__(split)
        self.h.w = self.d.w = split
        self.pressed.connect(split.update)
        self.released.connect(split.update)

    def paintEvent(self, _e):
        pass


class PlaySplit(QWidget):
    """PLAY plus a small arrow for the other ways to start. Each half lights up on hover; a shine sweeps across PLAY now and then."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("clear")            # painted here: no style-sheet fill behind the round corners
        self.setFixedSize(176, 48)
        self.play = _Zone(self)
        self.play.setText("PLAY")
        self.play.setAccessibleName("Play")
        self.more = _Zone(self)
        self.more.setAccessibleName("More ways to start")
        self.play.setGeometry(0, 0, 131, 48)
        self.more.setGeometry(132, 0, 44, 48)
        self.turn = Tween(self, 200)
        self.shine = Tween(self, 1100, curve=QEasingCurve.Type.InOutQuad)
        self.shine_timer = QTimer(self)
        self.shine_timer.setInterval(7000)
        self.shine_timer.timeout.connect(self._sweep)
        self.font_ = font(18, QFont.Weight.Bold, spacing=0.2)

    def set_menu(self, m):
        self.more.set_menu(m, above=True)
        m.aboutToShow.connect(lambda: self.turn.to(1))
        m.aboutToHide.connect(lambda: self.turn.to(0))

    def set_state(self, text, enabled):
        if (self.play.text(), self.play.isEnabled()) == (text, enabled):
            return
        self.play.setText(text)
        self.play.setEnabled(enabled)
        self.play.setCursor(Qt.CursorShape.PointingHandCursor if enabled else Qt.CursorShape.ArrowCursor)
        if enabled:
            self.shine_timer.start()
            QTimer.singleShot(600, self._sweep)
        else:
            self.shine_timer.stop()
        self.update()

    def _sweep(self):
        if self.play.isEnabled() and self.isVisible():
            self.shine.anim.stop()
            self.shine.v = 0.0
            self.shine.to(1)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        shape = QPainterPath()
        shape.addRoundedRect(QRectF(self.rect()), 12, 12)
        p.setClipPath(shape)
        on = self.play.isEnabled()
        base, lit, ink = (theme.ACCENT, theme.ACCENT_HOVER, theme.CRUST) if on else (theme.SURFACE0, theme.SURFACE1, theme.OVERLAY0)
        for z in (self.play, self.more):
            hv = z.h.v if (z is self.more or on) else 0
            c = mix(mix(base, lit, hv), theme.ACCENT_PRESSED if on else theme.MANTLE, z.d.v * 0.8)
            p.fillRect(QRectF(z.geometry()).adjusted(0, 0, 1 if z is self.play else 0, 0), c)
        if on and 0 < self.shine.v < 1:                       # a soft diagonal band of light
            x = -70 + (self.play.width() + 140) * self.shine.v
            g = QLinearGradient(x - 40, 0, x + 40, 0)
            g.setColorAt(0, alpha(theme.WHITE, 0))
            g.setColorAt(0.5, alpha(theme.WHITE, 0.42))
            g.setColorAt(1, alpha(theme.WHITE, 0))
            p.save()
            p.setTransform(p.transform().shear(-0.35, 0), False)
            p.fillRect(QRectF(x - 40 + 10, 0, 80, self.height()), g)
            p.restore()
        p.setClipping(False)
        p.fillRect(QRectF(131, 10, 1, 28), alpha(theme.CRUST if on else theme.SURFACE1, 0.3 if on else 1))
        p.setFont(self.font_)
        p.setPen(QColor(ink))
        p.drawText(QRectF(self.play.geometry()).adjusted(4, 0, 0, 0), Qt.AlignmentFlag.AlignCenter, self.play.text())
        paint_icon(p, "up", QRectF(132 + 13, 15, 18, 18), theme.CRUST if on else theme.TEXT, 2.2, 180 * self.turn.v)


def _cover(p, pm, rect, zoom=1.0, pan=0.0):
    """Draws pm scaled to cover rect (cropped to the middle), like CSS object-fit: cover; zoom > 1 crops more, pan (-1 .. 1) slides."""
    sw, sh = pm.width(), pm.height()
    if not sw or not sh:
        return
    s = max(rect.width() / sw, rect.height() / sh) * zoom
    w, h = rect.width() / s, rect.height() / s
    p.drawPixmap(rect, pm, QRectF((sw - w) / 2 * (1 + pan), (sh - h) / 2, w, h))


CARD_HUE = {"news": "MAUVE", "challenge": "PEACH", "record": "GOLD", "car": "SAPPHIRE", "community": "SAPPHIRE"}


def card_hue(kind):
    """The colour a card kind is drawn in (its art, its tag), from the current palette."""
    return getattr(theme, CARD_HUE.get(kind, "MAUVE"))


class CardArt(QWidget):
    """The top strip of a news card: its picture, else a gradient in the card's colour with its glyph (class letter, #1, version) in
    big coloured letters."""

    def __init__(self, card):
        super().__init__(card)
        self.card = card
        self.setFixedHeight(96)
        self.pix, self.glyph, self.kind = None, "", "news"

    def set(self, pix, glyph, kind):
        self.pix, self.glyph, self.kind = pix, glyph, kind
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        r = QRectF(self.rect())
        clip = QPainterPath()                    # the card's top corners are round (12 px, inside its 1 px outline)
        clip.addRoundedRect(r.adjusted(0, 0, 0, 12), 11, 11)
        p.setClipPath(clip)
        if self.pix and not self.pix.isNull():
            _cover(p, self.pix, r)
            return
        hue = card_hue(self.kind)
        g = QLinearGradient(r.topLeft(), r.bottomRight())
        g.setColorAt(0, mix(theme.MANTLE, hue, 0.30))
        g.setColorAt(1, mix(theme.MANTLE, hue, 0.06))
        p.fillRect(r, g)
        glow = QRadialGradient(QPointF(r.width() * 0.8, r.height() * 0.5), r.width() * 0.5)
        glow.setColorAt(0, alpha(hue, 0.25))
        glow.setColorAt(1, alpha(hue, 0))
        p.fillRect(r, glow)
        if not self.glyph:
            return
        px = 64
        f = font(px, QFont.Weight.Black, italic=True)
        while px > 28 and QFontMetrics(f).horizontalAdvance(self.glyph) > r.width() * 0.62:     # long glyphs (rc5, 0.9.10) get smaller
            px -= 4
            f = font(px, QFont.Weight.Black, italic=True)
        p.setFont(f)
        box = QRectF(0, 0, r.width() - 18, r.height())
        p.setPen(alpha(theme.CRUST, 0.45))                                           # a soft drop shadow
        p.drawText(box.translated(2, 3), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, self.glyph)
        tg = QLinearGradient(0, r.height() * 0.2, 0, r.height() * 0.85)
        tg.setColorAt(0, mix(hue, theme.WHITE, 0.25))
        tg.setColorAt(1, QColor(hue))
        p.setPen(QPen(QBrush(tg), 1))
        p.drawText(box, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, self.glyph)


def has_detail(c):
    return bool(c.get("body") or c.get("facts") or c.get("rows"))


def two_lines(text, fm, width):
    """text cut to what fits on two lines of `width`, with an ellipsis when it does not fit."""
    words, lines, cur = text.split(), [], ""
    for i, w in enumerate(words):
        trial = f"{cur} {w}".strip()
        if fm.horizontalAdvance(trial) <= width or not cur:
            cur = trial
            continue
        lines.append(cur)
        cur = w
        if len(lines) == 2:
            break
    else:
        lines.append(cur)
        return "\n".join(lines[:2]) if len(lines) <= 2 else ""
    return lines[0] + "\n" + fm.elidedText(lines[1] + " " + " ".join(words[i:]), Qt.TextElideMode.ElideRight, width)


class NewsCard(QFrame):
    clicked = Signal(object)                   # the card dict
    HEIGHT = 206

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("clear")            # painted here: no style-sheet fill behind the round corners
        self.url = self.full_title = self.full_text = ""
        self.h = Tween(self, 220)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(1, 1, 1, 1)
        lay.setSpacing(0)
        self.art = CardArt(self)
        lay.addWidget(self.art)
        body = QVBoxLayout()
        body.setContentsMargins(15, 13, 15, 15)
        body.setSpacing(4)
        self.tag = label("", 12, QFont.Weight.Bold, spacing=0.1)
        self.title = label("", 17, QFont.Weight.Medium, theme.WHITE)
        self.text = label("", 13, colour=theme.MUTED)
        self.text.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        body.addWidget(self.tag)
        body.addWidget(self.title)
        body.addWidget(self.text, 1)
        lay.addLayout(body, 1)
        sp = self.sizePolicy()
        sp.setRetainSizeWhenHidden(True)
        self.setSizePolicy(sp)
        self.setFixedHeight(self.HEIGHT)                 # every page the same height: no jumping when the page changes

    def set(self, c, pix):
        self.c = c
        self.url = "detail" if has_detail(c) else c["url"]
        self.art.set(pix, c.get("glyph", ""), c["kind"])
        self.tag.setText(c["tag"])
        self.tag.setStyleSheet(f"color: {card_hue(c['kind'])};")
        self.full_title, self.full_text = c["title"], c["text"]
        self.text.setStyleSheet(f"color: {theme.MUTED};")
        self._elide()
        self.setCursor(Qt.CursorShape.PointingHandCursor if self.url else Qt.CursorShape.ArrowCursor)

    def _elide(self):
        w = max(10, self.width() - 32)
        title = self.title.fontMetrics().elidedText(self.full_title, Qt.TextElideMode.ElideRight, w)
        text = two_lines(self.full_text, self.text.fontMetrics(), w)
        self.title.setText(title)
        self.text.setText(text)
        self.title.setToolTip(self.full_title if title != self.full_title else "")          # only when something is cut off
        self.text.setToolTip(self.full_text if text.endswith("…") else "")

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._elide()

    def enterEvent(self, e):
        self.h.to(1)
        super().enterEvent(e)

    def leaveEvent(self, e):
        self.h.to(0)
        super().leaveEvent(e)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(mix(theme.MANTLE, "#262a42", self.h.v))
        p.drawRoundedRect(r, 12, 12)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(mix(theme.BORDER, theme.ACCENT if self.url else theme.SURFACE2, self.h.v * (0.7 if self.url else 1)), 1))
        p.drawRoundedRect(r, 12, 12)

    def mouseReleaseEvent(self, e):
        if self.url and e.button() == Qt.MouseButton.LeftButton and self.rect().contains(e.position().toPoint()):
            self.clicked.emit(self.c)
        super().mouseReleaseEvent(e)


class Dots(QWidget):
    """Page dots; the long one glides to the current page. Clicking a dot goes to its page."""
    picked = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.n = 0
        self.pos_ = Tween(self, 320)
        self.setFixedHeight(16)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set(self, n, i):
        self.n = n
        self.setFixedWidth(max(0, (n - 1) * 16 + 24) if n else 0)
        self.pos_.to(i)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        x = 0.0
        for k in range(self.n):
            near = max(0.0, 1 - abs(k - self.pos_.v))
            w = 8 + 16 * near
            p.setBrush(mix(theme.SURFACE1, theme.ACCENT, near))
            p.drawRoundedRect(QRectF(x, 4, w, 8), 4, 4)
            x += w + 8

    def mousePressEvent(self, e):
        x = 0.0
        for k in range(self.n):
            w = 8 + 16 * max(0.0, 1 - abs(k - self.pos_.v))
            if x - 4 <= e.position().x() <= x + w + 4:
                self.picked.emit(k)
                return
            x += w + 8


class _Stage(QWidget):
    """Holds the page of cards at an offset that can be animated (a layout would put it straight back)."""

    def __init__(self, page, parent=None):
        super().__init__(parent)
        self.page = page
        page.setParent(self)
        page.installEventFilter(self)            # its layout changes (new card text) must resize the stage too
        self.off = QPointF(0, 0)
        self.ghost = QLabel(self)                 # a picture of the page that is leaving
        self.ghost.hide()

    def sizeHint(self):
        return self.page.sizeHint()

    def minimumSizeHint(self):
        return self.page.minimumSizeHint()

    def hasHeightForWidth(self):
        return True

    def heightForWidth(self, w):                 # wrapped card text: as tall as the page needs at this width
        lay = self.page.layout()
        lay.activate()
        return max(lay.totalHeightForWidth(w), lay.totalMinimumSize().height())

    def eventFilter(self, obj, e):
        if obj is self.page and e.type() == QEvent.Type.LayoutRequest:
            self.updateGeometry()
        return False

    def place(self):
        self.page.setGeometry(int(self.off.x()), int(self.off.y()), self.width(), self.height())

    def resizeEvent(self, e):
        self.place()
        super().resizeEvent(e)


class Carousel(QWidget):
    """Four cards a page (the first column a little wider). Pages slide; with more than one page there are arrows and dots, and it
    moves on by itself every few seconds unless the pointer is over it."""
    PER_PAGE = 4
    AUTO_MS = 9000
    opened = Signal(object)                    # a card was clicked

    def __init__(self, parent=None):
        super().__init__(parent)
        self.cards, self.page, self.pix = [], 0, lambda url: None
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(12)
        page = QWidget()
        page.setObjectName("clear")
        grid = QGridLayout(page)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(16)
        self.slots = []
        for i in range(self.PER_PAGE):
            c = NewsCard()
            c.clicked.connect(self.opened)
            grid.addWidget(c, 0, i)
            grid.setColumnStretch(i, 13 if i == 0 else 10)
            self.slots.append(c)
        self.stage = _Stage(page)
        lay.addWidget(self.stage)
        self.slide = QVariantAnimation(self)
        self.slide.setDuration(420)
        self.slide.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.slide.setStartValue(0.0)
        self.slide.setEndValue(1.0)
        self.slide.valueChanged.connect(self._step)
        self.slide.finished.connect(self._done)
        self._dx = self._dy = 0
        self.prev = IconButton("left", 32, 16, 14, label_text="Previous")
        self.next = IconButton("right", 32, 16, 14, label_text="Next")
        self.prev.clicked.connect(lambda: self.flip(-1))
        self.next.clicked.connect(lambda: self.flip(1))
        self.dots = Dots()
        self.dots.picked.connect(self.go)
        self.nav = QWidget()
        self.nav.setObjectName("clear")
        nl = QHBoxLayout(self.nav)
        nl.setContentsMargins(0, 0, 0, 0)
        nl.setSpacing(14)
        nl.addStretch(1)
        nl.addWidget(self.prev)
        nl.addWidget(self.dots, 0, Qt.AlignmentFlag.AlignVCenter)
        nl.addWidget(self.next)
        nl.addStretch(1)
        lay.addWidget(self.nav)
        self.auto = QTimer(self)
        self.auto.setInterval(self.AUTO_MS)
        self.auto.timeout.connect(self._auto)

    def pages(self):
        return max(1, -(-len(self.cards) // self.PER_PAGE))

    def set_cards(self, cards, pix):
        """pix(url) -> QPixmap or None for a card picture."""
        first = not self.cards and bool(cards)
        same = [(c["kind"], c["title"]) for c in cards] == [(c["kind"], c["title"]) for c in self.cards]
        self.cards, self.pix = cards, pix
        self.page = min(self.page, self.pages() - 1)
        self.setVisible(bool(cards))
        self._show()
        self.stage.updateGeometry()
        if first:
            self._animate(0, 18)                       # the cards rise into place the first time they arrive
        elif not same:
            fade_in(self.stage.page, 260)
        if self.pages() > 1:
            self.auto.start()
        else:
            self.auto.stop()

    def flip(self, step):
        self.go((self.page + step) % self.pages(), step)

    def go(self, page, step=None):
        if page == self.page or not self.cards:
            return
        step = step or (1 if page > self.page else -1)
        self.stage.ghost.setPixmap(self.stage.page.grab())
        self.stage.ghost.setGeometry(0, 0, self.stage.width(), self.stage.height())
        self.stage.ghost.show()
        self.stage.ghost.raise_()
        self.page = page
        self._show()
        self._animate(70 * step, 0)
        self.auto.start()                               # a click restarts the wait

    def _animate(self, dx, dy):
        self._dx, self._dy = dx, dy
        for w in (self.stage.page, self.stage.ghost):
            eff = QGraphicsOpacityEffect(w)
            w.setGraphicsEffect(eff)
        self.slide.stop()
        self.slide.start()
        self._step(0.0)

    def _step(self, t):
        t = float(t)
        st = self.stage
        st.off = QPointF(self._dx * (1 - t), self._dy * (1 - t))
        st.place()
        if st.page.graphicsEffect():
            st.page.graphicsEffect().setOpacity(t)
        if st.ghost.isVisible():
            st.ghost.move(int(-self._dx * t), 0)
            if st.ghost.graphicsEffect():
                st.ghost.graphicsEffect().setOpacity(1 - t)

    def _done(self):
        st = self.stage
        st.ghost.hide()
        st.ghost.clear()
        st.page.setGraphicsEffect(None)
        st.ghost.setGraphicsEffect(None)
        st.off = QPointF(0, 0)
        st.place()

    def _auto(self):
        w = self.window()
        if self.pages() > 1 and not self.underMouse() and w.isActiveWindow() and not w.isMinimized():
            self.flip(1)

    def _show(self):
        page = self.cards[self.page * self.PER_PAGE:(self.page + 1) * self.PER_PAGE]
        for i, slot in enumerate(self.slots):
            if i < len(page):
                slot.set(page[i], self.pix(page[i]["image"]) if page[i]["image"] else None)
                slot.show()
            else:
                slot.hide()
        self.nav.setVisible(self.pages() > 1)
        self.dots.set(self.pages(), self.page)


class Hero(QFrame):
    """The big rounded panel: the server's picture (covering, darkened towards the text) or the drawn light streaks and glow."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("hero")
        self.pix = None

    def set_picture(self, pix):
        pix = pix if pix and not pix.isNull() else None
        if pix is not self.pix:
            self.pix = pix
            self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        shape = QPainterPath()
        shape.addRoundedRect(r, 16, 16)
        p.fillPath(shape, QColor(theme.HERO))
        p.setClipPath(shape)
        if self.pix:
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            _cover(p, self.pix, r)
            g = QLinearGradient(r.left(), 0, r.right(), 0)               # keep the title readable
            g.setColorAt(0, QColor(26, 28, 46, 225))
            g.setColorAt(0.55, QColor(26, 28, 46, 90))
            g.setColorAt(1, QColor(26, 28, 46, 30))
            p.fillRect(r, g)
            g = QLinearGradient(0, r.top() + r.height() * 0.45, 0, r.bottom())   # and the cards
            g.setColorAt(0, QColor(26, 28, 46, 0))
            g.setColorAt(1, QColor(26, 28, 46, 200))
            p.fillRect(r, g)
        else:
            self._streaks(p, r)
        p.setClipping(False)
        p.setPen(QPen(QColor(theme.BORDER), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(shape)

    # the design's 1000 x 520 drawing: (colour, opacity, x1, y1, x2, y2)
    STREAKS = (("MAUVE", 0.14, 520, 60, 940, -60), ("MAUVE", 0.14, 460, 130, 960, -10), ("MAUVE", 0.14, 580, 210, 1000, 90),
               ("MAUVE", 0.14, 540, 280, 1000, 170), ("PEACH", 0.16, 640, 100, 1000, 0), ("PEACH", 0.16, 680, 170, 1000, 100))

    @classmethod
    def _streaks(cls, p, r):
        # scaled uniformly to cover the panel, so the glow stays a circle
        s = max(r.width() / 1000, r.height() / 520)
        ox, oy = r.left() + (r.width() - 1000 * s) / 2, r.top() + (r.height() - 520 * s) / 2

        def pt(x, y):
            return QPointF(ox + x * s, oy + y * s)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(alpha(theme.GRAPE, 0.08))           # a flat, crisp disc as in the design (a circle there, not an ellipse)
        p.drawEllipse(pt(800, 200), 250 * s, 250 * s)
        for colour, a, x1, y1, x2, y2 in cls.STREAKS:
            p.setPen(QPen(alpha(getattr(theme, colour), a), 2 * s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawLine(pt(x1, y1), pt(x2, y2))


class DetailSheet(QWidget):
    """A card's detail over the hero: the hero dims, and a panel sized to its content shows the tag, title, facts, text, a table
    (best times, cars), a primary action (PLAY) and the outside link. Closes with the X, Escape or a click beside the panel."""
    open_url = Signal(str)
    action = Signal(str)
    WIDTH = 620

    def __init__(self, parent):
        super().__init__(parent)
        self.setObjectName("clear")
        self.url, self.act = "", ""
        self.panel = QFrame(self)
        self.panel.setObjectName("clear")              # the sheet paints the panel (rounded) behind it
        lay = QVBoxLayout(self.panel)
        lay.setContentsMargins(28, 22, 22, 24)
        lay.setSpacing(10)
        top = QHBoxLayout()
        self.tag = label("", 12, QFont.Weight.Bold, spacing=0.1)
        top.addWidget(self.tag, 1, Qt.AlignmentFlag.AlignVCenter)
        self.x = IconButton("close", 32, 16, 14, label_text="Close")
        self.x.clicked.connect(self.close_sheet)
        top.addWidget(self.x)
        lay.addLayout(top)
        self.title = label("", 24, QFont.Weight.Bold, theme.WHITE, wrap=True)
        lay.addWidget(self.title)
        self.facts = QGridLayout()
        self.facts.setHorizontalSpacing(24)
        self.facts.setVerticalSpacing(8)
        lay.addLayout(self.facts)
        self.body = QTextBrowser()
        self.body.setOpenExternalLinks(True)
        self.body.setFrameShape(QFrame.Shape.NoFrame)
        self.body.setTextInteractionFlags(Qt.TextInteractionFlag.LinksAccessibleByMouse)
        self.body.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.body.setFont(font(14))
        lay.addWidget(self.body)
        self.rows = QGridLayout()
        self.rows.setHorizontalSpacing(14)
        self.rows.setVerticalSpacing(6)
        lay.addLayout(self.rows)
        foot = QHBoxLayout()
        foot.setSpacing(18)
        self.go = QPushButton("")
        self.go.setObjectName("primary")
        self.go.setCursor(Qt.CursorShape.PointingHandCursor)
        self.go.clicked.connect(lambda: (self.close_sheet(), self.action.emit(self.act)))
        foot.addWidget(self.go)
        self.link = LinkButton("")
        self.link.clicked.connect(lambda: self.open_url.emit(self.url))
        foot.addWidget(self.link)
        foot.addStretch(1)
        lay.addSpacing(4)
        lay.addLayout(foot)
        QShortcut(QKeySequence(Qt.Key.Key_Escape), self, self.close_sheet)
        self.hide()

    @staticmethod
    def _clear(grid):
        while grid.count():
            w = grid.takeAt(0).widget()
            if w:
                w.deleteLater()

    def show_card(self, c):
        self.url, self.act = c.get("url") or "", c.get("action") or ""
        hue = card_hue(c["kind"])
        self.tag.setText(c["tag"])
        self.tag.setStyleSheet(f"color: {hue};")
        self.title.setText(c["title"])
        self.title.setStyleSheet(f"color: {theme.WHITE};")
        self._clear(self.facts)
        for i, (k, v) in enumerate(c.get("facts") or []):
            kl, vl = label(k.upper(), 11, QFont.Weight.Bold, theme.MUTED, spacing=0.08), label(str(v), 16, QFont.Weight.Medium, theme.WHITE, wrap=True)
            self.facts.addWidget(kl, (i // 3) * 2, i % 3)
            self.facts.addWidget(vl, (i // 3) * 2 + 1, i % 3, Qt.AlignmentFlag.AlignTop)
        body = c.get("body") or ""
        self.body.setVisible(bool(body))
        self.body.setStyleSheet(f"QTextBrowser {{ background: transparent; border: none; color: {theme.TEXT}; }}")
        self.body.setMarkdown(body)
        self._clear(self.rows)
        for i, r in enumerate(c.get("rows") or []):
            pos = label(f"{r['pos']}", 15, QFont.Weight.Black, theme.GOLD if r["pos"] == 1 else theme.MUTED)
            pos.setFixedWidth(22)
            self.rows.addWidget(pos, i, 0)
            self.rows.addWidget(label(r["name"], 15, QFont.Weight.Medium, theme.WHITE), i, 1)
            self.rows.addWidget(label(r["value"], 15, QFont.Weight.Medium, theme.TEXT), i, 2, Qt.AlignmentFlag.AlignRight)
            self.rows.addWidget(label(r.get("extra") or "", 13, colour=theme.MUTED), i, 3)
        self.rows.setColumnStretch(3, 1)
        self.go.setText(c.get("action_label") or "")
        self.go.setVisible(bool(self.act))
        self.link.setText(c.get("link") or "Open")
        self.link.setFixedSize(QFontMetrics(self.link.font()).horizontalAdvance(self.link.text()) + 24, 40)
        self.link.setVisible(bool(self.url))
        self.place()
        self.raise_()
        self.show()
        self.setFocus()
        fade_in(self, 160)

    def place(self):
        """Covers the hero; the panel is centred, as tall as its content (the text scrolls when that is more than the hero allows).
        Measures from scratch each time, so a re-render while it is open gives the same layout."""
        self.setGeometry(self.parentWidget().rect())
        w = min(self.WIDTH, self.width() - 48)
        room = self.height() - 48
        self.panel.setFixedWidth(w)
        lay = self.panel.layout()
        if self.body.isVisible():
            m = lay.contentsMargins()
            probe = self.body.document().clone()            # the browser's own document keeps the width the browser gives it
            probe.setTextWidth(w - m.left() - m.right() - 14)  # 14: the vertical scroll bar's room
            self.body.setFixedHeight(int(probe.size().height()) + 6)
        lay.invalidate()
        h = lay.totalHeightForWidth(w) if lay.hasHeightForWidth() else lay.totalSizeHint().height()
        if h > room and self.body.isVisible():
            self.body.setFixedHeight(max(60, self.body.height() - (h - room)))
            lay.invalidate()
            h = room
        h = min(h, room)
        self.panel.setGeometry(int((self.width() - w) / 2), int((self.height() - h) / 2), w, int(h))
        self.update()

    def close_sheet(self):
        self.hide()

    def mousePressEvent(self, e):
        if not self.panel.geometry().contains(e.position().toPoint()):
            self.close_sheet()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        shape = QPainterPath()
        shape.addRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 16, 16)
        p.fillPath(shape, alpha(theme.CRUST, 0.72))
        r = QRectF(self.panel.geometry()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QPen(QColor(theme.BORDER), 1))
        p.setBrush(QColor(theme.MANTLE))
        p.drawRoundedRect(r, 14, 14)
