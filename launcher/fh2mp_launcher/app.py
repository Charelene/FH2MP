"""Entry point: python -m fh2mp_launcher"""
import os
import sys


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--dev" in argv:                            # the developer launcher: same program, own settings/key/cache (see paths.is_dev)
        argv.remove("--dev")
        os.environ["FH2MP_DEV"] = "1"
    if "--version" in argv:
        from .version import __version__
        print(__version__)
        return 0
    if "--selftest" in argv:                       # used by the packaging checks: imports everything, opens nothing
        from . import api, azimuth, core, identity, keystore, launch, shim, updater  # noqa: F401
        from .ui import main_window  # noqa: F401
        print("ok")
        return 0
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QFont, QFontDatabase
    from PySide6.QtWidgets import QApplication

    from .core import Launcher
    from .ui import theme
    from .ui.main_window import MainWindow

    app = QApplication([sys.argv[0]] + argv)
    app.setApplicationName("FH2MP Dev" if os.environ.get("FH2MP_DEV") == "1" else "FH2MP")
    app.setOrganizationName("FH2MP")
    fonts = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "fonts")
    for name in ("Roboto-Regular.ttf", "Roboto-Medium.ttf", "Roboto-Bold.ttf", "Roboto-BlackItalic.ttf"):
        QFontDatabase.addApplicationFont(os.path.join(fonts, name))
    from PySide6.QtGui import QGuiApplication, QIcon

    from . import desktop
    QGuiApplication.setDesktopFileName(desktop.desktop_id())          # GNOME/Wayland matches the window to its .desktop file by this name
    desktop.install_user_entry()
    app.setWindowIcon(QIcon(os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "icon.png")))
    app.setStyleSheet(theme.QSS)
    f = QFont("Roboto")
    f.setPixelSize(14)
    app.setFont(f)
    lc = Launcher()
    win = MainWindow(lc)
    win.resize(820, 560)
    win.show()
    win.start()
    if "--screenshot" in argv:                      # headless smoke test: render, save, quit
        from PySide6.QtCore import QTimer
        out = argv[argv.index("--screenshot") + 1]
        QTimer.singleShot(int(os.environ.get("FH2MP_SHOT_DELAY", "2500")), lambda: (win.grab().save(out), app.quit()))
    return app.exec()


def main_dev():
    return main(["--dev", *sys.argv[1:]])
