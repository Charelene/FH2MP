"""Entry point: python -m fh2mp_launcher"""
import os
import sys


def lock_profile(folder):
    """Hold the profile until process exit; Qt also recovers locks from dead owners."""
    from PySide6.QtCore import QLockFile
    lock = QLockFile(os.path.join(folder, 'launcher.lock'))
    lock.setStaleLockTime(0)  # A launcher may stay open for days; age alone is not stale.
    if lock.tryLock(0):
        return lock
    if lock.error() == QLockFile.LockError.LockFailedError:
        return None
    raise OSError('Could not lock the launcher profile folder: ' + folder)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv[:1] in (["--save-copy"], ["--save-adopt"]):
        from .saves import helper_dispatch
        try:
            return helper_dispatch(argv)
        except (OSError, ValueError) as e:
            print(str(e), file=sys.stderr)
            return 1
    if "--dev" in argv:                            # the developer launcher: same program, own settings/key/cache (see paths.is_dev)
        argv.remove("--dev")
        os.environ["FH2MP_DEV"] = "1"
    if "--version" in argv:
        from .version import __version__
        print(__version__)
        return 0
    if "--selftest" in argv:                       # used by the packaging checks: imports everything, opens nothing
        from . import api, azimuth, core, identity, instance, keystore, launch, shim, updater  # noqa: F401
        from .ui import main_window  # noqa: F401
        print("ok")
        return 0
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QFont, QFontDatabase
    from PySide6.QtWidgets import QApplication, QMessageBox

    from .core import Launcher
    from .ui import theme
    from .ui.main_window import MainWindow

    app = QApplication([sys.argv[0]] + argv)
    app.setApplicationName("FH2MP Dev" if os.environ.get("FH2MP_DEV") == "1" else "FH2MP")
    app.setOrganizationName("FH2MP")
    from . import paths
    from .instance import InstanceServer, request_activation
    folder = paths.config_dir()
    try:
        profile_lock = lock_profile(folder)
    except OSError as e:
        QMessageBox.critical(None, 'Could not open FH2MP', str(e))
        return 1
    if profile_lock is None:
        if request_activation(folder):
            return 0
        QMessageBox.information(None, 'FH2MP is still running',
                                'An older launcher is still using this profile and could not be brought forward. '
                                'Close that process in your task manager, then open FH2MP again.')
        return 0
    app._profile_lock = profile_lock
    try:
        app._instance_server = InstanceServer(folder, app)
    except OSError as e:
        QMessageBox.critical(None, 'Could not open FH2MP', str(e))
        return 1
    app.aboutToQuit.connect(app._instance_server.close)
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
    lc = Launcher(load_identity=False)
    win = MainWindow(lc)
    def activate():
        win.showNormal() if win.isMinimized() else win.show()
        win.raise_()
        win.activateWindow()
    app._instance_server.activated.connect(activate)
    area = win.screen().availableGeometry()          # never open larger than the screen: a laptop at 125 % display scaling is about 1093x614
    size = (min(1280, area.width() - 40), min(760, area.height() - 80))
    win.resize(*size)
    win.move(area.left() + (area.width() - size[0]) // 2, area.top() + (area.height() - size[1]) // 2)
    win.show()
    win.start()
    if "--screenshot" in argv:                      # headless smoke test: render, save, quit
        from PySide6.QtCore import QTimer
        out = argv[argv.index("--screenshot") + 1]
        QTimer.singleShot(int(os.environ.get("FH2MP_SHOT_DELAY", "2500")), lambda: (win.grab().save(out), app.quit()))
    return app.exec()


def main_dev():
    return main(["--dev", *sys.argv[1:]])
