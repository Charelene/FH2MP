"""The game's memory guard (fh2mp_launcher/memguard.py): the runaway rule, the cap, the switches, and on Windows a real Job cap and watchdog
against a small Python child (never the game)."""
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


class CapAndSwitchTests(unittest.TestCase):
    def test_cap_by_ram(self):
        for ram_gb, cap_gb in ((8, 12), (16, 12), (24, 16), (32, 16), (64, 16)):
            self.assertEqual(memguard.cap_bytes(ram_gb * GB), cap_gb * GB, ram_gb)
        self.assertEqual(memguard.cap_bytes(32 * GB, 20000), 20000 * MB)
        self.assertGreater(memguard.cap_bytes(32 * GB), int(8.6 * GB) * 1.5)          # a normal session peaked at 8.5 GB (2026-11-09)

    def test_floor(self):
        self.assertEqual(memguard.floor_bytes(16 * GB), int(1.5 * GB))
        self.assertEqual(memguard.floor_bytes(64 * GB), 64 * GB // 20)

    def test_options(self):
        self.assertEqual(memguard.options({}), (True, None))
        self.assertEqual(memguard.options(None), (True, None))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": "0"}), (False, None))
        self.assertEqual(memguard.options({"FH2MP_MEMGUARD": "off", "FH2MP_MEMORY_CAP_MB": "9000"}), (False, 9000))
        self.assertEqual(memguard.options({"FH2MP_MEMORY_CAP_MB": "lots"}), (True, None))
        self.assertEqual(memguard.options({"FH2MP_MEMORY_CAP_MB": "0"}), (True, None))

    def test_never_guards_off_windows_or_without_a_pid_or_when_off(self):
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
        g.close()

    def test_start_attaches_a_guard_and_still_returns_the_game(self):
        class P:
            pass
        with mock.patch.object(launch, "build_command", lambda *a, **k: (["x"], {}, ".")), \
                mock.patch.object(launch.subprocess, "Popen", lambda *a, **k: P()):
            p = launch.start(Settings(), tempfile.mkdtemp())
        self.assertIsInstance(p.memguard, memguard.NoGuard)            # a fake Popen has no pid


class ExitAndReportTests(unittest.TestCase):
    def test_note_game_exit_reports_a_stop_and_the_peak(self):
        g = memguard.NoGuard()
        g.stopped = {"grew_mb": 4096, "seconds": 2.0, "free_mb": 1200, "total_mb": 32768, "commit_mb": 12288}
        g.peak = lambda: 13 * GB
        g.cap = 16 * GB
        lc = mock.Mock(game_started_at=None, offline_run=False, process=mock.Mock(memguard=g))
        with mock.patch.object(core.logs, "event") as event, mock.patch.object(core.logs, "info") as info:
            stopped = core.Launcher.note_game_exit(lc, memguard.STOP_CODE)
        self.assertEqual(stopped["grew_mb"], 4096)
        self.assertEqual(event.call_args.kwargs["ended_by"], "memory guard")
        self.assertEqual(event.call_args.kwargs["code_hex"], "0xc0000017")
        self.assertEqual(info.call_args.kwargs, {"peak_mb": 13 * 1024, "cap_mb": 16 * 1024})

    def test_note_game_exit_without_a_guard(self):
        lc = mock.Mock(game_started_at=None, offline_run=False)          # a Mock's attributes are not a guard
        with mock.patch.object(core.logs, "event"):
            self.assertIsNone(core.Launcher.note_game_exit(lc, 0))

    def test_a_memguard_stop_is_offered_as_a_crash_report(self):
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "data", "logs"))
        since = time.time() - 60
        self.assertIsNone(crashreport.find_crash(d, since))
        with open(os.path.join(d, memguard.STOP_FILE), "w", encoding="utf-8") as f:
            f.write("2026-11-09 17:00:00 UTC STOPPED by the FH2MP launcher: the game's memory grew 4.0 GB in 2.0 s and the PC had 1.2 GB of 32 GB RAM left\n")
        crash = crashreport.find_crash(d, since)
        self.assertIsNotNone(crash)
        self.assertIn("STOPPED by the FH2MP launcher", crash.line)
        self.assertIsNone(crashreport.find_crash(d, time.time() + 60))          # an older stop is not offered again
        names = [n for n, _p in crashreport.diagnostic_logs(d, set())]
        self.assertIn("fh2mp-memguard.log", names)                              # and it travels with the report


CHILD = r"""
import sys, time
chunks, step, limit = [], int(sys.argv[1]) << 20, int(sys.argv[2]) << 20
try:
    while sum(len(c) for c in chunks) < limit:
        chunks.append(bytearray(step))          # zero-filled: committed and touched
        time.sleep(float(sys.argv[3]))
except MemoryError:
    sys.exit(7)
time.sleep(30)
"""


@unittest.skipIf(sys.platform != "win32", "Job Objects are Windows only")
class RealJobTests(unittest.TestCase):
    """A small Python child instead of the game: a few hundred MB at most."""

    def spawn(self, step_mb, limit_mb, pause_s):
        # a venv's python.exe on Windows is a stub that runs the base interpreter as its child: watch the interpreter itself, as the guard watches azimuth.exe
        exe = getattr(sys, "_base_executable", None) or sys.executable
        return subprocess.Popen([exe, "-I", "-c", CHILD, str(step_mb), str(limit_mb), str(pause_s)], stdin=subprocess.DEVNULL,
                                creationflags=0x00000008 | 0x00000200)

    def test_the_cap_makes_allocations_fail_and_the_peak_is_read(self):
        p = self.spawn(32, 900, 0.05)
        g = memguard.guard(p, tempfile.mkdtemp(), {"FH2MP_MEMORY_CAP_MB": "300"})
        try:
            self.assertIsInstance(g, memguard.Guard)
            if g.job is None:                                                   # a runner whose own job forbids nesting: the guard runs watchdog-only
                self.skipTest("this machine does not allow the child in a nested Job")
            self.assertEqual(g.cap, 300 * MB)
            self.assertEqual(p.wait(30), 7)                                     # MemoryError in the child: the cap held
            self.assertLessEqual(g.peak(), 300 * MB)
            self.assertGreater(g.peak(), 100 * MB)
            self.assertIsNone(g.stopped)
        finally:
            g.close()
            if p.poll() is None:
                p.kill()

    def test_the_watchdog_ends_a_runaway(self):
        d = tempfile.mkdtemp()
        p = self.spawn(40, 2000, 0.02)                                          # ~2 GB/s in 40 MB steps, up to 2 GB
        with mock.patch.object(memguard, "floor_bytes", lambda total: total), mock.patch.object(memguard, "GROW", 200 * MB):
            g = memguard.guard(p, d, {})                                         # free RAM is always "below the floor" here
            try:
                code = p.wait(30)
                g.thread.join(5)
            finally:
                g.close()
                if p.poll() is None:
                    p.kill()
        self.assertEqual(code & 0xffffffff, memguard.STOP_CODE)
        self.assertGreaterEqual(g.stopped["grew_mb"], 200)
        with open(os.path.join(d, memguard.STOP_FILE), encoding="utf-8") as f:
            self.assertIn("STOPPED by the FH2MP launcher", f.read())

    def test_closing_the_guard_leaves_the_game_running(self):
        p = self.spawn(8, 16, 0.01)
        g = memguard.guard(p, tempfile.mkdtemp(), {})
        try:
            time.sleep(0.5)
            g.close()                                                           # the launcher closing: no KILL_ON_JOB_CLOSE
            time.sleep(0.5)
            self.assertIsNone(p.poll())
        finally:
            p.kill()
            p.wait(10)


if __name__ == "__main__":
    unittest.main()
