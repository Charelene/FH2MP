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
    def test_primary_failure_survives_crash_handler_failure_and_restart(self):
        primary = "10 tid=3 CRASHED exception 0xc0000005 at game+0x12e26dd writing 0x1234 on thread 3"
        secondary = "11 tid=4 CRASHED exception 0xc0000005 at azimuth+0x1d237d reading 0x4321 on thread 4"
        old = "1 tid=1 STOPPED old failure at azimuth+0x136b9b"
        log = old + "\n2 tid=1 session B began x\n" + primary + "\n" + secondary
        log += "\n12 tid=1 session C began x\nrunning\n"
        self.assertEqual(crash_reports.failure_line(log), primary.removeprefix("10 tid=3 "))
        self.assertEqual(crash_reports.signature(crash_reports.failure_line(log))[0], "0xc0000005@game+0x12e26dd")

    def test_azimuth_stops_have_stable_signatures(self):
        key, title = crash_reports.signature("STOPPED D3D12 descriptor blocks exhausted at azimuth+0x136b9b")
        self.assertIn("azimuth+0x136b9b", key)
        self.assertIn("descriptor blocks", title)

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
        self.assertIn(f"[launcher.log](https://forza.scuffed.af/panel/crashes/{rid}/launcher.log/download/)", body["body"])
        self.assertIn("Mod Platform crash report", body["body"])
        self.assertIn("0  game+0x2D8EAD", body["body"])
        self.assertIn(f"[platform.log](https://forza.scuffed.af/panel/crashes/{rid}/platform.log/download/)", body["body"])
        self.assertTrue(body["body"].startswith("This issue was created automatically by launcher's crash reporter."))
        self.assertNotIn("| Crash dump |", body["body"])
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

    def test_restart_does_not_hide_the_reported_crash_and_graphics_check_is_ignored(self):
        self.assertIn("CRASHED exception", crash_reports.failure_line("1 tid=1 session A began x\n" + CRASH + "\n2 tid=1 session B began x\nfine\n"))
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
        for leak in ("pat@example.com", "203.0.113.9", "word1", "bob@example.org", "198.51.100.4"):
            self.assertNotIn(leak, body)
        self.assertIn("<email>", body)
        self.assertIn("CRASHED exception 0xc0000005", body)

    def test_normal_azimuth_dump_is_retained_privately(self):
        dump = b"MDMP" + b"x" * (4_897_508 - 4)
        payload = dict(report(), dump=base64.b64encode(dump).decode())
        self.assertLess(len(json.dumps(payload).encode()), crash_reports.MAX_BODY)
        rid = self.cr.submit(payload)
        with open(os.path.join(self.dir, rid, "crash.dmp"), "rb") as f:
            self.assertEqual(f.read(), dump)
        self.assertIn(f"/{rid}/crash.dmp/download/)", self.gh.calls[-1][2]["body"])

    def test_missing_dump_has_no_placeholder_row_or_link(self):
        self.cr.submit(dict(report(dump=False), dumpStatus="Dump exceeds 8 MB limit"))
        body = self.gh.calls[-1][2]["body"]
        self.assertNotIn("Dump exceeds", body)
        self.assertNotIn("| Crash dump |", body)
        self.assertNotIn("crash.dmp]", body)

    def test_all_attachments_are_linked_in_issues_and_duplicate_comments(self):
        payload = report()
        payload['files']['extra-123-ExtraOptions.log'] = 'mod diagnostics'
        payload['attachments'] = {'mod-crash.dmp': base64.b64encode(b'MDMPplatform').decode()}
        rid = self.cr.submit(payload)
        body = self.gh.calls[-1][2]['body']
        for name in (*payload['files'], 'crash.dmp', 'mod-crash.dmp'):
            self.assertIn(f'/{rid}/{name}/download/)', body)
        self.assertEqual(self.cr.download(rid, 'mod-crash.dmp'), b'MDMPplatform')
        self.assertEqual(self.cr.download(rid, 'extra-123-ExtraOptions.log'), b'mod diagnostics')
        again = self.cr.submit(payload)
        comment = self.gh.calls[-1][2]['body']
        self.assertIn(f'/{again}/fh2mp.log/download/)', comment)
        self.assertIn(f'/{again}/mod-crash.dmp/download/)', comment)

    def test_legacy_log_labels_are_clean_but_download_paths_are_preserved(self):
        payload = report()
        payload['files'].update({'extra-0123456789-radio.log': 'first',
                                 'extra-abcdef0123-radio.log': 'second'})
        rid = self.cr.submit(payload)
        body = self.gh.calls[-1][2]['body']
        self.assertIn(f'[radio.log](https://forza.scuffed.af/panel/crashes/{rid}/extra-0123456789-radio.log/download/)', body)
        self.assertIn(f'[radio-2.log](https://forza.scuffed.af/panel/crashes/{rid}/extra-abcdef0123-radio.log/download/)', body)
        self.assertEqual(self.cr.download(rid, 'extra-abcdef0123-radio.log'), b'second')

    def test_infinity_logs_have_no_issue_or_comment_links(self):
        payload = report()
        for name in ('infinity.log', 'Infinity.previous.log',
                     'extra-6757afa7b2-infinity.log', 'extra-85c0e9bdbf-infinity.previous-4.log'):
            payload['files'][name] = 'private infinity diagnostics'
        self.cr.submit(payload)
        self.assertNotIn('infinity', self.gh.calls[-1][2]['body'].lower())
        self.cr.submit(payload)
        self.assertNotIn('infinity', self.gh.calls[-1][2]['body'].lower())

    def test_downloads_require_operator_auth_and_reject_path_escapes(self):
        import types
        import ops_api
        rid = self.cr.submit(report())
        lsp = types.SimpleNamespace(LAUNCHER=self.ctx if hasattr(self, 'ctx') else types.SimpleNamespace(crashes=self.cr),
                                    AUTH=None, ApiError=type('ApiError', (Exception,), {}))
        cfg = ops_api.OpsConfig('test-token')
        path = f'/ops/v1/crash-reports/{rid}/crash.dmp'
        call = lambda headers, peer='127.0.0.1', proxied=False: ops_api.handle(cfg, lsp, 'GET', path, headers, b'', peer, proxied)
        self.assertEqual(call({})[0], 401)
        self.assertEqual(call({'Authorization': 'Bearer test-token'}, '203.0.113.1')[0], 403)
        self.assertEqual(call({'Authorization': 'Bearer test-token'}, proxied=True)[0], 403)
        status, download = call({'Authorization': 'Bearer test-token'})
        self.assertEqual(status, 200)
        self.assertTrue(download.data.startswith(b'MDMP'))
        for name in ('../issues.json', 'report.json', '/etc/passwd', 'a/../../azimuth.log'):
            with self.assertRaises(auth.AuthError):
                self.cr.download(rid, name)
        os.symlink('/etc/passwd', os.path.join(self.dir, rid, 'linked.log'))
        with self.assertRaises(auth.AuthError):
            self.cr.download(rid, 'linked.log')

    def test_larger_dump_and_combined_limits(self):
        dump = b'MDMP' + b'x' * (13 << 20)
        payload = dict(report(), dump=base64.b64encode(dump).decode())
        self.assertGreater(len(json.dumps(payload)), 16 << 20)
        rid = self.cr.submit(payload)
        self.assertEqual(self.cr.download(rid, 'crash.dmp'), dump)
        from unittest import mock
        with mock.patch.object(crash_reports, 'MAX_DUMP', 150):
            payload = dict(report(), attachments={'mod-crash.dmp': base64.b64encode(b'MDMP'+b'x'*100).decode()})
            with self.assertRaises(auth.AuthError) as cm:
                crash_reports.validate(payload)
            self.assertEqual(cm.exception.status, 413)

    def test_rejects_bad_input(self):
        for bad, code in ((report(dump=False, files={"x.json": "y"}), 400), (report(dump=False, files={}), 400),
                          (report(dump=False, files={"azimuth.log": "x" * (crash_reports.MAX_FILE + 1)}), 413), ({"v": 2}, 400),
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

    def test_http_handler_uses_the_crash_limit_instead_of_the_game_limit(self):
        import types
        import fh2mp_lsp
        accepted, rejected = [], []
        size = 17 << 20
        handler = types.SimpleNamespace(path="/v1/crash-reports", headers={"Content-Length": str(size)},
                                        rfile=io.BytesIO(b"x" * size), _v1=lambda body: accepted.append(len(body)),
                                        _reply_json=lambda status, obj: rejected.append(status))
        fh2mp_lsp.Handler._handle(handler)
        self.assertEqual(accepted, [size])
        self.assertEqual(rejected, [])
        handler.path = "/v1/me"
        fh2mp_lsp.Handler._handle(handler)
        self.assertEqual(rejected, [413])


if __name__ == "__main__":
    unittest.main()
