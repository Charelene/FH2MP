"""Memory watch for the game on Windows. By default it only measures: nothing here caps or ends the game.

Why it only watches. 1.3.1 (2026-11-09) capped the game's commit with a Job Object (12 GB up to 16 GB of RAM, 16 GB above) and ended the game
when free RAM was under max(1.5 GB, 5%) and the game had grown 3 GB within 10 s. Both numbers came from one 32 GB PC at 720p and both were wrong
(prod, the first hour, 13 crash reports):
- A game's commit counts its D3D12 memory, in VRAM or not. Normal play is 11-16 GB on 32 GB PCs with 16 GB cards (1440p / ultra) and 12 GB half a
  minute into a boot on a 16 GB laptop: 9 sessions ran into the cap and ended with "out of memory" (azimuth+0x207a92, +0x5765b6, game+0x125d9e8).
- Loading grows 3-8 GB within seconds, and on 14-16 GB PCs free RAM is under 1.5 GB during an ordinary boot (39 MB seen, the game went on):
  5 games were ended 18-27 s into a boot, at 5.1-5.7 GB.
So what a runaway looks like is not known yet; this module collects what will show it.

- watch (default): a thread reads, once a second, the game's commit, free RAM and the system's commit charge. While free RAM is under the floor
  it writes game.memory_low (at most every LOW_EVERY_S, LOW_LINES per game) and puts the log on disk, so the last seconds before a frozen PC
  survive its reset. At exit game.memory_peak says the peak commit, the lowest free RAM and the highest system commit share.
  The process is opened with query rights only: this mode cannot end or limit it.
- FH2MP_MEMGUARD=kill (opt-in, for our own tests): the 1.3.1 watchdog, ending the game with exit code STOP_CODE.
- FH2MP_MEMORY_CAP_MB=N (opt-in): a Job Object limit of N MB on the game's commit. No KILL_ON_JOB_CLOSE.
- FH2MP_MEMGUARD=0: nothing at all.
Nothing here may stop Play: any failure logs game.memguard_unavailable and the game runs unwatched.
"""
import ctypes
import os
import sys
import threading
import time

from . import logs

WIN = sys.platform == "win32"
GB, MB = 1 << 30, 1 << 20
GROW = 3 * GB                               # kill mode: growth within WINDOW_S
WINDOW_S = 10.0
WATCH_S = 1.0                               # sample period, watch mode
KILL_S = 0.25                               # sample period, kill mode
LOW_EVERY_S = 10.0
LOW_LINES = 30
STOP_CODE = 0xC0000017                      # STATUS_NO_MEMORY, as the game's exit code after a stop (kill mode)
STOP_FILE = os.path.join("data", "logs", "fh2mp-memguard.log")   # in the Azimuth folder: crashreport finds it and attaches it (kill mode)


def floor_bytes(total_ram):
    """Free RAM below which memory counts as low."""
    return max(int(1.5 * GB), total_ram // 20)


def runaway(samples, floor, grow=GROW, window=WINDOW_S):
    """Kill mode's rule. samples: [(t, free_ram, game_commit)], oldest first, newest last (commit None when unreadable). -> (grew_bytes, seconds)
    when free RAM is below `floor` now and the game's commit grew by `grow` or more within the last `window` seconds. Else None."""
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
    """-> (mode, cap in MB or None) from the game's environment (the launcher's own plus the Settings extra lines). mode: "watch" (default),
    "off" (FH2MP_MEMGUARD=0), "kill" (FH2MP_MEMGUARD=kill). Anything else is "watch": a typo never turns the old behaviour on."""
    env = env or {}
    value = env.get("FH2MP_MEMGUARD", "").strip().lower()
    mode = "off" if value in ("0", "off", "no", "false") else "kill" if value == "kill" else "watch"
    cap = env.get("FH2MP_MEMORY_CAP_MB", "").strip()
    return mode, (int(cap) if cap.isdigit() and int(cap) > 0 else None)


class Watch:
    """What the watcher keeps of a game: the peak commit, the lowest free RAM, the highest share of the system's commit limit in use, and when a
    game.memory_low line is due. No Windows calls: tested everywhere."""

    def __init__(self, total_ram, floor=None):
        self.total, self.floor = total_ram, floor_bytes(total_ram) if floor is None else floor
        self.peak = self.min_free = self.sys_pct = None
        self.samples = 0
        self._recent = []                    # (t, commit) of the last WINDOW_S
        self._low_at, self._low_lines = None, 0

    def sample(self, now, free, commit, peak_commit=None, sys_used=None, sys_limit=None):
        """One reading. -> the fields of a game.memory_low line when one is due, else None."""
        self.samples += 1
        for v in (commit, peak_commit):
            if v is not None and (self.peak is None or v > self.peak):
                self.peak = v
        self.min_free = free if self.min_free is None else min(self.min_free, free)
        pct = round(100.0 * sys_used / sys_limit) if sys_used is not None and sys_limit else None
        if pct is not None:
            self.sys_pct = pct if self.sys_pct is None else max(self.sys_pct, pct)
        if commit is not None:
            self._recent.append((now, commit))
        while self._recent and now - self._recent[0][0] > WINDOW_S:
            self._recent.pop(0)
        if free >= self.floor or self._low_lines >= LOW_LINES or (self._low_at is not None and now - self._low_at < LOW_EVERY_S):
            return None
        self._low_at, self._low_lines = now, self._low_lines + 1
        grew = commit - self._recent[0][1] if commit is not None and self._recent else None
        return {"commit_mb": commit // MB if commit is not None else None, "free_mb": free // MB, "ram_mb": self.total // MB,
                "sys_commit_pct": pct, "grew_mb_10s": grew // MB if grew is not None else None, "line": self._low_lines}

    def life_peak(self, peak_commit):
        """The process's own peak commit, read once more after it ended (a game shorter than one sample period still gets its line)."""
        if peak_commit:
            self.peak = max(self.peak or 0, peak_commit)
            self.samples += 1

    def summary(self):
        """The fields of game.memory_peak, or None when nothing was read."""
        if not self.samples:
            return None
        return {"peak_mb": self.peak // MB if self.peak is not None else None,
                "min_free_mb": self.min_free // MB if self.min_free is not None else None, "ram_mb": self.total // MB,
                "sys_commit_pct": self.sys_pct}


class NoGuard:
    """What guard() returns when there is nothing to watch (not Windows, switched off, or the Windows calls failed)."""
    stopped = None
    cap = None
    mode = "off"

    def peak(self):
        return None

    def summary(self):
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
    _k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    _k32.K32GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(_PMC), w.DWORD]
    _k32.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(_MSX)]
    _k32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
    _k32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
    _k32.WaitForSingleObject.restype = w.DWORD
    _k32.CloseHandle.argtypes = [w.HANDLE]
    _k32.GetCurrentThread.restype = w.HANDLE
    _k32.SetThreadPriority.argtypes = [w.HANDLE, ctypes.c_int]
    _QUERY = 0x1000 | 0x00100000             # PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE: all the watch needs, and it can do no more
    _TERMINATE, _SET_QUOTA = 0x0001, 0x0100  # asked for only in kill mode / with an explicit cap
    _LIMIT_PROCESS_MEMORY = 0x100
    _EXTENDED_LIMIT_INFORMATION = 9


def _system():
    """-> (free RAM, total RAM, commit charge, commit limit) of the system, in bytes."""
    m = _MSX()
    m.dwLength = ctypes.sizeof(_MSX)
    if not _k32.GlobalMemoryStatusEx(ctypes.byref(m)):
        raise OSError(ctypes.get_last_error(), "GlobalMemoryStatusEx")
    return m.ullAvailPhys, m.ullTotalPhys, m.ullTotalPageFile - m.ullAvailPageFile, m.ullTotalPageFile


def _commit(handle):
    """-> (commit now, peak commit of the process's life) in bytes, (None, None) when unreadable."""
    p = _PMC()
    p.cb = ctypes.sizeof(_PMC)
    if not _k32.K32GetProcessMemoryInfo(handle, ctypes.byref(p), p.cb):
        return None, None
    return p.PrivateUsage, p.PeakPagefileUsage


class Guard:
    """A running game being watched. `stopped` stays None unless kill mode ended the game: then {"grew_mb", "seconds", "free_mb", "total_mb",
    "commit_mb"}."""

    def __init__(self, pid, folder, env):
        self.pid, self.folder, self.stopped, self.job, self.cap = pid, folder, None, None, None
        self.mode, cap_mb = options(env)
        free, total, _used, _limit = _system()
        self.watch = Watch(total)
        self.total, self.floor = total, self.watch.floor
        access = _QUERY | (_TERMINATE if self.mode == "kill" else 0) | (_TERMINATE | _SET_QUOTA if cap_mb else 0)
        self.handle = _k32.OpenProcess(access, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess")
        if cap_mb:
            self.job = _k32.CreateJobObjectW(None, None)
            info = _EXT()
            info.BasicLimitInformation.LimitFlags = _LIMIT_PROCESS_MEMORY
            info.ProcessMemoryLimit = cap_mb * MB
            if self.job and _k32.SetInformationJobObject(self.job, _EXTENDED_LIMIT_INFORMATION, ctypes.byref(info), ctypes.sizeof(info)) \
                    and _k32.AssignProcessToJobObject(self.job, self.handle):
                self.cap = cap_mb * MB
            else:                               # e.g. a job the game is already in forbids nesting
                logs.warn("game", "game.memguard_cap_unavailable", pid=pid, cap_mb=cap_mb, error=ctypes.get_last_error(), then="no cap")
                if self.job:
                    _k32.CloseHandle(self.job)
                self.job = None
        logs.info("game", "game.memguard", pid=pid, mode=self.mode, cap_mb=self.cap // MB if self.cap else None, floor_mb=self.floor // MB,
                  ram_mb=total // MB, free_mb=free // MB)
        self._closing = threading.Event()
        self.thread = threading.Thread(target=self._run, name="memguard", daemon=True)
        self.thread.start()

    def _run(self):
        try:
            kill = self.mode == "kill"
            if kill:
                _k32.SetThreadPriority(_k32.GetCurrentThread(), 2)       # THREAD_PRIORITY_HIGHEST: keep sampling while the machine is short of memory
            samples = []
            period = int((KILL_S if kill else WATCH_S) * 1000)
            while not self._closing.is_set() and _k32.WaitForSingleObject(self.handle, period) == 0x102:   # WAIT_TIMEOUT: still running
                now = time.monotonic()
                free, _total, used, limit = _system()
                commit, peak = _commit(self.handle)
                low = self.watch.sample(now, free, commit, peak, used, limit)
                if low:
                    logs.warn("game", "game.memory_low", pid=self.pid, **low)
                    logs.sync()                 # a PC that freezes next is reset by hand: these lines are why we looked
                if kill:
                    samples.append((now, free, commit))
                    while samples and now - samples[0][0] > WINDOW_S:
                        samples.pop(0)
                    hit = runaway(samples, self.floor, GROW, WINDOW_S)
                    if hit:
                        self._stop(hit, free, commit)
                        return
            self.watch.life_peak(_commit(self.handle)[1])                # the process is gone (or the watch closes); its handle still answers
        except Exception as e:            # never let the watch take the launcher down
            logs.swallowed("game", "game.memguard_watch_failed", e, pid=self.pid)

    def _stop(self, hit, free, commit):
        grew, secs = hit
        _k32.TerminateProcess(self.handle, STOP_CODE)
        self.stopped = {"grew_mb": grew // MB, "seconds": round(secs, 1), "free_mb": free // MB, "total_mb": self.total // MB,
                        "commit_mb": (commit or 0) // MB}
        logs.warn("game", "game.memory_runaway", pid=self.pid, then="ended the game (FH2MP_MEMGUARD=kill)", **self.stopped)
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
        """The game's highest commit (bytes), or None when nothing was read."""
        return self.watch.peak

    def summary(self):
        """The fields of game.memory_peak (Watch.summary plus the cap, when one was set), or None."""
        out = self.watch.summary()
        if out is not None and self.cap:
            out["cap_mb"] = self.cap // MB
        return out

    def close(self):
        """Ends the watch (the game has normally ended by now: the thread takes its last reading and returns at once). The handles are closed
        only once the thread has left them."""
        t = getattr(self, "thread", None)
        if t is not None and t is not threading.current_thread():
            self._closing.set()
            t.join(WATCH_S + 1.0)
            if t.is_alive():
                return
        for attr in ("handle", "job"):
            h = getattr(self, attr, None)
            if h:
                _k32.CloseHandle(h)
                setattr(self, attr, None)


def guard(popen, folder, env):
    """Start watching the just-started game. -> Guard, or NoGuard. Never raises."""
    pid = getattr(popen, "pid", None)
    mode, _cap = options(env)
    if not WIN or mode == "off" or not isinstance(pid, int):
        if WIN and mode == "off":
            logs.info("game", "game.memguard_off", reason="FH2MP_MEMGUARD=0")
        return NoGuard()
    try:
        return Guard(pid, folder, env)
    except Exception as e:
        logs.warn("game", "game.memguard_unavailable", pid=pid, error=f"{type(e).__name__}: {e}", then="game runs without the memory watch")
        return NoGuard()
