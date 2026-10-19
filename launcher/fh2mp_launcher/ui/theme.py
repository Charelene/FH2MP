"""Colours and the style sheet: Catppuccin Macchiato and Roboto, as on scuffed.af (its main.css), mauve as the primary colour."""
BASE = "#24273a"
MANTLE = "#1e2030"
CRUST = "#181926"
SURFACE0 = "#363a4f"
SURFACE1 = "#494d64"
SURFACE2 = "#5b6078"
OVERLAY0 = "#6e738d"
TEXT_C = "#cad3f5"
SUBTEXT0 = "#a5adcb"
MAUVE = "#c6a0f6"
LAVENDER = "#b7bdf8"
BLUE = "#8aadf4"
SAPPHIRE = "#7dc4e4"
PEACH = "#f5a97f"
GREEN = "#a6da95"
YELLOW = "#eed49f"
RED = "#ed8796"

BG = BASE
SURFACE = MANTLE
SURFACE_2 = SURFACE0
BORDER = SURFACE0
TEXT = TEXT_C
MUTED = SUBTEXT0
ACCENT = MAUVE
ACCENT_HOVER = LAVENDER
ACCENT_PRESSED = "#a98ad6"
GOOD = GREEN
WARN = YELLOW
BAD = RED
FONT = "Roboto"

QSS = f"""
* {{ font-family: "Roboto", system-ui, sans-serif; font-size: 14px; }}
QWidget {{ background: {BG}; color: {TEXT}; }}
QDialog, QMainWindow, QStackedWidget, QStackedWidget > QWidget {{ background: {BG}; }}
QLabel {{ background: transparent; }}
QLabel#title {{ font-size: 26px; font-weight: 700; }}
QLabel#h2 {{ font-size: 18px; font-weight: 500; }}
QLabel#muted {{ color: {MUTED}; }}
QLabel#small {{ color: {MUTED}; font-size: 12px; }}
QLabel#code {{ font-family: monospace; font-size: 16px; letter-spacing: 1px; background: {SURFACE_2}; border: 1px solid {BORDER};
               border-radius: 10px; padding: 14px; }}
QFrame#card {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 12px; }}
QFrame#card QLabel {{ background: transparent; }}
QFrame#banner {{ background: {SURFACE_2}; border: 1px solid {BORDER}; border-left: 3px solid {ACCENT}; border-radius: 10px; }}
QFrame#banner[kind="warn"] {{ border-left-color: {WARN}; }}
QFrame#banner[kind="bad"] {{ border-left-color: {BAD}; }}
QFrame#rule {{ background: {BORDER}; max-height: 1px; min-height: 1px; border: none; }}
QPushButton {{ background: {SURFACE_2}; border: 1px solid {BORDER}; border-radius: 10px; padding: 8px 16px; color: {TEXT}; }}
QPushButton:hover {{ background: {SURFACE1}; border-color: {SURFACE2}; }}
QPushButton:pressed {{ background: {SURFACE}; }}
QPushButton:disabled {{ color: {OVERLAY0}; background: {SURFACE}; }}
QPushButton#play {{ background: {ACCENT}; border: none; color: {CRUST}; font-size: 20px; font-weight: 700; letter-spacing: 3px;
                   border-radius: 12px; padding: 16px 48px; }}
QPushButton#play:hover {{ background: {ACCENT_HOVER}; }}
QPushButton#play:pressed {{ background: {ACCENT_PRESSED}; }}
QPushButton#play:disabled {{ background: {SURFACE0}; color: {OVERLAY0}; }}
QPushButton#primary {{ background: {ACCENT}; border: none; color: {CRUST}; font-weight: 700; }}
QPushButton#primary:hover {{ background: {ACCENT_HOVER}; }}
QPushButton#primary:disabled {{ background: {SURFACE0}; color: {OVERLAY0}; }}
QPushButton#link {{ background: transparent; border: none; color: {MUTED}; padding: 6px 10px; }}
QPushButton#link:hover {{ color: {TEXT}; }}
QPushButton#patreon {{ background: transparent; border: 1px solid {PEACH}; color: {PEACH}; padding: 6px 14px; border-radius: 16px; }}
QPushButton#patreon:hover {{ background: {PEACH}; color: {CRUST}; }}
QPushButton#danger {{ color: {BAD}; }}
QLineEdit, QComboBox, QPlainTextEdit, QTextEdit {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 10px; padding: 8px 10px;
              selection-background-color: {ACCENT}; selection-color: {CRUST}; }}
QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {{ border-color: {ACCENT}; }}
QComboBox QAbstractItemView {{ background: {SURFACE}; border: 1px solid {BORDER}; selection-background-color: {SURFACE_2}; }}
QCheckBox {{ spacing: 8px; }}
QCheckBox::indicator {{ width: 18px; height: 18px; border-radius: 5px; border: 1px solid {BORDER}; background: {SURFACE}; }}
QCheckBox::indicator:checked {{ background: {ACCENT}; border-color: {ACCENT}; }}
QRadioButton {{ spacing: 8px; }}
QRadioButton::indicator {{ width: 16px; height: 16px; border-radius: 9px; border: 2px solid {SURFACE2}; background: {SURFACE}; }}
QRadioButton::indicator:checked {{ border-color: {ACCENT}; background: qradialgradient(cx:.5, cy:.5, radius:.5, fx:.5, fy:.5, stop:0 {ACCENT}, stop:.55 {ACCENT}, stop:.6 {SURFACE}, stop:1 {SURFACE}); }}
QProgressBar {{ background: {SURFACE}; border: none; border-radius: 4px; max-height: 8px; min-height: 8px; text-align: center; color: transparent; }}
QProgressBar::chunk {{ background: {ACCENT}; border-radius: 4px; }}
QListWidget {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 10px; padding: 4px; }}
QListWidget::item {{ padding: 8px; border-radius: 6px; }}
QListWidget::item:selected {{ background: {SURFACE_2}; color: {TEXT}; }}
QToolTip {{ background: {SURFACE_2}; color: {TEXT}; border: 1px solid {BORDER}; padding: 4px 8px; }}
QScrollBar:vertical {{ background: transparent; width: 10px; }}
QScrollBar::handle:vertical {{ background: {BORDER}; border-radius: 5px; min-height: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
"""
