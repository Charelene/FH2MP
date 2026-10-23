"""Short UI sounds the server offers as launcher assets (`/v1/launcher/assets/<name>.wav`): fetched once into the cache, then played
without Qt Multimedia (not shipped): winsound on Windows, the desktop's own player (PipeWire / PulseAudio / ALSA) on Linux."""
import os
import shutil
import subprocess
import sys

from .. import launch, paths


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
