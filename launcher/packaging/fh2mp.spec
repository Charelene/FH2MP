# PyInstaller spec: `pyinstaller packaging/fh2mp.spec` from the launcher/ folder -> dist/fh2mp-launcher/ (one folder, no unpacking at start)
import os
import re
import sys
from PyInstaller.utils.hooks import collect_submodules

root = os.path.abspath(os.path.join(SPECPATH, ".."))        # launcher/ (SPECPATH is the folder of this file)
hidden = collect_submodules("keyring.backends") + ["keyring.backends.SecretService", "keyring.backends.Windows", "secretstorage", "jeepney"]
excluded_qt = ["PySide6.QtWebEngineCore", "PySide6.QtWebEngineWidgets", "PySide6.QtQml", "PySide6.QtQuick", "PySide6.QtMultimedia",
               "PySide6.QtSql", "PySide6.QtTest", "PySide6.QtPdf", "PySide6.Qt3DCore", "PySide6.QtCharts",
               "PySide6.QtDataVisualization", "PySide6.QtBluetooth", "PySide6.QtSensors", "PySide6.QtSerialPort", "PySide6.QtSvg"]

extra_datas = []
if os.environ.get("FH2MP_BUILDCFG"):        # private/test build: baked defaults (see fh2mp_launcher/buildcfg.py)
    extra_datas.append((os.environ["FH2MP_BUILDCFG"], "fh2mp_launcher"))
if os.environ.get("FH2MP_MIRROR"):          # ... and a signed release mirror packed inside
    extra_datas.append((os.environ["FH2MP_MIRROR"], "fh2mp_launcher/mirror"))

a = Analysis(
    [os.path.join(SPECPATH, "entry.py")],
    pathex=[root],
    datas=[(os.path.join(root, "fh2mp_launcher", "assets"), "fh2mp_launcher/assets")] + extra_datas,
    hiddenimports=hidden,
    excludes=excluded_qt + ["tkinter", "unittest", "pydoc", "test"],
    noarchive=False,
)
pyz = PYZ(a.pure)
icon = os.path.join(SPECPATH, "icon.ico") if sys.platform == "win32" else None


def version_info():
    """Windows VERSIONINFO resource: an exe without one scores worse with antivirus ML (Defender flagged rc5 as Program:Win32/Contebrew.A!ml)."""
    if sys.platform != "win32":
        return None
    from PyInstaller.utils.win32.versioninfo import (FixedFileInfo, StringFileInfo, StringStruct, StringTable, VarFileInfo, VarStruct,
                                                     VSVersionInfo)
    text = re.search(r'"(.+)"', open(os.path.join(root, "fh2mp_launcher", "version.py")).read())[1]
    nums = tuple((list(map(int, re.findall(r"\d+", text))) + [0, 0, 0, 0])[:4])
    strings = [StringStruct("CompanyName", "FH2MP"), StringStruct("FileDescription", "FH2MP Launcher"),
               StringStruct("FileVersion", text), StringStruct("InternalName", "fh2mp-launcher"),
               StringStruct("LegalCopyright", "FH2MP contributors"), StringStruct("OriginalFilename", "fh2mp-launcher.exe"),
               StringStruct("ProductName", "FH2MP Launcher"), StringStruct("ProductVersion", text)]
    return VSVersionInfo(ffi=FixedFileInfo(filevers=nums, prodvers=nums), kids=[StringFileInfo([StringTable("040904B0", strings)]),
                                                                              VarFileInfo([VarStruct("Translation", [0x0409, 1200])])])


if os.environ.get("FH2MP_ONEFILE") == "1":      # one self-extracting executable (slower to start): for handing a test build around
    exe = EXE(pyz, a.scripts, a.binaries, a.datas, [], name="fh2mp-launcher", console=False, icon=icon, upx=False, version=version_info())
else:
    exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="fh2mp-launcher", console=False, icon=icon, upx=False, version=version_info())
    coll = COLLECT(exe, a.binaries, a.datas, name="fh2mp-launcher", upx=False)
