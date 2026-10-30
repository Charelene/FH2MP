"""Setup operations shared by the wizard and headless checks. No game files are downloaded."""
import os
import hashlib
import shutil
import stat
import tempfile
import zipfile
from pathlib import PurePosixPath

from . import azimuth, paths, shim, updater, upstream
from .constants import AZIMUTH_TLS_HELPERS, AZIMUTH_VERSION, BUILTIN_AZIMUTH_BUILDS
from .version import __version__

REVISION = 2
MIN_AZIMUTH = AZIMUTH_VERSION


def game_folder(folder):
    folder = os.path.abspath(os.path.expanduser(folder))
    if not os.path.isfile(os.path.join(folder, 'Mount', 'forza_durango_release_final.exe')):
        raise ValueError('Select your Forza Horizon 2 folder containing Mount/forza_durango_release_final.exe.')
    return folder


def version_ok(version):
    return version == AZIMUTH_VERSION


def archive_files(path):
    """Validate every member before extraction, accepting a single enclosing directory."""
    with zipfile.ZipFile(path) as z:
        entries = []
        seen = set()
        total = 0
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            parts = PurePosixPath(name).parts
            mode = info.external_attr >> 16
            if (not parts or name.startswith('/') or '..' in parts or ':' in name or '\x00' in name
                    or stat.S_ISLNK(mode) or any(p.endswith((' ', '.')) for p in parts)):
                raise ValueError('The Azimuth ZIP contains an unsafe path.')
            if info.is_dir():
                continue
            key = name.casefold()
            if key in seen:
                raise ValueError('The Azimuth ZIP contains duplicate paths.')
            seen.add(key)
            total += info.file_size
            if total > 512 * 1024 * 1024 or len(entries) >= 10000:
                raise ValueError('The Azimuth ZIP is larger than expected.')
            entries.append((name, info))
        roots = [n[:-len('azimuth.exe')] for n, _ in entries if n.lower().endswith('azimuth.exe')]
        if len(roots) != 1:
            raise ValueError('Select the official Azimuth ZIP containing one azimuth.exe.')
        root = roots[0]
        files = {n[len(root):]: z.read(i) for n, i in entries if n.startswith(root)}
        if 'azimuth.exe' not in files or files['azimuth.exe'][:2] != b'MZ':
            raise ValueError('The ZIP does not contain Azimuth for Windows.')
        return files


def install_azimuth(archive, destination, cancelled=lambda: False):
    """Stage and validate first; back up every replaced file and roll back any failed update.

    Existing settings, saves, player pictures and mods survive an update. Destination
    is chosen explicitly in the wizard; this never discovers and mutates installations.
    """
    files = archive_files(archive)
    validate_azimuth_files(files)
    destination = os.path.abspath(destination)
    os.makedirs(destination, exist_ok=True)
    if os.path.islink(destination):
        raise ValueError('Choose an installation folder rather than a symbolic link.')
    with tempfile.TemporaryDirectory(prefix='fh2mp-azimuth-') as stage:
        exe = os.path.join(stage, 'azimuth.exe')
        with open(exe, 'wb') as f:
            f.write(files['azimuth.exe'])
        existing = os.path.isfile(os.path.join(destination, 'azimuth.exe'))
        backup = tempfile.mkdtemp(prefix='azimuth-before-setup-', dir=destination) if existing else os.path.join(stage, 'backup')
        changed = []
        try:
            for name, data in files.items():
                if cancelled():
                    raise updater.UpdateError('Setup cancelled.')
                target = os.path.join(destination, *name.split('/'))
                # Reject symlinks at every level (including the leaf) before reading or writing.
                cur = destination
                for part in name.split('/'):
                    cur = os.path.join(cur, part)
                    if os.path.islink(cur):
                        raise ValueError('The installation contains a symbolic link: ' + name)
                if existing and os.path.exists(target) and (name.lower().endswith(('.ini', '.dat', '.png'))
                        or name.lower().startswith(('data/saves/', 'mods/', 'fh2-mods/'))):
                    continue
                old = os.path.exists(target)
                if old:
                    saved = os.path.join(backup, *name.split('/'))
                    os.makedirs(os.path.dirname(saved), exist_ok=True)
                    shutil.copy2(target, saved)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                changed.append((target, name, old))
                paths.atomic_write(target, data, 0o644)
            # Modern Azimuth ships per-title helpers. Leaving the legacy helper
            # behind makes the launcher reject an otherwise complete upgrade.
            legacy = 'data/bin/azimuth-tls.dll'
            if legacy not in files and all(n in files for n in ('data/bin/azimuth-tls-1.dll', 'data/bin/azimuth-tls-2.dll')):
                target = os.path.join(destination, *legacy.split('/'))
                if os.path.islink(target):
                    raise ValueError('The installation contains a symbolic link: ' + legacy)
                if os.path.isfile(target):
                    saved = os.path.join(backup, *legacy.split('/'))
                    os.makedirs(os.path.dirname(saved), exist_ok=True)
                    shutil.copy2(target, saved)
                    changed.append((target, legacy, True))
                    os.unlink(target)
        except BaseException:
            for target, name, old in reversed(changed):
                if old:
                    shutil.copy2(os.path.join(backup, *name.split('/')), target)
                elif os.path.exists(target):
                    os.unlink(target)
            raise
    return destination


def validate_azimuth_files(files):
    digest = hashlib.sha256(files['azimuth.exe']).hexdigest()
    if not any(b['sha256'] == digest for b in BUILTIN_AZIMUTH_BUILDS):
        with tempfile.TemporaryDirectory(prefix='fh2mp-zip-check-') as stage:
            exe = os.path.join(stage, 'azimuth.exe')
            with open(exe, 'wb') as f:
                f.write(files['azimuth.exe'])
            version = azimuth.exe_version(exe)
        detail = f'Azimuth {version}' if version else 'An unrecognized Azimuth build'
        raise ValueError(f'{detail} selected. Choose the official Azimuth {AZIMUTH_VERSION} ZIP.')

    for name, expected in AZIMUTH_TLS_HELPERS.items():
        data = files.get('data/bin/' + name, b'')
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'The Azimuth ZIP is incomplete or contains a mismatched {name}. '
                             f'Choose the complete official Azimuth {AZIMUTH_VERSION} ZIP.')


def validate_archive(path):
    """Check the whole archive and exact host immediately after selection."""
    try:
        validate_azimuth_files(archive_files(path))
    except zipfile.BadZipFile:
        raise ValueError('This is not an Azimuth ZIP file.') from None
    return AZIMUTH_VERSION


class Packages:
    """Get and Install are distinct operations; only verified packages reach disk."""
    def __init__(self, lc):
        self.lc = lc
        self.dependencies = {}
        self.shim_package = None
        self.manifest = None

    def get_azimuth(self, progress=None, cancelled=lambda: False):
        """Azimuth's pinned ZIP from the FH2MP mirror (constants.AZIMUTH_ARCHIVE) -> its path; size and SHA-256 are checked by the download, the
        contents by validate_archive()."""
        from .constants import AZIMUTH_ARCHIVE
        return updater.Updater(channel=self.lc.settings['channel']).download_url(AZIMUTH_ARCHIVE['url'], AZIMUTH_ARCHIVE, progress, cancelled)

    def get_dependency(self, kind, progress=None, cancelled=lambda: False):
        key = upstream.extra_options_kind(self.lc.settings['azimuth_dir']) if kind == 'extra' else 'platform'
        self.dependencies[kind] = upstream.fetch(key, updater.Updater(channel=self.lc.settings['channel']), progress, cancelled)

    def shim_current(self):
        """A version marker alone cannot distinguish successive local candidates."""
        manifest = self.lc.manifest
        if not manifest or manifest.get('tag') != f'v{__version__}':
            up = updater.Updater(channel=self.lc.settings['channel'])
            if not up.base.startswith('file://'):
                return False
            try:
                manifest = up.fetch_manifest(tag=f'v{__version__}')
            except updater.UpdateError:
                return False
        expected = manifest.get('shim', {}).get('dllSha256')
        if manifest.get('shim', {}).get('version') != __version__:
            return False
        try:
            from .constants import MODS_DIR, SHIM_DLL
            target = os.path.join(self.lc.settings['azimuth_dir'], MODS_DIR, SHIM_DLL)
            return bool(expected) and updater.sha256_file(target) == expected
        except OSError:
            return False

    def install_dependencies(self):
        folder = self.lc.settings['azimuth_dir']
        if ('platform' not in self.dependencies and not shim.platform_ok(folder)
                or 'extra' not in self.dependencies and not shim.extra_options_complete(folder)):
            raise ValueError('Download the missing add-ons first.')
        if 'platform' in self.dependencies:
            shim._install_platform(folder, self.dependencies['platform'])
        if 'extra' in self.dependencies:
            shim.install_extra(folder, self.dependencies['extra'])
        self.lc.repair_tls_helper()

    def get_shim(self, progress=None, cancelled=lambda: False):
        from .core import read_shim_zip
        up = updater.Updater(channel=self.lc.settings['channel'])
        manifest = up.fetch_manifest(tag=f'v{__version__}')
        if manifest['shim']['version'] != __version__:
            raise updater.UpdateError(f'This launcher requires FH2MP {__version__}. The signed release does not match.')
        entry = manifest['shim']
        path = up.download(manifest['tag'], entry, progress, cancelled)
        data = read_shim_zip(path)
        if shim.blob_version(data) != entry['version']:
            raise updater.UpdateError('The FH2MP DLL version does not match its signed manifest.')
        self.manifest, self.shim_package = manifest, data
        return entry['version']

    def install_shim(self):
        if self.shim_package is None:
            raise ValueError('Download FH2MP first.')
        shim.install(self.lc.settings['azimuth_dir'], self.shim_package, self.manifest['shim'].get('dllSha256'))
        self.lc.manifest = self.manifest
