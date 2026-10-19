"""Where the private key rests: the operating system's keyring (Windows Credential Manager, macOS Keychain, Secret Service / KWallet on
Linux) when there is one, otherwise a file that only the user can read. Never next to the game, never in settings.json."""
import json
import os

from . import paths
from .identity import Identity, b64u, unb64u

SERVICE = "fh2mp-launcher"


def _account():
    return "identity-v1-dev" if paths.is_dev() else "identity-v1"     # the dev launcher has its own key


def _keyring():
    if os.environ.get("FH2MP_NO_KEYRING"):               # tests, headless boxes: use the private file
        return None
    try:
        import keyring
        from keyring.backends import fail
        kr = keyring.get_keyring()
        if isinstance(kr, fail.Keyring) or kr.priority < 1:
            return None
        return keyring
    except Exception:  # noqa: BLE001 - no usable backend (headless Linux, missing dbus, ...)
        return None


def _file():
    return os.path.join(paths.config_dir(), "identity.key")


def backend_name():
    kr = _keyring()
    return type(kr.get_keyring()).__name__ if kr else "private file"


def load():
    """-> Identity or None."""
    kr = _keyring()
    if kr:
        try:
            v = kr.get_password(SERVICE, _account())
            if v:
                return Identity(unb64u(v))
        except Exception:  # noqa: BLE001
            pass
    try:
        with open(_file(), encoding="utf-8") as f:
            return Identity(unb64u(json.load(f)["seed"]))
    except (OSError, ValueError, KeyError):
        return None


def save(identity):
    """Stores the key in the keyring, or in the private file when the keyring refuses. Returns where it went."""
    kr = _keyring()
    if kr:
        try:
            kr.set_password(SERVICE, _account(), b64u(identity.seed))
            if kr.get_password(SERVICE, _account()) == b64u(identity.seed):
                try:
                    os.unlink(_file())
                except OSError:
                    pass
                return backend_name()
        except Exception:  # noqa: BLE001
            pass
    paths.atomic_write(_file(), json.dumps({"v": 1, "seed": b64u(identity.seed)}).encode(), 0o600)
    return "private file"


def delete():
    kr = _keyring()
    if kr:
        try:
            kr.delete_password(SERVICE, _account())
        except Exception:  # noqa: BLE001
            pass
    try:
        os.unlink(_file())
    except OSError:
        pass
