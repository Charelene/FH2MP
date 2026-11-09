"""The game's memory watch (fh2mp_launcher/memguard.py): by default it measures and never caps or ends the game (1.3.1 did both and ended
healthy games: the series below are from prod's crash reports of 2026-11-09). Kill mode and the cap are opt-in; on Windows they and the
watch run against a small Python child (never the game)."""
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from fh2mp_launcher import core, crashreport, launch, memguard  # noqa: E402
from fh2mp_launcher.settings import Settings  # noqa: E402

GB, MB = 1 << 30, 1 << 20


class RunawayTests(unittest.TestCase):
    """Kill mode's rule (opt-in)."""
    FLOOR = int(1.5 * GB)

    def series(self, step_s, commits, free):
        return [(i * step_s, free(i), c) for i, c in enumerate(commits)]

    def test_normal_play_is_left_alone(self):
        s = self.series(0.25, [8 * GB + (i % 5) * 20 * MB for i in range(400)], lambda i: 14 * GB)
        self.assertIsNone(memguard.runaway(s, self.FLOOR))

    def test_a_fast_runaway_with_the_pc_running_out_is_ended(self):
        s = self.series(0.25, [8 * GB + i * 512 * MB for i in range(9)], lambda i: 6 * GB - i * 600 * MB)   # +4 GB in 2 s, 1.2 GB free at the end
        hit = memguard.runaway(s, self.FLOOR)
        self.assertIsNotNone(hit)
        self.assertEqual(hit[0], 4 * GB)
        self.assertAlmostEqual(hit[1], 2.0)

    def test_a_fast_climb_with_plenty_of_ram_is_left_alone(self):
        s = self.series(0.25, [8 * GB + i * 512 * MB for i in range(9)], lambda i: 20 * GB)
        self.assertIsNone(memguard.runaway(s, self.FLOOR))

    def test_a_slow_fill_or_another_program_is_left_alone(self):
        s = self.series(1.0, [8 * GB + i * 200 * MB for i in range(11)], lambda i: GB)          # 2 GB in 10 s: below GROW
        self.assertIsNone(memguard.runaway(s, self.FLOOR))
        s = self.series(0.25, [8 * GB] * 40, lambda i: 10 * GB - i * 250 * MB)                   # free RAM gone, the game did not grow
        self.assertIsNone(memguard.runaway(s, self.FLOOR))

    def test_growth_older_than_the_window_does_not_count(self):
        s = self.series(1.0, [8 * GB] + [12 * GB] * 12, lambda i: GB)                               # the jump was 12 s ago
        self.assertIsNone(memguard.runaway(s[1:], self.FLOOR))
        self.assertIsNotNone(memguard.runaway(s[:3], self.FLOOR))

    def test_unreadable_commit_and_empty(self):
        self.assertIsNone(memguard.runaway([], self.FLOOR))
        self.assertIsNone(memguard.runaway([(0, GB, 8 * GB), (1, GB, None)], self.FLOOR))
        self.assertIsNotNone(memguard.runaway([(0, GB, None), (1, GB, 8 * GB), (2, GB, 12 * GB)], self.FLOOR))


class WatchTests(unittest.TestCase):
    """What the default mode keeps and when it writes a line. Series: prod 2026-11-09, the games 1.3.1 capped or ended."""

    def test_a_16_gb_laptop_boot_that_the_cap_ended(self):          # issue #437: commit 12.2 GB after 37 s, free RAM never under the floor
        wt = memguard.Watch(15676 * MB)
        lines = [wt.sample(t, free * MB, commit * MB, None, (commit + 6000) * MB, 24000 * MB)
                 for t, commit, free in ((1, 2102, 8145), (14, 7098, 5830), (15, 9180, 5217), (34, 11137, 3154), (36, 12243, 2420))]
        self.assertEqual(lines, [None] * 5)
        self.assertEqual(wt.summary(), {"peak_mb": 12243, "min_free_mb": 2420, "ram_mb": 15676, "sys_commit_pct": 76})

    def test_a_32_gb_pc_at_16_gb_commit(self):                       # normal play at 1440p / ultra: 10.7-16.4 GB for 50 minutes
        wt = memguard.Watch(31883 * MB)
        for i, commit in enumerate((10822, 15314, 13363, 15686, 16364, 11194)):
            self.assertIsNone(wt.sample(i * 60.0, 10000 * MB, commit * MB))
        self.assertEqual(wt.summary()["peak_mb"], 16364)
        self.assertIsNone(wt.summary()["sys_commit_pct"])            # not read: left out of the line

    def test_low_ram_writes_lines_but_only_now_and_then(self):       # issues #439, #440: 14-16 GB PCs, free RAM 39 MB-1.5 GB in a normal boot
        wt = memguard.Watch(14188 * MB)
        self.assertEqual(wt.floor, 1536 * MB)
        self.assertIsNone(wt.sample(0.0, 5817 * MB, 400 * MB))
        low = wt.sample(8.0, 325 * MB, 4476 * MB, None, 13000 * MB, 16000 * MB)
        self.assertEqual(low, {"commit_mb": 4476, "free_mb": 325, "ram_mb": 14188, "sys_commit_pct": 81, "grew_mb_10s": 4076, "line": 1})
        self.assertIsNone(wt.sample(9.0, 39 * MB, 5100 * MB))        # 1 s later: not again yet
        self.assertIsNone(wt.sample(17.9, 61 * MB, 5300 * MB))
        again = wt.sample(18.0, 1136 * MB, 5310 * MB)
        self.assertEqual((again["line"], again["grew_mb_10s"]), (2, 834))      # growth since the oldest reading of the last 10 s (4476 at t=8)
        self.assertIsNone(wt.sample(30.0, 4000 * MB, 5310 * MB))     # free RAM is back: nothing
        self.assertEqual(wt.summary()["min_free_mb"], 39)

    def test_the_line_budget_and_the_life_peak(self):
        wt = memguard.Watch(16 * GB)
        lines = [wt.sample(i * memguard.LOW_EVERY_S, 100 * MB, 5 * GB, 9 * GB) for i in range(memguard.LOW_LINES + 20)]
        self.assertEqual(sum(1 for ln in lines if ln), memguard.LOW_LINES)
        self.assertEqual(wt.peak, 9 * GB)                              # the process's own lifetime peak beats what the samples caught
        self.assertIsNone(memguard.Watch(16 * GB).summary())           # nothing read: no line at exit
        wt = memguard.Watch(16 * GB)
        self.assertIsNone(wt.sample(0.0, 8 * GB, None))                # commit unreadable
        self.assertEqual(wt.summary(), {"peak_mb": None, "min_free_mb": 8192, "ram_mb": 16384, "sys_commit_pct": None})


class SwitchTests(unittest.TestCase):
    def test_floor(self):
        self.assertEqual(memguard.floor_bytes(16 * GB), int(1.5 * GB))
        self.assertEqual(memguard.floor_bytes(64 * GB), 64 * GB // 20)

    def test_options(self):
        self.assertEqual(memguard.options({}), ("watch", None))        # the default never caps or ends the game
        self.assertEqual(memguard.options(None), ("watch", None))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": "0"}), ("off", None))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": "off", "FH2MP_MEMORY_CAP_MB": "9000"}), ("off", 9000))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": "kill"}), ("kill", None))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": " KILL "}), ("kill", None))
        for other in ("1", "on", "true", "yes", "killl", ""):          # 1.3.1's "on" values and typos: watch, never kill
            self.assertEqual(memguard.options({"FH2MP_MEMGUARD": other}), ("watch", None), other)
        self.assertEqual(memguard.options({"FH2MP_MEMORY_CAP_MB": "lots"}), ("watch", None))
        self.assertEqual(memguard.options({"FH2MP_MEMORY_CAP_MB": "0"}), ("watch", None))
        self.assertEqual(memguard.options({"FH2MP_MEMORY_CAP_MB": "20000"}), ("watch", 20000))

    def test_no_automatic_cap_exists(self):
        self.assertFalse(hasattr(memguard, "cap_bytes"))
        self.assertFalse(hasattr(memguard, "CAP_MIN"))

    def test_nothing_to_watch_off_windows_without_a_pid_or_when_off(self):
        class P:
            pid = 1234
        with mock.patch.object(memguard, "WIN", False):
            self.assertIsInstance(memguard.guard(P(), ".", {}), memguard.NoGuard)
        self.assertIsInstance(memguard.guard(object(), ".", {}), memguard.NoGuard)
        self.assertIsInstance(memguard.guard(P(), ".", {"FH2MP_MEMGUARD": "0"}), memguard.NoGuard)

    def test_a_failing_windows_call_never_stops_play(self):
        class P:
            pid = 1234
        with mock.patch.object(memguard, "WIN", True), mock.patch.object(memguard, "Guard", side_effect=OSError(5, "OpenProcess")), \
                mock.patch.object(memguard.logs, "warn") as warn:
            g = memguard.guard(P(), ".", {})
        self.assertIsInstance(g, memguard.NoGuard)
        self.assertEqual(warn.call_args.args[1], "game.memguard_unavailable")
        self.assertIsNone(g.peak())
        self.assertIsNone(g.summary())
        g.close()

    def test_start_attaches_a_guard_and_still_returns_the_game(self):
        class P:
            pass
        with mock.patch.object(launch, "build_command", lambda *a, **k: (["x"], {}, ".")), \
                mock.patch.object(launch.subprocess, "Popen", lambda *a, **k: P()):
            p = launch.start(Settings(), tempfile.mkdtemp())
        self.assertIsInstance(p.memguard, memguard.NoGuard)            # a fake Popen has no pid


class ExitAndReportTests(unittest.TestCase):
    def guard(self, stopped=None, seen=None):
        g = memguard.NoGuard()
        g.stopped, g.closed = stopped, 0
        g.summary = lambda: seen

        def close():
            g.closed += 1
        g.close = close
        return g

    def test_note_game_exit_logs_what_the_watch_saw(self):
        g = self.guard(seen={"peak_mb": 12243, "min_free_mb": 2420, "ram_mb": 15676, "sys_commit_pct": 76})
        lc = mock.Mock(game_started_at=None, offline_run=False, process=mock.Mock(memguard=g))
        with mock.patch.object(core.logs, "event") as event, mock.patch.object(core.logs, "info") as info:
            self.assertIsNone(core.Launcher.note_game_exit(lc, 0))
        self.assertIsNone(event.call_args.kwargs["ended_by"])
        self.assertEqual(info.call_args.args[1], "game.memory_peak")
        self.assertEqual(info.call_args.kwargs, {"peak_mb": 12243, "min_free_mb": 2420, "ram_mb": 15676, "sys_commit_pct": 76})
        self.assertEqual(g.closed, 1)

    def test_note_game_exit_reports_a_kill_mode_stop(self):
        g = self.guard(stopped={"grew_mb": 4096, "seconds": 2.0, "free_mb": 1200, "total_mb": 32768, "commit_mb": 12288})
        lc = mock.Mock(game_started_at=None, offline_run=False, process=mock.Mock(memguard=g))
        with mock.patch.object(core.logs, "event") as event, mock.patch.object(core.logs, "info") as info:
            stopped = core.Launcher.note_game_exit(lc, memguard.STOP_CODE)
        self.assertEqual(stopped["grew_mb"], 4096)
        self.assertEqual(event.call_args.kwargs["ended_by"], "memory guard")
        self.assertEqual(event.call_args.kwargs["code_hex"], "0xc0000017")
        info.assert_not_called()                                        # nothing was read: no peak line

    def test_note_game_exit_without_a_guard(self):
        lc = mock.Mock(game_started_at=None, offline_run=False)          # a Mock's attributes are not a guard
        with mock.patch.object(core.logs, "event"):
            self.assertIsNone(core.Launcher.note_game_exit(lc, 0))

    def test_a_kill_mode_stop_is_offered_as_a_crash_report(self):
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "data", "logs"))
        since = time.time() - 60
        self.assertIsNone(crashreport.find_crash(d, since))
        with open(os.path.join(d, memguard.STOP_FILE), "w", encoding="utf-8") as f:
            f.write("2026-11-09 17:00:00 UTC STOPPED by the FH2MP launcher: the game's memory grew 4.0 GB in 2.0 s and the PC had 1.2 GB of 32 GB RAM left\n")
        crash = crashreport.find_crash(d, since)
        self.assertIsNotNone(crash)
        self.assertIn("STOPPED by the FH2MP launcher", crash.line)
        self.assertIsNone(crashreport.find_crash(d, time.time() + 60))          # an older stop (1.3.1 left such files) is not offered again
        names = [n for n, _p in crashreport.diagnostic_logs(d, set())]
        self.assertIn("fh2mp-memguard.log", names)


CHILD = r"""
import sys, time
chunks, step, limit = [], int(sys.argv[1]) << 20, int(sys.argv[2]) << 20
try:
    while sum(len(c) for c in chunks) < limit:
        chunks.append(bytearray(step))          # zero-filled: committed and touched
        time.sleep(float(sys.argv[3]))
except MemoryError:
    sys.exit(7)
time.sleep(float(sys.argv[4]))
"""


@unittest.skipIf(sys.platform != "win32", "process handles and Job Objects are Windows only")
class RealProcessTests(unittest.TestCase):
    """A small Python child instead of the game."""

    def spawn(self, step_mb, limit_mb, pause_s, hold_s=30):
        # a venv's python.exe on Windows is a stub that runs the base interpreter as its child: watch the interpreter itself, as the guard watches azimuth.exe
        exe = getattr(sys, "_base_executable", None) or sys.executable
        return subprocess.Popen([exe, "-I", "-c", CHILD, str(step_mb), str(limit_mb), str(pause_s), str(hold_s)], stdin=subprocess.DEVNULL,
                                creationflags=0x00000008 | 0x00000200)

    def test_the_default_watch_never_ends_a_fast_growing_game(self):
        # everything 1.3.1 ended a game for, at once: free RAM "under the floor" for the whole run and growth far above its 3 GB / 10 s rule
        p = self.spawn(40, 600, 0.02, hold_s=2.5)                       # 600 MB in about half a second, then 2.5 s of play, then a normal exit
        with mock.patch.object(memguard, "floor_bytes", lambda total: total), mock.patch.object(memguard, "GROW", 100 * MB), \
                mock.patch.object(memguard.logs, "warn") as warn, mock.patch.object(memguard.logs, "sync") as sync:
            g = memguard.guard(p, tempfile.mkdtemp(), {})
            try:
                self.assertIsInstance(g, memguard.Guard)
                self.assertEqual((g.mode, g.job, g.cap), ("watch", None, None))
                code = p.wait(30)
            finally:
                g.close()
                if p.poll() is None:
                    p.kill()
        self.assertEqual(code, 0)                                       # it ended by itself
        self.assertIsNone(g.stopped)
        self.assertGreaterEqual(g.peak(), 600 * MB)                     # the peak of its whole life, read after it ended
        seen = g.summary()
        self.assertGreaterEqual(seen["peak_mb"], 600)
        self.assertNotIn("cap_mb", seen)
        self.assertEqual([c.args[1] for c in warn.call_args_list][:1], ["game.memory_low"])     # the low-RAM line was written
        self.assertTrue(sync.called)

    def test_the_watch_cannot_end_the_game_with_its_handle(self):
        import ctypes
        p = self.spawn(8, 16, 0.01)
        g = memguard.guard(p, tempfile.mkdtemp(), {})
        try:
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            k32.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            self.assertFalse(k32.TerminateProcess(g.handle, 1))          # opened with query rights only
            self.assertEqual(ctypes.get_last_error(), 5)                 # ERROR_ACCESS_DENIED
            time.sleep(0.3)
            self.assertIsNone(p.poll())
        finally:
            g.close()
            p.kill()
            p.wait(10)

    def test_an_explicit_cap_makes_allocations_fail(self):
        p = self.spawn(32, 900, 0.05)
        g = memguard.guard(p, tempfile.mkdtemp(), {"FH2MP_MEMORY_CAP_MB": "300"})
        try:
            self.assertIsInstance(g, memguard.Guard)
            if g.job is None:                                                   # a runner whose own job forbids nesting: no cap
                self.skipTest("this machine does not allow the child in a nested Job")
            self.assertEqual(g.cap, 300 * MB)
            self.assertEqual(p.wait(30), 7)                                     # MemoryError in the child: the cap held
        finally:
            g.close()
            if p.poll() is None:
                p.kill()
        self.assertLessEqual(g.peak(), 300 * MB)
        self.assertGreater(g.peak(), 100 * MB)
        self.assertIsNone(g.stopped)
        self.assertEqual(g.summary()["cap_mb"], 300)

    def test_kill_mode_ends_a_runaway(self):
        d = tempfile.mkdtemp()
        p = self.spawn(40, 2000, 0.02)                                          # ~2 GB/s in 40 MB steps, up to 2 GB
        with mock.patch.object(memguard, "floor_bytes", lambda total: total), mock.patch.object(memguard, "GROW", 200 * MB):
            g = memguard.guard(p, d, {"FH2MP_MEMGUARD": "kill"})                 # free RAM is always "below the floor" here
            try:
                code = p.wait(30)
            finally:
                g.close()
                if p.poll() is None:
                    p.kill()
        self.assertEqual(code & 0xffffffff, memguard.STOP_CODE)
        self.assertGreaterEqual(g.stopped["grew_mb"], 200)
        with open(os.path.join(d, memguard.STOP_FILE), encoding="utf-8") as f:
            self.assertIn("STOPPED by the FH2MP launcher", f.read())

    def test_closing_the_watch_leaves_the_game_running(self):
        p = self.spawn(8, 16, 0.01)
        g = memguard.guard(p, tempfile.mkdtemp(), {})
        try:
            time.sleep(0.5)
            g.close()                                                           # the launcher closing
            time.sleep(0.5)
            self.assertIsNone(p.poll())
        finally:
            p.kill()
            p.wait(10)


if __name__ == "__main__":
    unittest.main()
