"""Crash reports: noticing a crash from Azimuth's files, what is collected, and what is blanked. No Qt, no network."""
import base64
import os
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import crashreport, settings as settings_mod  # noqa: E402

BEGAN = "100 tid=1 session S began 2026-10-23 22:13:02 UTC: Azimuth 0.9.10 (azimuth.exe) on Wine 11.0 (wine-11.0 (CachyOS)), CachyOS\n"
GPU = ("110 tid=2 graphics adapter NVIDIA GeForce RTX 3080 Ti (vendor 0x10de, device 0x2208), driver 35.0.19.9999; resource binding tier 3\n")
CRASH = "200 tid=3 CRASHED exception 0xc0000005 at game+0xcf2dc3 reading 0x20 on thread 3 (modified: Mount\\mediaoverride)\n"


class FakeLauncher:
    def __init__(self, st):
        self.settings, self.account = st, {"gamertag": "Speedy"}

    def builds(self):
        return []


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.folder = os.path.join(self.tmp, "Azimuth")
        self.logs = os.path.join(self.folder, "data", "logs")
        os.makedirs(self.logs)
        os.makedirs(os.path.join(self.folder, "data", "bin"))
        with open(os.path.join(self.folder, "azimuth.exe"), "wb") as f:
            f.write(b"MZ")
        env = mock.patch.dict(os.environ, {"FH2MP_LAUNCHER_HOME": os.path.join(self.tmp, "home")})
        env.start()
        self.addCleanup(env.stop)
        self.st = settings_mod.Settings(os.path.join(self.tmp, "settings.json"))
        self.st["azimuth_dir"] = self.folder

    def write(self, rel, data, mtime=None):
        p = os.path.normpath(os.path.join(self.folder, rel))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb" if isinstance(data, bytes) else "w") as f:
            f.write(data)
        if mtime:
            os.utime(p, (mtime, mtime))
        return p

    def log(self, text, mtime=None):
        return self.write("data/logs/azimuth.log", text, mtime)


class FindTests(Base):
    def test_a_previous_sessions_mod_report_is_not_attached_to_a_new_crash(self):
        now = time.time()
        start = crashreport.datetime.datetime.fromtimestamp(now - 20, crashreport.datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        began = BEGAN.replace("2026-10-23 22:13:02", start)
        self.log(began + CRASH, now)
        self.write("fh2-mods/crashes/old.txt", "CRASH: old physics failure", now - 60)
        found = crashreport.find_crash(self.folder, now - 100)
        self.assertIn("game+0xcf2dc3", found.line)
        self.assertIsNone(found.mod_crash)

    def test_primary_failure_is_not_replaced_by_dump_writer_crash(self):
        now = time.time()
        self.log(BEGAN + CRASH + "201 tid=4 CRASHED exception 0xc0000005 at 0x6ffffd74c0e9 reading 0xffffffffffffffff\n", now)
        found = crashreport.find_crash(self.folder, now - 10)
        self.assertIn("game+0xcf2dc3", found.line)

    def test_crash_line_and_dump(self):
        now = time.time()
        self.log(BEGAN + GPU + CRASH, now)
        dmp = self.write("data/logs/20261023T221302-072-256.dmp", b"MDMP" + b"\0" * 50, now - 1)
        c = crashreport.find_crash(self.folder, now - 100)
        self.assertIn("CRASHED exception 0xc0000005", c.line)
        self.assertEqual(c.dump, dmp)
        self.assertEqual(c.seen, now)

    def test_stopped_counts_but_not_the_graphics_check(self):
        now = time.time()
        self.log(BEGAN + "300 tid=3 STOPPED the swap chain presents flip buffer 1 at game+0x10f4852\n", now)
        self.assertIn("STOPPED", crashreport.find_crash(self.folder, now - 100).line)
        self.log(BEGAN + "300 tid=3 STOPPED CreateDXGIFactory2 failed\n", now)
        self.assertIsNone(crashreport.find_crash(self.folder, now - 100))

    def test_older_crash_or_older_session_is_not_a_new_one(self):
        now = time.time()
        self.log(BEGAN + CRASH + BEGAN + "400 tid=1 audio output\n", now)       # it crashed, then the next session ran fine
        self.assertIsNone(crashreport.find_crash(self.folder, now - 100))
        self.log(BEGAN + CRASH, now - 500)                                        # file untouched since the time asked about
        self.assertIsNone(crashreport.find_crash(self.folder, now - 100))
        self.write("data/logs/old.dmp", b"MDMP", now - 500)
        self.assertIsNone(crashreport.find_crash(self.folder, now - 100))

    def test_mod_platform_report(self):
        now = time.time()
        self.log(BEGAN, now - 500)
        old = self.write("fh2-mods/crashes/crash-old.txt", "old", now - 500)
        self.write("fh2-mods/crashes/crash-new.txt", "CRASH: access violation\n  0 game+0x2D8EAD\n", now - 5)
        self.write("fh2-mods/crashes/crash-new.dmp", b"MDMP", now - 5)
        c = crashreport.find_crash(self.folder, now - 100)
        self.assertEqual(os.path.basename(c.mod_crash), "crash-new.txt")
        self.assertEqual((c.line, c.dump), ("", None))               # the platform's report alone is enough to offer a report
        self.assertNotEqual(c.mod_crash, old)
        self.assertEqual(c.seen, now - 5)

    def test_dump_alone(self):
        now = time.time()
        self.log(BEGAN, now)
        self.write("data/logs/a.dmp", b"MDMP", now)
        c = crashreport.find_crash(self.folder, now - 10)
        self.assertEqual((c.line, os.path.basename(c.dump)), ("", "a.dmp"))

    def test_dump_associates_failure_with_previous_session_after_restart(self):
        now = time.time()
        marker = "20261025T213713-771-24184.dmp"
        def began_at(t):
            stamp = crashreport.datetime.datetime.fromtimestamp(t, crashreport.datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
            return BEGAN.replace("2026-10-23 22:13:02", stamp)
        self.log(began_at(now - 60) + CRASH + "220 tid=3 crash dump E:\\FH2\\data\\logs\\" + marker + ", 4897508 bytes\n" +
                 began_at(now - 10) + "running\n", now)
        self.write("data/logs/" + marker, b"MDMP", now - 19)
        mod = self.write("fh2-mods/crashes/matching.txt", "CRASH: matching failure", now - 20)
        found = crashreport.find_crash(self.folder, now - 100)
        self.assertIn("CRASHED exception", found.line)
        self.assertEqual(found.mod_crash, mod)
        # A report from a later session must not be paired with this older dump (#47).
        os.utime(mod, (now - 5, now - 5))
        self.assertIsNone(crashreport.find_crash(self.folder, now - 100).mod_crash)

    def test_no_folder(self):
        self.assertIsNone(crashreport.find_crash("", 0))
        self.assertIsNone(crashreport.find_crash(os.path.join(self.tmp, "nope"), 0))


class CheckTests(Base):
    def test_first_call_only_notes_the_time(self):
        self.log(BEGAN + CRASH)
        self.assertIsNone(crashreport.check(self.st, now=time.time() + 5))       # old crash from before reports existed
        self.assertGreater(self.st["crash_handled"], 0)
        self.log(BEGAN + CRASH, time.time() + 10)
        c = crashreport.check(self.st, now=time.time() + 20)
        self.assertIsNotNone(c)
        crashreport.handled(self.st, c)
        self.assertIsNone(crashreport.check(self.st))                          # not offered twice
        self.assertEqual(settings_mod.Settings(self.st.path)["crash_handled"], self.st["crash_handled"])    # and remembered

    def test_asking_off_offers_nothing_and_skips_what_happened_meanwhile(self):
        self.st["crash_handled"] = 1.0
        self.st["crash_report_ask"] = False
        self.log(BEGAN + CRASH)
        self.assertIsNone(crashreport.check(self.st))
        self.st["crash_report_ask"] = True
        self.assertIsNone(crashreport.check(self.st))


class ScrubTests(unittest.TestCase):
    def test_user_names_in_paths(self):
        cases = {
            r"Z:\home\ugandanhobo\Documents\azimuth.exe": r"Z:\home\<user>\Documents\azimuth.exe",
            r"C:\Users\John Smith\AppData\x.dmp": r"C:\Users\<user>\AppData\x.dmp",
            "/home/ugandanhobo/.local/share": "/home/<user>/.local/share",
            "/Users/anna/Library": "/Users/<user>/Library",
            r"c:/users/Bob/x": "c:/users/<user>/x",
            r"C:\Games\ForzaHorizon2\Mount": r"C:\Games\ForzaHorizon2\Mount",
        }
        for src, want in cases.items():
            self.assertEqual(crashreport.scrub(src), want)

    def test_session_token(self):
        self.assertEqual(crashreport.scrub("token fh2.abcdefghijklmnop0123456789 end"), "token <token> end")


class SecretTests(unittest.TestCase):
    """Audit 2026-10-25 F05: more than paths and FH2 tokens is blanked before a report is shown or sent."""

    def test_personal_data_and_secrets(self):
        cases = {
            "from pat@example.com": "from <email>",
            "peer 203.0.113.9:5000 and 127.0.0.1": "peer <ip>:5000 and 127.0.0.1",
            "driver 24.10.1.0, game 1.0.0.64": "driver 24.10.1.0, game 1.0.0.64",
            "dsn postgresql://fh2:pw@db/x": "dsn postgresql://<credentials>@db/x",
            "Authorization: Bearer abcdefghij": "Authorization: <secret>",
            "password='a b' rest": "password=<secret> rest",
            "seed: QmFzZTY0U2VjcmV0S2V5MTIzNDU2Nzg5MEFCQ0RFRkdISQ==": "seed: <secret>",
            "key QmFzZTY0U2VjcmV0S2V5MTIzNDU2Nzg5MEFCQ0RFRkdISQ== end": "key <secret> end",
            "C:/Games/ForzaHorizon2/Mount/mediaoverride/Stripped/gamedbRC.slt": "C:/Games/ForzaHorizon2/Mount/mediaoverride/Stripped/gamedbRC.slt",
        }
        for src, want in cases.items():
            self.assertEqual(crashreport.scrub(src), want, src)


class BuildTests(Base):
    def test_fields_files_and_payload(self):
        now = time.time()
        self.log(BEGAN + GPU + CRASH + "  Z:\\home\\ugandanhobo\\x\n", now)
        self.write("data/bin/fh2mp.log", "shim line\nsecond /home/ugandanhobo/y\n")
        self.write("fh2-mods/platform.log", "18:13:02 FH2 Mod Platform 2.7.1 in Z:\\home\\ugandanhobo\\x\n", now - 1)
        self.write("fh2-mods/crashes/crash-1.txt", "CRASH: access violation at game+0x1\ncall stack:\n", now)
        self.write("data/logs/a.dmp", b"MDMP" + b"\1" * 20, now)
        rep = crashreport.build(FakeLauncher(self.st), crashreport.find_crash(self.folder, now - 10))
        f = dict(rep.fields)
        self.assertEqual([k for k, _v in rep.fields], ["Date", "System", "GPU", "Driver's version", "Launcher version", "Azimuth version",
                                                       "FH2 Extra Options version", "Your FH2MP name"])
        self.assertEqual((f["System"], f["GPU"], f["Driver's version"]), ("Wine 11.0 (wine-11.0 (CachyOS)), CachyOS", "NVIDIA GeForce RTX 3080 Ti", "35.0.19.9999"))
        self.assertEqual((f["Your FH2MP name"], f["FH2 Extra Options version"]), ("Speedy", "not installed"))
        self.assertTrue(f["Date"].endswith(" UTC"))
        names = [n for n, _p, _s in rep.sizes()]
        self.assertEqual(names, ["azimuth.log", "crashdump", "fh2mp.log", "platform.log", "mod-crash.txt"])      # no launcher.log yet: not listed
        self.assertEqual(rep.payload()["files"]["mod-crash.txt"].splitlines()[0], "CRASH: access violation at game+0x1")
        body = rep.payload()
        self.assertEqual(body["v"], 1)
        self.assertEqual(set(body["files"]), {"azimuth.log", "fh2mp.log", "platform.log", "mod-crash.txt"})     # no launcher.log written in this test
        self.assertNotIn("ugandanhobo", "".join(body["files"].values()))
        preview = os.path.join(self.folder, "preview")
        for name, path, size in rep.sizes(preview):
            if name != "crashdump":                                   # the text files open as the exact text that is sent
                with open(path, encoding="utf-8", newline="") as f:
                    self.assertEqual((f.read(), os.path.dirname(path)), (body["files"][name], preview))
        self.assertEqual(base64.b64decode(body["dump"]), b"MDMP" + b"\1" * 20)
        self.assertEqual(body["fields"]["gpu"], "NVIDIA GeForce RTX 3080 Ti")

    def test_extra_logs_and_matching_platform_dump_are_collected(self):
        now = time.time()
        self.log(BEGAN + CRASH, now)
        self.write("data/logs/game.dmp", b"MDMPazimuth", now)
        self.write("FH2_ExtraOptions.log", "extra options diagnostics password=hidden")
        self.write("fh2-mods/Radio/radio.log", "radio diagnostics")
        self.write("fh2mp-session.txt", "SECRET SESSION CONTENT")
        mod = self.write("fh2-mods/crashes/crash-current.txt", "current stack", now)
        self.write("fh2-mods/crashes/crash-current.dmp", b"MDMPplatform", now)
        self.write("fh2-mods/crashes/crash-old.dmp", b"MDMPold", now - 100)
        found = crashreport.find_crash(self.folder, now - 10)
        self.assertEqual(found.mod_crash, mod)
        rep = crashreport.build(FakeLauncher(self.st), found)
        payload = rep.payload()
        text = "\n".join(payload["files"].values())
        self.assertIn("extra options diagnostics password=<secret>", text)
        self.assertIn("radio diagnostics", text)
        self.assertNotIn("SECRET SESSION CONTENT", text)
        self.assertEqual(base64.b64decode(payload["attachments"]["mod-crash.dmp"]), b"MDMPplatform")
        self.assertIn("mod-crash.dmp", [name for name, _, _ in rep.sizes()])
        self.assertLessEqual(sum(len(v.encode()) for v in payload["files"].values()), crashreport.MAX_TEXT)

    def test_normal_five_megabyte_dump_is_included(self):
        now = time.time()
        self.log(BEGAN + CRASH, now)
        data = b"MDMP" + b"x" * (4_897_508 - 4)
        self.write("data/logs/crash.dmp", data, now)
        rep = crashreport.build(FakeLauncher(self.st), crashreport.find_crash(self.folder, now - 10))
        self.assertEqual(base64.b64decode(rep.payload()["dump"]), data)
        self.assertIn("crashdump", [name for name, _, _ in rep.sizes()])

    def test_oversized_dump_has_an_explicit_reason(self):
        now = time.time()
        self.log(BEGAN + CRASH, now)
        self.write("data/logs/crash.dmp", b"MDMP" + b"x" * 100, now)
        with mock.patch.object(crashreport, "MAX_DUMP", 10):
            rep = crashreport.build(FakeLauncher(self.st), crashreport.find_crash(self.folder, now - 10))
        self.assertNotIn("dump", rep.payload())
        self.assertIn("exceeds", rep.payload()["dumpStatus"])
        self.assertTrue(any("not attached" in name for name, _, _ in rep.sizes()))

    def test_big_files_are_cut_to_their_end_and_a_huge_dump_stays_home(self):
        now = time.time()
        self.log(BEGAN + "x" * 100 + "\n" + "".join(f"{i} tid=1 line\n" for i in range(200_000)) + CRASH, now)
        self.write("data/logs/big.dmp", b"MDMP" + b"\0" * (crashreport.MAX_DUMP + 1), now)
        rep = crashreport.build(FakeLauncher(self.st), crashreport.find_crash(self.folder, now - 10))
        text = dict((n, t) for n, _p, t in rep.files)["azimuth.log"]
        self.assertLessEqual(len(text), crashreport.LIMITS["azimuth.log"])
        self.assertTrue(text.rstrip().endswith("(modified: Mount\\mediaoverride)"))
        self.assertRegex(text.splitlines()[0], r"^\d+ tid=1 line$")        # begins at a line start, not in the middle of one
        self.assertEqual(dict(rep.fields)["System"], "Wine 11.0 (wine-11.0 (CachyOS)), CachyOS")      # still known: read from the whole file
        self.assertIsNone(rep.dump)
        self.assertNotIn("dump", rep.payload())


if __name__ == "__main__":
    unittest.main()
