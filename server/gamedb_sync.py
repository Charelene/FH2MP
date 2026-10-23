"""The car database every online player runs with: the operator's canonical gamedbRC.slt (the stock one plus approved car packs).

Signed-in launchers compare it with their copy at every start and download it when it differs (launcher/fh2mp_launcher/gamedb.py). They
place it in <Mount>/mediaoverride/stripped/, which FH2 Extra Options loads before mediapc and the original media, so a car pack the player
dropped into mediapc does not change what the game reads online.

The file is set with FH2MP_SYNC_GAMEDB; without it the endpoints answer 404 and launchers start the game with whatever they have. The file
is re-read when it changes, so a new car pack goes live without a restart.
"""
import hashlib
import lzma
import os
import threading

FILE_NAME = "gamedbRC.slt"
DOWNLOAD_PATH = f"/v1/gamedb/{FILE_NAME}.xz"
MAX_SIZE = 64 * 1024 * 1024          # the stock file is about 6 MB


class GameDb:
    def __init__(self, path=None):
        self.path = path if path is not None else os.environ.get("FH2MP_SYNC_GAMEDB", "")
        self._lock = threading.Lock()
        self._key = None
        self.sha256, self.size, self.blob = "", 0, b""

    def _load(self):
        """Brings sha256/size/blob up to date with the file. -> True when there is something to serve (the last good copy is kept when
        the file goes missing or is being replaced)."""
        if not self.path:
            return False
        try:
            st = os.stat(self.path)
            key = (st.st_mtime_ns, st.st_size)
            if key != self._key and st.st_size <= MAX_SIZE:
                with open(self.path, "rb") as f:
                    data = f.read(MAX_SIZE + 1)
                if data[:16] == b"SQLite format 3\x00" and len(data) <= MAX_SIZE:
                    self.sha256, self.size = hashlib.sha256(data).hexdigest(), len(data)
                    self.blob = lzma.compress(data, preset=9)        # once per change; a client decompresses it in well under a second
                    self._key = key
        except OSError:
            pass
        return bool(self.blob)

    def manifest(self):
        """-> {name, sha256, size, xzSize, path} or None when no database is set."""
        with self._lock:
            if not self._load():
                return None
            return {"name": FILE_NAME, "sha256": self.sha256, "size": self.size, "xzSize": len(self.blob), "path": DOWNLOAD_PATH}

    def download(self):
        """-> the xz-compressed file, or None when no database is set."""
        with self._lock:
            return self.blob if self._load() else None
