"""Draws the launcher icon (run once; the results are committed): mauve rounded square, dark 'F', Roboto Bold."""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QFont, QFontDatabase, QGuiApplication, QImage, QLinearGradient, QPainter, QPainterPath, QTransform

here = os.path.dirname(os.path.abspath(__file__))
app = QGuiApplication([])
QFontDatabase.addApplicationFont(os.path.join(here, "..", "fh2mp_launcher", "assets", "fonts", "Roboto-Bold.ttf"))
S = 512
QFontDatabase.addApplicationFont(os.path.join(here, "..", "fh2mp_launcher", "assets", "fonts", "Roboto-BlackItalic.ttf"))
img = QImage(S, S, QImage.Format.Format_ARGB32)
img.fill(Qt.GlobalColor.transparent)
p = QPainter(img)
p.setRenderHint(QPainter.RenderHint.Antialiasing)
tile = QPainterPath()
tile.addRoundedRect(QRectF(24, 24, S - 48, S - 48), 110, 110)
p.fillPath(tile, QColor("#1e2030"))
f = QFont("Roboto")
f.setPixelSize(420)
f.setWeight(QFont.Weight.Black)
f.setItalic(True)
g = QLinearGradient(100, 0, 430, 0)
g.setColorAt(0, QColor("#f5a97f"))
g.setColorAt(0.55, QColor("#f5bde6"))
g.setColorAt(1, QColor("#c6a0f6"))
path = QPainterPath()
path.addText(0, 0, f, "F")
path.setFillRule(Qt.FillRule.WindingFill)
br = path.boundingRect()
sc = 300 / br.height()                                   # the glyph fills ~60% of the tile, exactly centred
path = QTransform().translate(S / 2, S / 2).scale(sc, sc).translate(-br.center().x(), -br.center().y()).map(path)
g = QLinearGradient(S / 2 - 150, 0, S / 2 + 150, 0)
g.setColorAt(0, QColor("#f5a97f"))
g.setColorAt(0.55, QColor("#f5bde6"))
g.setColorAt(1, QColor("#c6a0f6"))
p.fillPath(path, g)
p.end()
img.scaled(256, 256, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation).save(
    os.path.join(here, "..", "fh2mp_launcher", "assets", "icon.png"))
img.save(os.path.join(here, "icon-512.png"))
try:
    from PIL import Image
    Image.open(os.path.join(here, "icon-512.png")).save(os.path.join(here, "icon.ico"), sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
except ImportError:
    print("Pillow missing: icon.ico not written", file=sys.stderr)
