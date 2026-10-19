"""Gamerpic preparation with Qt's own image code: any common format in, a centred square PNG out."""
from PySide6.QtCore import QBuffer, QByteArray, QIODevice, Qt
from PySide6.QtGui import QImage, QImageReader


def load_square(path, size):
    """Loads an image, crops the largest centred square and scales it to size x size. None if it cannot be read."""
    r = QImageReader(path)
    r.setAutoTransform(True)
    img = r.read()
    if img.isNull():
        return None
    side = min(img.width(), img.height())
    img = img.copy((img.width() - side) // 2, (img.height() - side) // 2, side, side)
    return img.scaled(size, size, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)


def png_bytes(img, fmt=QImage.Format.Format_RGBA8888):
    buf = QBuffer()
    buf.open(QIODevice.OpenModeFlag.WriteOnly)
    img.convertToFormat(fmt).save(buf, "PNG")
    return bytes(QByteArray(buf.data()))


def prepare(path):
    """-> (png for the server: 256x256 RGBA, png for Azimuth's Gamerpic.png: 1080x1080 RGB) or None."""
    big = load_square(path, 1080)
    if big is None:
        return None
    small = big.scaled(256, 256, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)
    return png_bytes(small), png_bytes(big, QImage.Format.Format_RGB888)
