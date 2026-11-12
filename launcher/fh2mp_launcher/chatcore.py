"""The chat core (fh2mp-chat, Rust): the launcher starts it hidden with itself, so the in-game Guide can reach it during play, shows it from the
Chats button and closes it when the launcher exits. The core keeps one instance per config dir itself, so every call here is a short-lived
process that hands its request to the running core.

Off unless FH2MP_CHAT_CORE=1 until the chat server runs on the realm (board item "MTProto chat P7"); until then the launcher's own Chats dialog
is used. FH2MP_CHAT_EXE points at a core outside the install (development)."""
import logging
import os
import subprocess
import sys

from . import paths

log = logging.getLogger("fh2mp.chatcore")

EXE_NAME = "fh2mp-chat.exe" if sys.platform == "win32" else "fh2mp-chat"
TARGETS = ("club", "requests")


def enabled():
    return os.environ.get("FH2MP_CHAT_CORE") == "1"


def exe_path():
    """The core next to the launcher (frozen builds), or FH2MP_CHAT_EXE; None when neither exists."""
    override = os.environ.get("FH2MP_CHAT_EXE")
    if override:
        return override if os.path.isfile(override) else None
    if getattr(sys, "frozen", False):
        candidate = os.path.join(os.path.dirname(sys.executable), EXE_NAME)
        return candidate if os.path.isfile(candidate) else None
    return None


def command(*args):
    """The core's command line for these arguments, with --dev for the dev launcher; None when no core is installed."""
    exe = exe_path()
    if exe is None:
        return None
    return [exe, *args, *(["--dev"] if paths.is_dev() else [])]


def valid_target(target):
    if target in TARGETS:
        return True
    if isinstance(target, str) and target.startswith("friend:"):
        h = target[len("friend:"):]
        return 0 < len(h) <= 16 and all(c in "0123456789abcdef" for c in h)
    return False


def _spawn(cmd, what):
    try:
        flags = {"creationflags": 0x08000000 | 0x00000200} if sys.platform == "win32" else {"start_new_session": True}   # no window, own group
        subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, **flags)
        return True
    except OSError as e:
        log.warning("chat core %s: could not start %s (%s)", what, cmd[0], e)
        return False


def start_background():
    """Starts the core hidden (it keeps running when its window is closed). True when a core was asked to start."""
    if not enabled():
        return False
    cmd = command("--background")
    if cmd is None:
        log.info("chat core: not installed next to the launcher; the launcher's own chat is used")
        return False
    log.info("chat core: started hidden for this launcher session (%s)", cmd[0])
    return _spawn(cmd, "start")


def open_chat(target=None):
    """Shows the running core's window, at a chat when one is named. False when the core is off or missing (use the launcher's dialog)."""
    if not enabled():
        return False
    cmd = command("--open", *([target] if target is not None and valid_target(target) else []))
    return cmd is not None and _spawn(cmd, "open")


def quit_core():
    """Closes the running core (the launcher is exiting)."""
    if not enabled():
        return False
    cmd = command("--quit")
    return cmd is not None and _spawn(cmd, "quit")
