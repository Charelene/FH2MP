"""Prepare Proton and its native shader compiler without starting Azimuth or the game."""
import hashlib
import os
import shutil
import subprocess
import tempfile
import time

from . import itch, launch, paths, updater

CAB_URL = 'https://download.microsoft.com/download/B/0/C/B0C80BA3-8AD6-4958-810B-6882485230B5/standalonesdk/Installers/61d57a7a82309cd161a854a6f4619e52.cab'
CAB_ENTRY = {'name': 'd3dcompiler47-x64.cab', 'size': 1776364,
             'sha256': 'f736e161547095bb8d98c636b85fdfeb4070fefeee3c3745db3ce88f6eb1d9de'}
CAB_MEMBER = 'fil3585cb2ea5db13cc0838f8d06b5c9679'
DLL_SHA256 = 'e994847e01a6f1e4cbdc5a864616ac262f67ee4f14db194984661a8d927ab7f4'
# An existing native compiler supplied with the user's installed game is also usable.
NATIVE_HASHES = {DLL_SHA256, '4432bbd1a390874f3f0a503d45cc48d346abc3a8c0213c289f4b615bf0ee84f3'}


def compiler_path(prefix):
    return os.path.join(prefix, 'pfx', 'drive_c', 'windows', 'system32', 'd3dcompiler_47.dll')


def ready(prefix):
    if not prefix or not os.path.isfile(os.path.join(prefix, 'pfx', 'system.reg')):
        return False
    try:
        with open(compiler_path(prefix), 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest() in NATIVE_HASHES
    except OSError:
        return False


def _run(argv, env, cancelled, timeout=180):
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        deadline = time.monotonic() + timeout
        try:
            while process.poll() is None:
                if cancelled():
                    raise updater.UpdateError('Setup cancelled.')
                if time.monotonic() >= deadline:
                    raise updater.UpdateError('Linux support setup timed out. Close any running Azimuth process and try again.')
                time.sleep(.1)
            if process.returncode:
                raise updater.UpdateError('Could not prepare Linux support. Check that Proton and Steam are installed, then retry.')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


def compiler_bytes(progress=None, cancelled=lambda: False):
    archive = updater.Updater().download_url(CAB_URL, CAB_ENTRY, progress, cancelled)
    # Reuse the official pinned butler archive reader, already used for Azimuth.
    # cabextract is optional: either route must yield the exact pinned DLL.
    with tempfile.TemporaryDirectory(prefix='fh2mp-compiler-') as folder:
        cabextract = shutil.which('cabextract')
        if cabextract:
            argv = [cabextract, '-q', '-F', CAB_MEMBER, '-d', folder, archive]
        else:
            argv = [itch.binary(progress, cancelled), 'extract', archive, '--dir', folder]
        _run(argv, launch.host_env(), cancelled)
        with open(os.path.join(folder, CAB_MEMBER), 'rb') as f:
            data = f.read()
    if hashlib.sha256(data).hexdigest() != DLL_SHA256:
        raise updater.UpdateError('The Microsoft shader compiler failed its checksum check.')
    return data


def prepare(settings, proton, prefix, progress=None, cancelled=lambda: False):
    if ready(prefix):
        return
    data = compiler_bytes(progress, cancelled)
    if cancelled():
        raise updater.UpdateError('Setup cancelled.')
    registry = os.path.join(prefix, 'pfx', 'system.reg')
    if not os.path.isfile(registry):
        if os.path.isdir(prefix) and os.listdir(prefix):
            raise updater.UpdateError('Choose an empty dedicated Proton prefix folder, or an existing Proton prefix.')
        os.makedirs(prefix, exist_ok=True)
        env = launch.host_env()
        roots = launch.steam_roots()
        env.update(STEAM_COMPAT_DATA_PATH=prefix,
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=roots[0] if roots else os.path.expanduser('~/.local/share/Steam'))
        _run([proton, 'run', 'wineboot.exe', '-u'], env, cancelled)
    if not os.path.isfile(registry):
        raise updater.UpdateError('Proton did not create its prefix. Check your Proton installation and retry.')
    if cancelled():
        raise updater.UpdateError('Setup cancelled.')
    target = compiler_path(prefix)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    backup = None
    if os.path.exists(target):
        backup = target + '.before-fh2mp'
        n = 1
        while os.path.exists(backup):
            backup = target + f'.before-fh2mp-{n}'
            n += 1
        shutil.copy2(target, backup)
    try:
        paths.atomic_write(target, data, 0o644)
        if not ready(prefix):
            raise updater.UpdateError('The native shader compiler could not be verified after installation.')
    except BaseException:
        if backup:
            shutil.copy2(backup, target)
        elif os.path.exists(target):
            os.unlink(target)
        raise
