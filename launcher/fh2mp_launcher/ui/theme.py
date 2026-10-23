"""Colours and the style sheet: the launcher design (night blue, lilac primary, peach -> grape wordmark) and Roboto, as on scuffed.af.
The colours are module globals that use() can swap for another palette at run time; widgets read them when they paint."""
DEFAULT = {
    "BASE": "#242740", "MANTLE": "#1f2134", "CRUST": "#1a1630", "HERO": "#1a1c2e", "SURFACE0": "#363a54", "SURFACE1": "#4a4f72",
    "SURFACE2": "#5d638a", "OVERLAY0": "#7c829f", "LINE": "#31344e", "TEXT_C": "#d5d9f5", "SUBTEXT0": "#b5bad8", "WHITE": "#ffffff",
    "MAUVE": "#c9a0f7", "LAVENDER": "#d9c3f7", "BLUE": "#8aadf4", "SAPPHIRE": "#7ec8e3", "PEACH": "#ff9f6e", "GRAPE": "#c78cf7",
    "GREEN": "#8fe0a0", "GOLD": "#f2d479", "YELLOW": "#f2c46d", "RED": "#f2899a", "ACCENT_PRESSED": "#b58ae6",
}
ALT = {**DEFAULT,
       "BASE": "#173a1e", "MANTLE": "#12301a", "CRUST": "#0d2312", "HERO": "#102a16", "SURFACE0": "#245230", "SURFACE1": "#316b3e",
       "SURFACE2": "#3f824c", "OVERLAY0": "#7fa080", "LINE": "#285634", "TEXT_C": "#eef5cf", "SUBTEXT0": "#bcd49a",
       "MAUVE": "#f5e31a", "LAVENDER": "#fff176", "BLUE": "#8cc63e", "SAPPHIRE": "#9be15d", "PEACH": "#8cc63e", "GRAPE": "#f5e31a",
       "GREEN": "#b5f07a", "GOLD": "#ffe94d", "ACCENT_PRESSED": "#dccb10"}
FONT = "Roboto"
QSS = ""
current = "DEFAULT"


def use(name="DEFAULT"):
    """Switches every colour (and QSS) to the palette DEFAULT or ALT; the caller re-applies QSS and repaints."""
    global BG, SURFACE, SURFACE_2, BORDER, TEXT, MUTED, ACCENT, ACCENT_HOVER, GOOD, WARN, BAD, PILL, QSS, current
    current = name
    globals().update({"DEFAULT": DEFAULT, "ALT": ALT}[name])
    g = globals()
    BG, SURFACE, SURFACE_2, BORDER, TEXT, MUTED = g["BASE"], g["MANTLE"], g["SURFACE0"], g["LINE"], g["TEXT_C"], g["SUBTEXT0"]
    ACCENT, ACCENT_HOVER, GOOD, WARN, BAD = g["MAUVE"], g["LAVENDER"], g["GREEN"], g["YELLOW"], g["RED"]
    # the message boxes (status pill, message of the day, dialog banners): (background, outline, text, button text)
    PILL = {"warn": ("#3a3020", "#6b5a2c", g["YELLOW"], "#2a2112"),
            "bad": ("#3d2230", "#6e3448", g["RED"], "#2a1219"),
            "info": (_mix(g["MANTLE"], g["MAUVE"], 0.12), _mix(g["LINE"], g["MAUVE"], 0.35), g["LAVENDER"], g["CRUST"])}
    QSS = _qss()
    return QSS


def _mix(a, b, t):
    pa, pb = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(pa, pb))


def _pill_qss():
    out = []
    for kind, (bg, line, ink, btn_ink) in PILL.items():
        sel = "" if kind == "info" else f'[kind="{kind}"]'
        out.append(f"QFrame#pill{sel}, QFrame#banner{sel} {{ background: {bg}; border: 1px solid {line}; border-radius: 10px; }}")
        out.append(f"QFrame#pill{sel} QLabel, QFrame#banner{sel} QLabel {{ color: {ink}; }}")
        out.append(f"QFrame#pill{sel} QPushButton, QFrame#banner{sel} QPushButton {{ background: {ink}; color: {btn_ink}; border: none; "
                   f"border-radius: 7px; padding: 4px 10px; font-size: 13px; font-weight: 700; }}")
    return "\n".join(out)


def _qss():
    return f"""
QWidget {{ background: {BG}; color: {TEXT}; }}
QDialog, QMainWindow, QStackedWidget, QStackedWidget > QWidget {{ background: {BG}; }}
QLabel {{ background: transparent; }}
QLabel#title {{ font-size: 26px; font-weight: 700; color: {WHITE}; }}
QLabel#h2 {{ font-size: 18px; font-weight: 500; color: {WHITE}; }}
QLabel#muted {{ color: {MUTED}; }}
QLabel#small {{ color: {MUTED}; font-size: 12px; }}
QLabel#code {{ font-family: monospace; font-size: 16px; letter-spacing: 1px; background: {SURFACE_2}; border: 1px solid {BORDER};
               border-radius: 10px; padding: 14px; }}
QFrame#card {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 12px; }}
QFrame#card QLabel {{ background: transparent; }}
{_pill_qss()}
QFrame#rule {{ background: {BORDER}; max-height: 1px; min-height: 1px; border: none; }}
QFrame#topbar {{ border: none; border-bottom: 1px solid {BORDER}; }}
QWidget#clear {{ background: transparent; }}
QPushButton {{ background: {SURFACE_2}; border: 1px solid {BORDER}; border-radius: 10px; padding: 8px 16px; color: {TEXT}; }}
QPushButton:hover {{ background: {SURFACE1}; border-color: {SURFACE2}; }}
QPushButton:pressed {{ background: {SURFACE}; }}
QPushButton:disabled {{ color: {OVERLAY0}; background: {SURFACE}; }}
QPushButton#primary {{ background: {ACCENT}; border: none; color: {CRUST}; font-weight: 700; }}
QPushButton#primary:hover {{ background: {ACCENT_HOVER}; }}
QPushButton#primary:disabled {{ background: {SURFACE0}; color: {OVERLAY0}; }}
QPushButton#link {{ background: transparent; border: none; color: {TEXT}; padding: 6px 4px; font-size: 16px; }}
QPushButton#link:hover {{ color: {WHITE}; }}
QPushButton#danger {{ color: {BAD}; }}
QMenu {{ background: {MANTLE}; border: 1px solid {BORDER}; border-radius: 10px; padding: 6px; }}
QMenu::item {{ background: transparent; padding: 8px 18px; border-radius: 7px; color: {TEXT}; }}
QMenu::item:selected {{ background: {SURFACE0}; color: {WHITE}; }}
QMenu::item:disabled {{ color: {OVERLAY0}; }}
QMenu::separator {{ height: 1px; background: {BORDER}; margin: 4px 8px; }}
QLineEdit, QComboBox, QPlainTextEdit, QTextEdit {{ background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 10px; padding: 8px 10px;
              selection-background-color: {ACCENT}; selection-color: {CRUST}; }}
QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {{ border-color: {ACCENT}; }}
QComboBox QAbstractItemView {{ background: {SURFACE}; border: 1px solid {BORDER}; selection-background-color: {SURFACE_2}; }}
QCheckBox {{ spacing: 8px; }}
QCheckBox::indicator {{ width: 18px; height: 18px; border-radius: 5px; border: 1px solid {SURFACE1}; background: {SURFACE}; }}
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
QScrollBar::handle:vertical {{ background: {SURFACE1}; border-radius: 5px; min-height: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
"""


use()
