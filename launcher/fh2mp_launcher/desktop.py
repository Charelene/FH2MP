"""Linux desktop integration for a launcher that is just an executable (not installed from the .deb): a menu entry and an icon, so GNOME/KDE show
the right name and picture in the dock and the window list. Packages ship their own .desktop file; this does nothing for them."""
import os
import shutil
import sys

from . import paths


def desktop_id():
    return "fh2mp-launcher-dev" if paths.is_dev() else "fh2mp-launcher"


def install_user_entry():
    """Writes ~/.local/share/applications/<id>.desktop and the icon when running as a loose executable / AppImage. Never raises."""
    try:
        if not sys.platform.startswith("linux") or not getattr(sys, "frozen", False):
            return
        exe = os.environ.get("APPIMAGE") or sys.executable
        if os.path.realpath(exe).startswith(("/usr/", "/opt/")):
            return                                           # installed from the .deb: the package owns the entry
        data = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
        icon_src = os.path.join(getattr(sys, "_MEIPASS", ""), "fh2mp_launcher", "assets", "icon.png")
        icon_dir = os.path.join(data, "icons", "hicolor", "256x256", "apps")
        icon_dst = ""
        if os.path.isfile(icon_src):
            import hashlib
            with open(icon_src, "rb") as f:
                blob = f.read()
            # the file name carries a hash of the picture: GNOME Shell caches an icon by path and never notices a changed file
            icon_dst = os.path.join(icon_dir, f"{desktop_id()}-{hashlib.sha256(blob).hexdigest()[:8]}.png")
            os.makedirs(icon_dir, exist_ok=True)
            if not os.path.isfile(icon_dst):
                with open(icon_dst, "wb") as f:
                    f.write(blob)
            for old in os.listdir(icon_dir):
                if old.startswith(desktop_id()) and old.endswith(".png") and os.path.join(icon_dir, old) != icon_dst:
                    try:
                        os.unlink(os.path.join(icon_dir, old))
                    except OSError:
                        pass
        entry = os.path.join(data, "applications", f"{desktop_id()}.desktop")
        name = "FH2MP Dev" if paths.is_dev() else "FH2MP"
        text = (f"[Desktop Entry]\nType=Application\nName={name}\nComment=Forza Horizon 2 online\nExec=\"{exe}\"{' --dev' if paths.is_dev() else ''}\n"
                f"Icon={icon_dst or desktop_id()}\nCategories=Game;\nTerminal=false\nStartupWMClass={desktop_id()}\n")
        try:
            if open(entry, encoding="utf-8").read() == text:
                return
        except OSError:
            pass
        os.makedirs(os.path.dirname(entry), exist_ok=True)
        with open(entry, "w", encoding="utf-8") as f:
            f.write(text)
        os.chmod(entry, 0o755)
    except Exception:  # noqa: BLE001 - cosmetic only
        pass
