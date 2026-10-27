"""Short UI sounds the server offers as launcher assets (`/v1/launcher/assets/<name>.wav`): fetched once into the cache, then played
without Qt Multimedia (not shipped): winsound on Windows, the desktop's own player (PipeWire / PulseAudio / ALSA) on Linux."""
import os
import shutil
import subprocess
import sys

from .. import launch, paths

SOCIAL_CUES = {"receive": "zen-receive.wav", "friend_added": "zen-success.wav"}


def social(cue):
    """Play a bundled Zen cue through the existing native player; audio never blocks the UI."""
    name = SOCIAL_CUES.get(cue)
    if not name:
        return
    path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "assets", "sounds", name)
    try:
        play(path)
    except (OSError, RuntimeError):
        pass


class SocialNotifications:
    """Compare confirmed social state, sharing message watermarks between polls and open chats."""
    def __init__(self, emit=social):
        self.emit = emit
        self.context = None
        self.friends = None
        self.incoming = {}
        self.accepts = set()

    def _context(self, context):
        if context != self.context:
            self.context, self.friends, self.incoming = context, None, {}
            self.accepts.clear()

    def snapshot(self, context, data):
        self._context(context)
        friends = {p['xuid'] for p in data.get('friends', [])}
        if self.friends is not None and friends - self.friends:
            self.emit('friend_added')
        initial = self.friends is None
        self.accepts -= friends
        self.friends = friends | self.accepts
        received = False
        for ch in data.get('chats', []):
            if 'lastIncomingId' not in ch:
                continue
            latest = ch.get('lastIncomingId', 0)
            previous = self.incoming.get(ch['id'])
            if not initial and previous is not None and latest > previous:
                received = True
            self.incoming[ch['id']] = max(latest, previous or 0)
        if received:
            self.emit('receive')

    def accepted(self, context, xuid):
        self._context(context)
        if xuid not in self.accepts and (self.friends is None or xuid not in self.friends):
            self.emit('friend_added')
        self.accepts.add(xuid)
        if self.friends is not None:
            self.friends.add(xuid)

    def messages(self, context, ref, rows, me):
        self._context(context)
        latest = max((r['id'] for r in rows if r['sender'] != me), default=0)
        previous = self.incoming.get(ref)
        if previous is not None and latest > previous:
            self.emit('receive')
        self.incoming[ref] = max(latest, previous or 0)


def fetch(api, name):
    """-> local path of the sound, downloading it the first time; None when the server has none."""
    path = os.path.join(paths.cache_dir(), "ui", name)
    if os.path.isfile(path):
        return path
    data = api.asset(name)
    if data[:4] != b"RIFF" or len(data) > 2_000_000:
        return None
    os.makedirs(os.path.dirname(path), exist_ok=True)
    paths.atomic_write(path, data)
    return path


def play(path):
    if sys.platform == "win32":
        import winsound
        winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)
        return
    for tool in ("pw-play", "paplay", "aplay"):
        exe = shutil.which(tool)
        if exe:
            subprocess.Popen([exe, path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             env=launch.host_env(), start_new_session=True)
            return
