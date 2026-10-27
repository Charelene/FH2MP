"""Small native controls for the supplied Friends design, using the launcher's tweens."""
import hashlib
import math

from PySide6.QtCore import QEvent, QRectF, Qt, Signal
from PySide6.QtGui import (QAbstractTextDocumentLayout, QColor, QGuiApplication, QPainter, QPainterPath, QPalette,
                          QPen, QPixmap, QTextDocument, QTextOption)
from PySide6.QtWidgets import (QApplication, QFrame, QGraphicsDropShadowEffect, QGraphicsScene,
                               QHBoxLayout, QLabel, QLineEdit, QVBoxLayout, QWidget)

from . import theme
from .widgets import HoverButton, Icon, IconButton, Tween, font, menu, mix, paint_icon


class BadgeIcon(IconButton):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.count = 0

    def set_count(self, count):
        self.count = count
        self.update()

    def paintEvent(self, event):
        super().paintEvent(event)
        if not self.count:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(QColor(theme.BG), 2))
        p.setBrush(QColor(theme.PEACH))
        p.drawEllipse(QRectF(self.width() - 18, 0, 18, 18))
        p.setPen(QColor(theme.CRUST))
        p.setFont(font(10, 700))
        p.drawText(QRectF(self.width() - 18, 0, 18, 18), Qt.AlignmentFlag.AlignCenter, str(min(self.count, 99)))


class Button(HoverButton):
    def __init__(self, text="", fn=None, primary=False, icon="", height=40, parent=None):
        super().__init__(parent)
        self.setText(text)
        self.primary, self.icon = primary, icon
        self.tone = None
        self.font_px, self.text_spacing, self.badge = 14, 0, 0
        self.setFont(font(14, spacing=0))
        self.setFixedHeight(height)
        self.setMinimumWidth(self.fontMetrics().horizontalAdvance(text) + (48 if icon and text else 32))
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        if fn:
            self.clicked.connect(fn)

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        bg = theme.ACCENT if self.primary else theme.SURFACE0
        ink = theme.CRUST if self.primary else theme.TEXT
        if self.tone == 'held':
            bg, ink = '#1f3a2d', theme.GOOD
        elif not self.isEnabled():
            bg, ink = "#2a2e4a", "#9aa0c8"
        else:
            bg = mix(bg, theme.WHITE, self.h.v * .12)
            bg = mix(bg, theme.CRUST, self.d.v * .14)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(bg))
        p.drawRoundedRect(QRectF(self.rect()), 10, 10)
        p.setFont(font(self.font_px, 700 if self.primary or self.tone == 'held' or self.text_spacing else 400, spacing=self.text_spacing))
        p.setPen(QColor(ink))
        text_width = p.fontMetrics().horizontalAdvance(self.text())
        total = text_width + (24 if self.icon and self.text() else 0)
        left = (self.width() - total) / 2
        if self.icon:
            x = left if self.text() else (self.width() - 18) / 2
            paint_icon(p, self.icon, QRectF(x, (self.height() - 18) / 2, 18, 18), ink)
            left += 24
        if self.text():
            p.drawText(QRectF(left, 0, text_width + 2, self.height()), Qt.AlignmentFlag.AlignVCenter, self.text())
        if self.hasFocus():
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor(theme.ACCENT_HOVER), 1.5))
            p.drawRoundedRect(QRectF(self.rect()).adjusted(2, 2, -2, -2), 8, 8)
        if self.badge:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(theme.PEACH))
            p.drawEllipse(QRectF(self.width() - 15, 1, 14, 14))
            p.setFont(font(9, 700))
            p.setPen(QColor(theme.CRUST))
            p.drawText(QRectF(self.width() - 15, 1, 14, 14), Qt.AlignmentFlag.AlignCenter, str(min(self.badge, 9)))


class Search(QFrame):
    def __init__(self, placeholder):
        super().__init__()
        self.setFixedHeight(40)
        self.setObjectName('socialSearch')
        self.setStyleSheet('QFrame#socialSearch {background:#2f3350;border-radius:10px;}')
        row = QHBoxLayout(self)
        row.setContentsMargins(12, 0, 12, 0)
        row.setSpacing(8)
        row.addWidget(Icon('search', theme.MUTED, 16))
        self.edit = QLineEdit()
        self.edit.setPlaceholderText(placeholder)
        self.edit.setAccessibleName(placeholder)
        self.edit.setStyleSheet('QLineEdit {background:transparent;border:0;padding:0;color:white;font-size:14px;}')
        row.addWidget(self.edit, 1)


class PersonAvatar(QWidget):
    def __init__(self, profile, size=36, parent=None, controller=None):
        super().__init__(parent)
        self.profile, self.px = profile, size
        self.controller = controller
        if controller is not None:
            controller.avatar_changed.connect(self._loaded)
            controller.avatar(profile['xuid'])
        self.setFixedSize(size + 4, size + 4)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

    def _loaded(self, xuid):
        if xuid == self.profile['xuid']:
            self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        offline = self.profile.get('state') == 'offline'
        palette = (theme.PEACH, theme.GRAPE, theme.GOLD, theme.SAPPHIRE)
        colour = palette[hashlib.sha256(str(self.profile['xuid']).encode()).digest()[0] % len(palette)]
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(theme.SURFACE1 if offline else colour))
        p.drawEllipse(QRectF(0, 0, self.px, self.px))
        picture = self.controller.pictures.get(self.profile['xuid']) if self.controller is not None else None
        if picture is not None and not picture.isNull():
            p.save()
            clip = QPainterPath()
            clip.addEllipse(QRectF(0, 0, self.px, self.px))
            p.setClipPath(clip)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            side = min(picture.width(), picture.height())
            p.drawPixmap(QRectF(0, 0, self.px, self.px), picture,
                         QRectF((picture.width()-side)/2, (picture.height()-side)/2, side, side))
            p.restore()
        else:
            p.setFont(font(24 if self.px > 40 else 15, 700))
            p.setPen(QColor(theme.TEXT if offline else '#14161f'))
            p.drawText(QRectF(0, 0, self.px, self.px), Qt.AlignmentFlag.AlignCenter, self.profile['gamertag'][:1].upper())
        dot = 14 if self.px > 40 else 11
        p.setPen(QPen(QColor(theme.BG if self.px > 40 else theme.MANTLE), 3 if self.px > 40 else 2))
        p.setBrush(QColor(theme.OVERLAY0 if offline else theme.GOOD))
        p.drawEllipse(QRectF(self.px - dot + 1, self.px - dot + 1, dot, dot))


class MessageBubble(QWidget):
    """Plain text measured by Qt's text layout, including emoji and long unbroken words."""
    def __init__(self, text, mine=False, max_width=420):
        super().__init__()
        self.text, self.mine = text, mine
        self.document = QTextDocument(self)
        self.document.setDocumentMargin(0)
        self.document.setDefaultFont(font(15))
        option = self.document.defaultTextOption()
        option.setWrapMode(QTextOption.WrapMode.WrapAtWordBoundaryOrAnywhere)
        self.document.setDefaultTextOption(option)
        self.document.setPlainText(text)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.set_max_width(max_width)
        self.setAccessibleName(text)
        self.copy_menu = menu(self)
        self.copy_menu.addAction('Copy message', lambda: QGuiApplication.clipboard().setText(self.text))

    def set_max_width(self, width):
        self.document.setTextWidth(max(24, width - 28))
        content_width = math.ceil(self.document.idealWidth())
        self.setFixedWidth(content_width + 28)
        self.document.setTextWidth(content_width)
        self.setFixedHeight(math.ceil(self.document.size().height()) + 20)

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(theme.ACCENT if self.mine else '#2f3350'))
        p.drawRoundedRect(QRectF(self.rect()), 14, 14)
        p.translate(14, 10)
        context = QAbstractTextDocumentLayout.PaintContext()
        context.palette.setColor(QPalette.ColorRole.Text, QColor(theme.CRUST if self.mine else theme.WHITE))
        self.document.documentLayout().draw(p, context)

    def contextMenuEvent(self, event):
        self.copy_menu.popup(event.globalPos())


class Overlay(QFrame):
    """A centred application panel with an inline heading and eased enter/exit."""
    finished = Signal(int)

    def __init__(self, controller, title, width, parent):
        host = parent.window().centralWidget()
        super().__init__(host)
        self.c, self.host = controller, host
        self.panel_width, self.panel_height = width, None
        self._closing = False
        self.setObjectName('socialOverlay')
        self.setStyleSheet('QFrame#socialOverlay {background:transparent;}')
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._shadow_key = None
        self._shadow_pixmap = None
        self._transition_pixmap = None
        self._backdrop_pixmap = None
        self._capturing_backdrop = False
        self.shell = QFrame(self)
        self.shell.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.shell.setObjectName('socialShell')
        self.shell.setStyleSheet(f'QFrame#socialShell {{background:{theme.BG};border-radius:16px;}}')
        outer = QVBoxLayout(self.shell)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        chrome = QFrame()
        chrome.setFixedHeight(56)
        chrome.setObjectName('socialChrome')
        chrome.setStyleSheet('QFrame#socialChrome {background:transparent;}')
        row = QHBoxLayout(chrome)
        row.setContentsMargins(24, 0, 16, 0)
        text = QLabel(title)
        text.setAlignment(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft)
        text.setFont(font(18, 700))
        text.setStyleSheet('color:white;background:transparent;')
        row.addWidget(text, 1)
        close = Button(icon='close', fn=self.close, height=28)
        close.setFixedWidth(28)
        close.setMinimumWidth(28)
        close.setAccessibleName('Close ' + title)
        row.addWidget(close)
        outer.addWidget(chrome)
        self.body = QWidget()
        self.body.setObjectName('clear')
        self.lay = QVBoxLayout(self.body)
        self.lay.setContentsMargins(24, 24, 24, 24)
        self.lay.setSpacing(20)
        outer.addWidget(self.body, 1)
        self.error = QLabel(self.body)
        self.error.setTextFormat(Qt.TextFormat.PlainText)
        self.error.setWordWrap(True)
        self.error.setStyleSheet(f'color:{theme.BAD};font-size:13px;background:transparent;')
        self.error.hide()
        self.motion = Tween(self, ms=200, on_change=self._frame)
        self.motion.anim.finished.connect(self._settled)
        host.installEventFilter(self)

    def set_panel_size(self, width, height=None):
        self.panel_width, self.panel_height = width, height

    def _frame(self, value):
        # Geometry/layout and the blurred shadow only change on resize.
        self._move_panel()

    def _move_panel(self):
        self.shell.move((self.width() - self.shell.width()) // 2,
                        (self.height() - self.shell.height()) // 2 + round((1 - self.motion.v) * 14))

    def _cache_shadow(self):
        width, height = self.shell.width(), self.shell.height()
        ratio = self.devicePixelRatioF()
        key = (width, height, ratio)
        if key == self._shadow_key:
            return
        # Render the blur once at the display's pixel density. Changing an
        # effect's opacity/color every frame invalidated its full-size blur.
        margin = 40
        bounds = QRectF(-margin, -margin, width + margin * 2, height + margin * 2)
        scene = QGraphicsScene()
        path = QPainterPath()
        path.addRoundedRect(QRectF(0, 0, width, height), 16, 16)
        item = scene.addPath(path)
        item.setPen(Qt.PenStyle.NoPen)
        item.setBrush(QColor(theme.BG))
        effect = QGraphicsDropShadowEffect()
        effect.setBlurRadius(32)
        effect.setOffset(0, 8)
        effect.setColor(QColor(0, 0, 0, 90))
        item.setGraphicsEffect(effect)
        pixmap = QPixmap(math.ceil(bounds.width() * ratio), math.ceil(bounds.height() * ratio))
        pixmap.setDevicePixelRatio(ratio)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        scene.render(painter, QRectF(0, 0, bounds.width(), bounds.height()), bounds)
        painter.end()
        self._shadow_pixmap, self._shadow_key = pixmap, key

    def place(self):
        previous_size = self.shell.size()
        self.setGeometry(self.host.rect())
        width = min(self.panel_width, max(280, self.width() - 40))
        self.shell.setFixedWidth(width)
        self.shell.layout().activate()
        height = self.panel_height or self.shell.sizeHint().height()
        height = min(height, max(120, self.height() - 40))
        self.shell.resize(width, height)
        self._move_panel()
        self._cache_shadow()
        if self._transition_pixmap is not None and self.shell.size() != previous_size:
            self._transition_pixmap = self.shell.grab()
        if self._backdrop_pixmap is not None and self._backdrop_pixmap.deviceIndependentSize() != self.size():
            self._cache_backdrop()

    def _cache_backdrop(self):
        self._capturing_backdrop = True
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, False)
        try:
            self._backdrop_pixmap = self.host.grab()
        finally:
            self._capturing_backdrop = False
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, True)

    def _capture_transition(self):
        # Move a single image for the brief transition instead of repainting
        # every message bubble, avatar and control at each intermediate y.
        if self._transition_pixmap is None:
            self._transition_pixmap = self.shell.grab()
            self.shell.hide()
            self._cache_backdrop()

    def showEvent(self, event):
        super().showEvent(event)
        self.place()
        self._capture_transition()
        self.raise_()
        self.setFocus()
        self.motion.to(1)

    def paintEvent(self, _):
        if self._capturing_backdrop:
            return
        p = QPainter(self)
        if self._backdrop_pixmap is not None:
            p.drawPixmap(0, 0, self._backdrop_pixmap)
        p.fillRect(self.rect(), QColor(14, 15, 24, round(168 * self.motion.v)))
        if self._shadow_pixmap is not None:
            p.setOpacity(self.motion.v)
            p.drawPixmap(self.shell.x() - 40, self.shell.y() - 40, self._shadow_pixmap)
        if self._transition_pixmap is not None:
            p.setOpacity(1)
            p.drawPixmap(self.shell.pos(), self._transition_pixmap)

    def eventFilter(self, target, event):
        if target is self.host and event.type() == QEvent.Type.Resize:
            self.place()
        return super().eventFilter(target, event)

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.close()
        else:
            super().keyPressEvent(event)

    def focusNextPrevChild(self, forward):
        current = QApplication.focusWidget() or self
        fields = [w for w in self.shell.findChildren(QWidget) if w.isVisible() and w.isEnabled() and w.focusPolicy() & Qt.FocusPolicy.TabFocus]
        if not fields:
            return False
        index = fields.index(current) if current in fields else (-1 if forward else 0)
        fields[(index + (1 if forward else -1)) % len(fields)].setFocus(Qt.FocusReason.TabFocusReason if forward else Qt.FocusReason.BacktabFocusReason)
        return True

    def mousePressEvent(self, event):
        if not self.shell.geometry().contains(event.position().toPoint()):
            self.close()

    def closeEvent(self, event):
        if self.motion.v > 0:
            event.ignore()
            self._closing = True
            self._capture_transition()
            self.motion.to(0)
        else:
            self.finished.emit(0)
            super().closeEvent(event)

    def _settled(self):
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, False)
        self._backdrop_pixmap = None
        if self._closing:
            self.close()
        else:
            self._transition_pixmap = None
            self.shell.show()
            self.update()

    def failed(self, error):
        from ..core import friendly
        parent = self.error.parentWidget()
        if parent is None or parent.layout() is None or parent.layout().indexOf(self.error) < 0:
            self.lay.addWidget(self.error)
        self.error.setText(friendly(error))
        self.error.show()
        self.place()
