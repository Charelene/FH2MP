"""Memory guard for the game on Windows: a cap on its private commit (Job Object) and a watchdog that ends a runaway before the PC runs out.

Why (2026-11-09): a player's PC froze with RAM going from 55% to 100% within seconds (1.2.0, 32 GB). Prod crash reports stop with Azimuth's own
"out of memory" (92..886 MB at once at azimuth+0x207a92, its pipeline cache save; 864 bytes at +0x493f21), so a game can run its machine dry.
Measured on this project's Windows PC (RTX 3080 Ti + RX 6600, 32 GB): the game's private commit counts all its D3D12 memory, in VRAM or not,
and a Job's process memory limit catches it (an allocation past the cap fails, so Azimuth ends with its own "STOPPED out of memory" line and dump);
a normal session peaked at 8.5 GB of commit; ending a 3 GB process takes 0.2-0.3 s.

- Cap: CAP_MIN..CAP_MAX by 3/4 of RAM (12 GB on 8-16 GB machines, 16 GB from 22 GB of RAM up); FH2MP_MEMORY_CAP_MB overrides.
- Watchdog: every SAMPLE_S while the game runs; it ends the game (exit code STOP_CODE) only when free RAM is below the floor (max(1.5 GB, 5% of
  RAM)) and the game's own commit grew by GROW within WINDOW_S: a slow fill, or another program eating memory, never triggers it.
- FH2MP_MEMGUARD=0 turns both off. Nothing here may stop Play: any failure logs game.memguard_unavailable and the game runs unguarded.
- No KILL_ON_JOB_CLOSE: closing the launcher leaves the game running, still under its cap.
"""
import ctypes
import os
import sys
import threading
import time

from . import logs

WIN = sys.platform == "win32"
GB, MB = 1 << 30, 1 << 20
CAP_MIN, CAP_MAX = 12 * GB, 16 * GB
GROW = 3 * GB
WINDOW_S = 10.0
SAMPLE_S = 0.25
STOP_CODE = 0xC0000017                      # STATUS_NO_MEMORY, as the game's exit code after a stop
STOP_FILE = os.path.join("data", "logs", "fh2mp-memguard.log")   # in the Azimuth folder: crashreport finds it and attaches it


def cap_bytes(total_ram, override_mb=None):
    """The game's commit cap for a machine with total_ram bytes of RAM."""
    if override_mb:
        return int(override_mb) * MB
    return max(CAP_MIN, min(CAP_MAX, total_ram * 3 // 4))


def floor_bytes(total_ram):
    """Free RAM below which a growing game is ended."""
    return max(int(1.5 * GB), total_ram // 20)


def runaway(samples, floor, grow=GROW, window=WINDOW_S):
    """samples: [(t, free_ram, game_commit)], oldest first, newest last (commit None when unreadable). -> (grew_bytes, seconds) when the game must
    be ended: free RAM below `floor` now and the game's commit grew by `grow` or more within the last `window` seconds. Else None."""
    if not samples:
        return None
    t, free, commit = samples[-1]
    if free >= floor or commit is None:
        return None
    for t0, _f0, c0 in samples:
        if t - t0 <= window and c0 is not None:
            if commit - c0 >= grow:
                return commit - c0, t - t0
            break
    return None


def options(env):
    """-> (enabled, cap override in MB or None) from the game's environment (the launcher's own plus the Settings extra lines)."""
    env = env or {}
    enabled = env.get("FH2MP_MEMGUARD", "1").strip() not in ("0", "off", "no", "false")
    cap = env.get("FH2MP_MEMORY_CAP_MB", "").strip()
    return enabled, (int(cap) if cap.isdigit() and int(cap) > 0 else None)


class NoGuard:
    """What guard() returns when there is nothing to guard (not Windows, switched off, or the Windows calls failed)."""
    stopped = None
    cap = None

    def peak(self):
        return None

    def close(self):
        pass


if WIN:
    import ctypes.wintypes as w

    class _PMC(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD)] + [(n, ctypes.c_size_t) for n in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
            "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]

    class _MSX(ctypes.Structure):
        _fields_ = [("dwLength", w.DWORD), ("dwMemoryLoad", w.DWORD)] + [(n, ctypes.c_ulonglong) for n in (
            "ullTotalPhys", "ullAvailPhys", "ullTotalPageFile", "ullAvailPageFile", "ullTotalVirtual", "ullAvailVirtual", "ullAvailExtendedVirtual")]

    class _BASIC(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong), ("LimitFlags", w.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t), ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", w.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", w.DWORD), ("SchedulingClass", w.DWORD)]

    class _IO(ctypes.Structure):
        _fields_ = [(n, ctypes.c_ulonglong) for n in ("r", "w", "o", "rb", "wb", "ob")]

    class _EXT(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", _BASIC), ("IoInfo", _IO), ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    _k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _k32.OpenProcess.restype = w.HANDLE
    _k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    _k32.CreateJobObjectW.restype = w.HANDLE
    _k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
    _k32.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    _k32.QueryInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p]
    _k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    _k32.K32GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(_PMC), w.DWORD]
    _k32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(_MSX)]
    _k32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
    _k32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    _k32.WaitForSingleObject.restype = w.DWORD
    _k32.CloseHandle.argtypes = [w.HANDLE]
    _k32.GetCurrentThread.restype = w.HANDLE
    _k32.SetThreadPriority.argtypes = [w.HANDLE, ctypes.c_int]
    _ACCESS = 0x0100 | 0x0001 | 0x1000 | 0x00100000      # SET_QUOTA | TERMINATE | QUERY_LIMITED_INFORMATION | SYNCHRONIZE
    _LIMIT_PROCESS_MEMORY = 0x100
    _EXTENDED_LIMIT_INFORMATION = 9


def _free_ram():
    m = _MSX()
    m.dwLength = ctypes.sizeof(_MSX)
    if not _k32.GlobalMemoryStatusEx(ctypes.byref(m)):
        raise OSError(ctypes.get_last_error(), "GlobalMemoryStatusEx")
    return m.ullAvailPhys, m.ullTotalPhys


def _commit(handle):
    p = _PMC()
    p.cb = ctypes.sizeof(_PMC)
    return p.PrivateUsage if _k32.K32GetProcessMemoryInfo(handle, ctypes.byref(p), p.cb) else None


class Guard:
    """A running game under its cap and watchdog. `stopped` is None, or {"grew_mb", "seconds", "free_mb", "total_mb", "commit_mb"} once the
    watchdog ended the game."""

    def __init__(self, pid, folder, env):
        self.pid, self.folder, self.stopped, self.job = pid, folder, None, None
        _enabled, override = options(env)
        free, self.total = _free_ram()
        self.cap, self.floor = cap_bytes(self.total, override), floor_bytes(self.total)
        self.handle = _k32.OpenProcess(_ACCESS, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess")
        self.job = _k32.CreateJobObjectW(None, None)
        info = _EXT()
        info.BasicLimitInformation.LimitFlags = _LIMIT_PROCESS_MEMORY
        info.ProcessMemoryLimit = self.cap
        capped = bool(self.job and _k32.SetInformationJobObject(self.job, _EXTENDED_LIMIT_INFORMATION, ctypes.byref(info), ctypes.sizeof(info))
                      and _k32.AssignProcessToJobObject(self.job, self.handle))
        if not capped:          # e.g. a job the game is already in forbids nesting: the watchdog still runs
            logs.warn("game", "game.memguard_cap_unavailable", pid=pid, error=ctypes.get_last_error(), then="watchdog only")
            if self.job:
                _k32.CloseHandle(self.job)
            self.job = None
        logs.info("game", "game.memguard", pid=pid, cap_mb=self.cap // MB if capped else None, floor_mb=self.floor // MB, ram_mb=self.total // MB,
                  free_mb=free // MB)
        self.thread = threading.Thread(target=self._watch, name="memguard", daemon=True)
        self.thread.start()

    def _watch(self):
        try:
            _k32.SetThreadPriority(_k32.GetCurrentThread(), 2)       # THREAD_PRIORITY_HIGHEST: keep sampling while the machine is short of memory
            samples = []
            while _k32.WaitForSingleObject(self.handle, int(SAMPLE_S * 1000)) == 0x102:   # WAIT_TIMEOUT: still running
                now = time.monotonic()
                free, _total = _free_ram()
                samples.append((now, free, _commit(self.handle)))
                while samples and now - samples[0][0] > WINDOW_S:
                    samples.pop(0)
                hit = runaway(samples, self.floor, GROW, WINDOW_S)
                if hit:
                    self._stop(hit, free, samples[-1][2])
                    return
        except Exception as e:            # never let the guard take the launcher down; the cap stays in place
            logs.swallowed("game", "game.memguard_watch_failed", e, pid=self.pid)

    def _stop(self, hit, free, commit):
        grew, secs = hit
        _k32.TerminateProcess(self.handle, STOP_CODE)
        self.stopped = {"grew_mb": grew // MB, "seconds": round(secs, 1), "free_mb": free // MB, "total_mb": self.total // MB,
                        "commit_mb": (commit or 0) // MB}
        logs.warn("game", "game.memory_runaway", pid=self.pid, then="ended the game", **self.stopped)
        try:
            path = os.path.join(self.folder, STOP_FILE)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())} STOPPED by the FH2MP launcher: the game's memory grew "
                        f"{grew / GB:.1f} GB in {secs:.1f} s and the PC had {free / GB:.1f} GB of {self.total / GB:.0f} GB RAM left "
                        f"(game commit {(commit or 0) / GB:.1f} GB)\n")
        except OSError as e:
            logs.swallowed("game", "game.memguard_note_failed", e, path=STOP_FILE)

    def peak(self):
        """The game's highest private commit while it was in the job (bytes), or None."""
        if not self.job:
            return None
        info = _EXT()
        if not _k32.QueryInformationJobObject(self.job, _EXTENDED_LIMIT_INFORMATION, ctypes.byref(info), ctypes.sizeof(info), None):
            return None
        return info.PeakProcessMemoryUsed

    def close(self):
        for attr in ("handle", "job"):
            h = getattr(self, attr, None)
            if h:
                _k32.CloseHandle(h)
                setattr(self, attr, None)


def guard(popen, folder, env):
    """Put the just-started game under the cap and the watchdog. -> Guard, or NoGuard. Never raises."""
    pid = getattr(popen, "pid", None)
    enabled, _cap = options(env)
    if not WIN or not enabled or not isinstance(pid, int):
        if WIN and not enabled:
            logs.info("game", "game.memguard_off", reason="FH2MP_MEMGUARD=0")
        return NoGuard()
    try:
        return Guard(pid, folder, env)
    except Exception as e:
        logs.warn("game", "game.memguard_unavailable", pid=pid, error=f"{type(e).__name__}: {e}", then="game runs without the memory guard")
        return NoGuard()
