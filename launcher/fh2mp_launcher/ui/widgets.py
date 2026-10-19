from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QBrush, QColor, QFont, QLinearGradient, QPainter, QPainterPath, QPixmap
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QSizePolicy, QVBoxLayout, QWidget

from . import theme


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
            p.drawPixmap(r.toRect(), self._pix.scaled(self._size, self._size, Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                                      Qt.TransformationMode.SmoothTransformation))
        else:
            g = QLinearGradient(0, 0, self._size, self._size)
            g.setColorAt(0, QColor(theme.MAUVE))
            g.setColorAt(1, QColor(theme.BLUE))
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


class Dot(QWidget):
    COLOURS = {"good": theme.GOOD, "warn": theme.WARN, "bad": theme.BAD, "idle": theme.OVERLAY0}

    def __init__(self, parent=None):
        super().__init__(parent)
        self._c = QColor(self.COLOURS["idle"])
        self.setFixedSize(12, 12)

    def set_state(self, state):
        self._c = QColor(self.COLOURS.get(state, self.COLOURS["idle"]))
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setBrush(self._c)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(1, 1, 10, 10)


class StatusRow(QWidget):
    clicked = Signal()

    def __init__(self, title, parent=None):
        super().__init__(parent)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 6, 0, 6)
        lay.setSpacing(12)
        self.dot = Dot()
        self.title = QLabel(title)
        self.title.setStyleSheet("font-weight: 500;")
        self.detail = QLabel("")
        self.detail.setObjectName("muted")
        self.detail.setWordWrap(True)
        self.button = QPushButton("")
        self.button.hide()
        self.button.clicked.connect(self.clicked)
        col = QVBoxLayout()
        col.setSpacing(1)
        col.addWidget(self.title)
        col.addWidget(self.detail)
        lay.addWidget(self.dot, 0, Qt.AlignmentFlag.AlignTop)
        lay.addLayout(col, 1)
        lay.addWidget(self.button, 0, Qt.AlignmentFlag.AlignVCenter)

    def set(self, state, detail, button=None):
        self.dot.set_state(state)
        self.detail.setText(detail)
        if button:
            self.button.setText(button)
            self.button.show()
        else:
            self.button.hide()


class Card(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("card")
        self.lay = QVBoxLayout(self)
        self.lay.setContentsMargins(20, 18, 20, 18)
        self.lay.setSpacing(8)


class Banner(QFrame):
    """A one-message strip: kind is info | warn | bad. Optional action button."""
    action = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("banner")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(14, 10, 10, 10)
        self.label = QLabel("")
        self.label.setWordWrap(True)
        self.button = QPushButton("")
        self.button.clicked.connect(self.action)
        lay.addWidget(self.label, 1)
        lay.addWidget(self.button)
        self.hide()

    def show_message(self, text, kind="info", button=None):
        self.setProperty("kind", kind)
        self.style().unpolish(self)
        self.style().polish(self)
        self.label.setText(text)
        self.button.setVisible(bool(button))
        self.button.setText(button or "")
        self.show()


def hrule():
    f = QFrame()
    f.setObjectName("rule")
    f.setFrameShape(QFrame.Shape.HLine)
    return f


def spacer():
    w = QWidget()
    w.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
    return w


class Brand(QWidget):
    """The FH2MP wordmark as on forza.scuffed.af: Roboto Black Italic, peach -> pink -> mauve. Sized to the ink, no empty margins."""

    def __init__(self, px=34, parent=None):
        super().__init__(parent)
        font = QFont("Roboto")
        font.setPixelSize(px)
        font.setWeight(QFont.Weight.Black)
        font.setItalic(True)
        path = QPainterPath()
        path.addText(0, 0, font, "FH2MP")
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
        g.setColorAt(0.55, QColor("#f5bde6"))
        g.setColorAt(1, QColor(theme.MAUVE))
        p.translate(1, 1)
        p.fillPath(self._path, QBrush(g))
