"""Out-of-commit stops (crash family memory-growth-oom, #91 / #468 / #477): the room Windows can still commit before Play, the memory
watch's summary in the crash report, and the advice when the commit limit was reached."""
import os
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import core, crashreport, memguard, settings as settings_mod  # noqa: E402

GB, MB = 1 << 30, 1 << 20


class PageFileGrowth(unittest.TestCase):
    def grow(self, entries, automatic=False, ram=16 * GB, sizes=None, free=None):
        sizes, free = sizes or {}, free or {}
        return memguard.page_file_growth(entries, automatic, ram, lambda p: sizes.get(p[:2].upper()),
                                         lambda p: free.get(p[:2].upper()))

    def test_managed_by_windows_grows_into_its_drive(self):
        # "?:\pagefile.sys" (every drive managed, the default): C: with 10 GB free and a 4 GB page file can give 8 GB more
        self.assertEqual(self.grow(["?:\\pagefile.sys"], sizes={"C:": 4 * GB}, free={"C:": 10 * GB}), 8 * GB)
        # a nearly full system drive: nothing to grow into
        self.assertEqual(self.grow(["C:\\pagefile.sys 0 0"], sizes={"C:": 2 * GB}, free={"C:": 1 * GB}), 0)
        # never past 3 x RAM
        self.assertEqual(self.grow(["?:\\pagefile.sys"], ram=8 * GB, sizes={"C:": 20 * GB}, free={"C:": 500 * GB}), 4 * GB)

    def test_a_set_size_grows_to_its_maximum_only(self):
        self.assertEqual(self.grow(["D:\\pagefile.sys 2048 4096"], sizes={"D:": 2 * GB}, free={"D:": 500 * GB}), 2 * GB)
        self.assertEqual(self.grow(["D:\\pagefile.sys 4096 4096"], sizes={"D:": 4 * GB}, free={"D:": 500 * GB}), 0)
        # AutomaticManagedPagefile overrides the numbers
        self.assertEqual(self.grow(["C:\\pagefile.sys 4096 4096"], automatic=True, sizes={"C:": 4 * GB}, free={"C:": 6 * GB}), 4 * GB)

    def test_no_page_file_or_unknown_drives(self):
        self.assertEqual(self.grow([]), 0)
        self.assertEqual(self.grow([""]), 0)
        self.assertEqual(self.grow(["E:\\pagefile.sys 0 0"]), 0)                 # free space unknown: counts nothing


class CheckMemory(unittest.TestCase):
    def lc(self):
        lc = core.Launcher.__new__(core.Launcher)
        return lc

    def test_a_laptop_without_room_gets_a_warning(self):
        room = {"room_mb": 5300, "limit_mb": 23800, "used_mb": 18500, "grow_mb": 0, "ram_mb": 15700, "free_mb": 900}
        lc = self.lc()
        with mock.patch.object(core.memguard, "commit_room", return_value=room), mock.patch.object(core.logs, "event") as event:
            text = lc.check_memory()
        self.assertIn("5.2 GB", text)
        self.assertIn("page file", text)
        self.assertEqual(event.call_args.args[:3], ("WARN", "play", "game.commit_room"))
        self.assertEqual(lc.start_warnings(), text)

    def test_room_enough_or_unknown_says_nothing(self):
        lc = self.lc()
        room = {"room_mb": 24171, "limit_mb": 44285, "used_mb": 28560, "grow_mb": 8446, "ram_mb": 32694, "free_mb": 13953}
        with mock.patch.object(core.memguard, "commit_room", return_value=room), mock.patch.object(core.logs, "event") as event:
            self.assertEqual(lc.check_memory(), "")
        self.assertEqual(event.call_args.args[0], "INFO")
        with mock.patch.object(core.memguard, "commit_room", return_value=None):
            self.assertEqual(lc.check_memory(), "")
        lc.disk_warning = "Drive C: has only 1.0 GB free, so the game may fail to save."
        self.assertEqual(lc.start_warnings(), lc.disk_warning)


class Reports(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        env = mock.patch.dict(os.environ, {"FH2MP_LAUNCHER_HOME": os.path.join(self.tmp, "home")})
        env.start()
        self.addCleanup(env.stop)
        self.st = settings_mod.Settings(os.path.join(self.tmp, "settings.json"))

    # shaped like #477 (2026-11-13): an RTX 2050 laptop, the boot stopped 34 s in
    MEM = {"peak_mb": 11930, "min_free_mb": 96, "ram_mb": 15700, "sys_commit_pct": 99, "commit_limit_mb": 23800}

    def crash(self, line, when):
        return crashreport.Crash("folder", line, None, when)

    def test_note_game_exit_keeps_the_summary_for_the_report(self):
        g = memguard.NoGuard()
        g.summary = lambda: dict(self.MEM)
        lc = mock.Mock(game_started_at=None, offline_run=False, process=mock.Mock(memguard=g), settings=self.st)
        with mock.patch.object(core.logs, "event"), mock.patch.object(core.logs, "info"):
            core.Launcher.note_game_exit(lc, 0xE0424833)
        kept = settings_mod.Settings(self.st.path)["last_game_memory"]
        self.assertEqual(kept["sys_commit_pct"], 99)
        self.assertAlmostEqual(kept["ended"], time.time(), delta=5)

    def test_the_summary_belongs_to_the_crash_it_was_measured_with(self):
        now = time.time()
        self.st["last_game_memory"] = dict(self.MEM, ended=now)
        self.assertIsNotNone(crashreport.game_memory(self.st, self.crash("STOPPED out of memory", now - 5)))
        self.assertIsNone(crashreport.game_memory(self.st, self.crash("STOPPED out of memory", now - 3600)))   # an older crash
        self.assertIsNone(crashreport.game_memory(self.st, self.crash("STOPPED out of memory", now + 600)))    # a later game
        self.st["last_game_memory"] = {}
        self.assertIsNone(crashreport.game_memory(self.st, self.crash("STOPPED out of memory", now)))

    def test_memory_text(self):
        self.assertEqual(crashreport.memory_text(self.MEM),
                         "game peak 11.7 GB of 15.3 GB RAM, system commit peak 99% of 23.2 GB, free RAM low 0.1 GB")
        self.assertEqual(crashreport.memory_text(None), "not measured")
        self.assertEqual(crashreport.memory_text({"peak_mb": 8000, "ram_mb": 16000, "sys_commit_pct": None, "min_free_mb": None}),
                         "game peak 7.8 GB of 15.6 GB RAM")

    def test_a_full_commit_limit_gets_its_advice(self):
        for line in ("STOPPED out of memory: 2097264 bytes at azimuth+0x5765b6",
                     "CRASHED exception 0xc0000005 at game+0x125d9e8 writing 0x1 on thread 22016",
                     "STOPPED D3D12 arena allocation HRESULT=0x8007000e"):
            c = self.crash(line, 0.0)
            c.memory = dict(self.MEM)
            hint = crashreport.known_cause(c)
            self.assertIn("Windows ran out of memory", hint, line)
            self.assertIn("23 GB", hint)
            self.assertIn("Virtual memory", hint)

    def test_no_advice_without_the_numbers_or_for_another_crash(self):
        c = self.crash("STOPPED out of memory: 864 bytes at azimuth+0x493f21", 0.0)
        self.assertNotIn("Windows ran out of memory", crashreport.known_cause(c))           # not measured (Linux, an older report)
        c.memory = dict(self.MEM, sys_commit_pct=70)                                          # room left: Azimuth's own limit, not Windows'
        self.assertNotIn("Windows ran out of memory", crashreport.known_cause(c))
        c = self.crash("CRASHED exception 0xc0000005 at game+0x66a492", 0.0)
        c.memory = dict(self.MEM)
        self.assertNotIn("Windows ran out of memory", crashreport.known_cause(c))

    def test_the_payload_carries_the_memory_field(self):
        r = crashreport.Report([("Date", "x"), ("Memory", crashreport.memory_text(self.MEM))], [], None, None)
        self.assertEqual(r.payload()["fields"]["memory"], crashreport.memory_text(self.MEM))


if __name__ == "__main__":
    unittest.main()
