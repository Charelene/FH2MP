"""Crash reports (/v1/crash-reports): checks, storage, and the GitHub issue (a fake GitHub, no network)."""
import base64
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crash_reports  # noqa: E402
import launcher_api  # noqa: E402
import auth  # noqa: E402

CRASH = "34275360 tid=2100 CRASHED exception 0xc0000005 at game+0xcf2dc3 reading 0x20 on thread 2100 (modified: Mount\\mediaoverride; WINMM.dll)"
STOP = "27814067 tid=2832 STOPPED CreatePlacementTexture2D requires GPU-write desc=0x14e7eec8 at game+0x10c897b (modified: x)"
LOG = "100 tid=1 session S began 2026-10-23 22:13:02 UTC: Azimuth 0.9.10 (azimuth.exe) on Wine 11.0, CachyOS\n" + CRASH + "\n"


def report(log=LOG, name="Speedy", dump=True, **extra):
    r = {"v": 1, "fields": {"date": "2026-10-23 22:13 UTC", "system": "Wine 11.0, CachyOS", "gpu": "NVIDIA RTX", "driver": "35.0",
                            "launcher": "1.0.0", "azimuth": "0.9.10", "extraOptions": "0.2.98", "name": name},
         "files": {"azimuth.log": log, "fh2mp.log": "a\nb\n", "launcher.log": "x", "platform.log": "p",
                                                         "mod-crash.txt": "CRASH: access violation at game+0x2D8EAD\ncall stack:\n   0  game+0x2D8EAD\n"}}
    if dump:
        r["dump"] = base64.b64encode(b"MDMP" + b"\0" * 100).decode()
    r.update(extra)
    return r


class FakeGitHub:
    """Stands in for urlopen: records the calls and answers like the issues API."""

    def __init__(self):
        self.calls, self.next, self.states = [], 1, {}

    def __call__(self, req, timeout=0):
        body = json.loads(req.data) if req.data else None
        path = req.full_url.split("/repos/", 1)[1]
        self.calls.append((req.get_method(), path, body))
        if req.get_method() == "POST" and path.endswith("/issues"):
            n, self.next = self.next, self.next + 1
            self.states[n] = "open"
            return io.BytesIO(json.dumps({"number": n}).encode())
        if req.get_method() == "GET":
            return io.BytesIO(json.dumps({"state": self.states[int(path.rsplit("/", 1)[1])]}).encode())
        return io.BytesIO(b"{}")


class CrashReportTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.gh = FakeGitHub()
        self.cr = crash_reports.CrashReports(self.dir, crash_reports.GitHub("o/r", "tok", self.gh), background=False)

    def test_stores_the_files_and_opens_an_issue(self):
        rid = self.cr.submit(report())
        d = os.path.join(self.dir, rid)
        self.assertEqual(sorted(os.listdir(d)), ["azimuth.log", "crash.dmp", "fh2mp.log", "launcher.log", "mod-crash.txt", "platform.log", "report.json"])
        with open(os.path.join(d, "report.json")) as f:
            self.assertEqual(json.load(f)["issue"], 1)
        method, path, body = self.gh.calls[-1]
        self.assertEqual((method, path), ("POST", "o/r/issues"))
        self.assertEqual(body["title"], "Crash 0xc0000005 at game+0xcf2dc3")
        self.assertIn("crash-report", body["labels"])
        self.assertIn(rid, body["body"])
        self.assertIn("`Speedy`", body["body"])
        self.assertNotIn("launcher.log", body["body"])
        self.assertIn("Mod Platform crash report", body["body"])
        self.assertIn("0  game+0x2D8EAD", body["body"])
        self.assertNotIn("platform.log", body["body"].replace("Platform", ""))
        self.assertLess(len(body["body"]), 65536)

    def test_same_crash_is_a_comment_until_the_issue_is_closed(self):
        self.cr.submit(report())
        self.cr.submit(report(name="Other"))
        self.assertEqual([c[0] for c in self.gh.calls], ["POST", "GET", "POST"])
        self.assertTrue(self.gh.calls[-1][1].endswith("/issues/1/comments"))
        self.assertIn("Other", self.gh.calls[-1][2]["body"])
        self.gh.states[1] = "closed"
        self.cr.submit(report())
        self.assertTrue(self.gh.calls[-1][1].endswith("/issues"))
        self.assertEqual(self.gh.next, 3)

    def test_another_address_is_another_issue(self):
        self.cr.submit(report())
        self.cr.submit(report(log="s\n" + STOP + "\n"))
        self.assertEqual(self.gh.next, 3)
        self.assertIn("Azimuth stop at game+0x10c897b", self.gh.calls[-1][2]["title"])
        self.assertNotIn("0x14e7eec8", self.gh.calls[-1][2]["title"])

    def test_only_the_last_session_counts_and_the_graphics_check_does_not(self):
        self.assertEqual(crash_reports.failure_line("1 tid=1 session A began x\n" + CRASH + "\n2 tid=1 session B began x\nfine\n"), "")
        self.assertEqual(crash_reports.failure_line(LOG), "CRASHED exception 0xc0000005 at game+0xcf2dc3 reading 0x20 on thread 2100")
        self.assertEqual(crash_reports.failure_line("1 tid=1 STOPPED CreateDXGIFactory2 failed at 0x1"), "")

    def test_text_cannot_break_out_of_the_issue(self):
        bad = LOG + "```\n</details>\n@everyone\n"
        self.cr.submit(report(log=bad, name="a|b`\n@x"))
        body = self.gh.calls[-1][2]["body"]
        self.assertIn("````text", body)                      # the fence is longer than the longest backtick run in the log
        self.assertIn("| Your FH2MP name | `a/b' @x` |", body)

    def test_public_issue_is_a_redacted_summary(self):
        """Audit 2026-10-25 F05: e-mail and IP addresses and secret-looking values reached the public issue; fh2mp.log is not published."""
        log = LOG + "contact pat@example.com from 203.0.113.9 recovery_key: word1 word2\n"
        self.cr.submit(report(log=log, **{"files": {"azimuth.log": log, "fh2mp.log": "lsp at 198.51.100.4 token=abc\n",
                                                    "mod-crash.txt": "CRASH at game+0x1 user bob@example.org\n"}}))
        body = self.gh.calls[-1][2]["body"]
        for leak in ("pat@example.com", "203.0.113.9", "word1", "bob@example.org", "198.51.100.4", "fh2mp.log"):
            self.assertNotIn(leak, body)
        self.assertIn("<email>", body)
        self.assertIn("CRASHED exception 0xc0000005", body)

    def test_rejects_bad_input(self):
        for bad, code in ((report(dump=False, files={"x.txt": "y"}), 400), (report(dump=False, files={}), 400),
                          (report(dump=False, files={"azimuth.log": "x" * 400_000}), 413), ({"v": 2}, 400),
                          (dict(report(), dump="not base64!"), 400), (dict(report(), dump=base64.b64encode(b"nope").decode()), 400)):
            with self.assertRaises(crash_reports.auth.AuthError) as cm:
                self.cr.submit(bad)
            self.assertEqual(cm.exception.status, code, bad)
        self.assertEqual(self.gh.calls, [])

    def test_github_failure_keeps_the_report(self):
        def boom(req, timeout=0):
            raise OSError("no route")
        cr = crash_reports.CrashReports(self.dir, crash_reports.GitHub("o/r", "tok", boom), background=False)
        rid = cr.submit(report())
        self.assertTrue(os.path.exists(os.path.join(self.dir, rid, "crash.dmp")))
        with open(os.path.join(self.dir, rid, "report.json")) as f:
            self.assertIsNone(json.load(f)["issue"])


class FakeAuth:
    """Only what the endpoint uses (no database needed): the real rate limiter."""
    scale = 1

    def __init__(self):
        self.limits = auth.RateLimiter()

    def hit(self, key, limit, window):
        return self.limits.hit(key, limit, window)


class CrashReportApiTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.gh = FakeGitHub()
        self.cr = crash_reports.CrashReports(self.dir, crash_reports.GitHub("o/r", "tok", self.gh), background=False)
        self.ctx = launcher_api.Ctx(FakeAuth(), None, launcher_api.LauncherConfig(), lambda: 0, crashes=self.cr)

    def post(self, body, ip="203.0.113.5"):
        return launcher_api.handle(self.ctx, "POST", "/v1/crash-reports", {}, json.dumps(body).encode(), ip)

    def test_needs_no_sign_in(self):
        status, obj, _t = self.post(report())
        self.assertEqual(status, 202)
        self.assertEqual(len(obj["id"]), 16)

    def test_off_when_not_configured(self):
        self.ctx.crashes = None
        self.assertEqual(self.post(report())[0], 503)

    def test_rate_limited_per_address(self):
        codes = [self.post(report())[0] for _ in range(7)]
        self.assertEqual(codes, [202] * 5 + [429] * 2)
        self.assertEqual(self.post(report(), ip="198.51.100.7")[0], 202)

    def test_bigger_body_only_for_this_endpoint(self):
        self.assertEqual(launcher_api.body_limit("/v1/crash-reports"), crash_reports.MAX_BODY)
        self.assertEqual(launcher_api.body_limit("/v1/me"), launcher_api.MAX_BODY)


if __name__ == "__main__":
    unittest.main()
