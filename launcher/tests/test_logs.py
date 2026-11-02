"""launcher.log (logs.py): the line shape, what the failure paths say, the run id across the hand-overs, and that nothing secret reaches the file."""
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fh2mp_launcher import api, core, keystore, logs, settings, shim
from fh2mp_launcher.api import ApiError
from fh2mp_launcher.core import PlayBlocked

LINE = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}[+-]\d{4} (TRACE|DEBUG|INFO|WARN|ERROR) \S+ \S+ run=[0-9a-f]{8}\b")


class Case(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.home, True)
        patch = mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=self.home, FH2MP_NO_KEYRING="1")
        patch.start()
        self.addCleanup(patch.stop)
        logs._every.clear()
        self.path = os.path.join(logs.paths.cache_dir(), "launcher.log")

    def text(self):
        try:
            return Path(self.path).read_text(encoding="utf-8")
        except FileNotFoundError:
            return ""


class Lines(Case):
    def test_shape_time_level_subsystem_event_run_fields(self):
        logs.info("play", "play.start", offline=True, skipped=None, note="two words")
        line = self.text().strip()
        self.assertRegex(line, LINE)
        self.assertIn(f"INFO play play.start run={logs.run()} offline=True note='two words'", line)
        self.assertNotIn("skipped", line)

    def test_levels_below_the_setting_are_not_written_and_a_subsystem_can_be_raised(self):
        logs.debug("api", "api.detail")
        self.assertEqual(self.text(), "")
        with mock.patch.object(logs, "_subs", {"api": logs.LEVELS["DEBUG"]}):
            logs.debug("api", "api.detail")
            logs.trace("api", "api.more")
            logs.debug("play", "play.detail")
        self.assertIn("api.detail", self.text())
        self.assertNotIn("api.more", self.text())
        self.assertNotIn("play.detail", self.text())

    def test_tokens_keys_and_the_home_folder_never_reach_the_file(self):
        home = os.path.expanduser("~")
        logs.warn("api", "x", detail="token=fh2.AbCdEfGhIjKlMnOp", header="Bearer abc.def", folder=os.path.join(home, "Games"), raw="fh2.AbCdEfGhIjKlMnOp", key="seed: c2VjcmV0")
        text = self.text()
        for secret in ("AbCdEfGh", "abc.def", "c2VjcmV0", home):
            self.assertNotIn(secret, text)
        self.assertIn("~", text)

    def test_a_repeating_condition_is_one_line_with_a_count(self):
        now = [100.0]
        with mock.patch.object(logs.time, "monotonic", lambda: now[0]):
            for _ in range(5):
                logs.every(60, "WARN", "api", "api.http_error", key="k", status=500)
            now[0] += 61
            logs.every(60, "WARN", "api", "api.http_error", key="k", status=500)
        rows = self.text().splitlines()
        self.assertEqual(len(rows), 2)
        self.assertIn("repeated=4", rows[1])

    def test_the_file_is_rotated_and_old_failure_helpers_still_write(self):
        with open(self.path, "w") as f:
            f.write("x" * (logs.MAX_BYTES + 1))
        api.log_failure("PLAY", "honk?secret=1", ValueError("x"), "broke")
        api.log_note("hello")
        self.assertTrue(os.path.exists(self.path + ".1"))
        self.assertIn("call.failed", self.text())
        self.assertIn("path=honk ", self.text())
        self.assertIn("text=hello", self.text())

    def test_a_log_that_cannot_be_written_never_raises(self):
        with mock.patch.object(logs.paths, "cache_dir", side_effect=OSError("no")):
            logs.error("api", "x")


class Failures(Case):
    def test_an_exception_is_written_once_with_its_stack(self):
        try:
            raise KeyError("boom")
        except KeyError as e:
            err = e
        logs.exc("ERROR", "game", "game.failed", err)
        logs.job_failed("job", err)                 # the layer above saw the same exception
        text = self.text()
        self.assertEqual(text.count("KeyError: 'boom'") - text.count("  KeyError"), 1)
        self.assertIn("  Traceback", text)
        self.assertNotIn("job.", text)

    def test_a_job_that_crashes_is_an_error_a_refusal_is_a_warning_without_a_stack(self):
        logs.job_failed("Main.play", PlayBlocked("Close the game", "game_running"))
        self.assertIn("WARN job job.failed", self.text())
        self.assertNotIn("Traceback", self.text())
        logs.job_failed("Main.play", ZeroDivisionError("bug"))
        self.assertIn("ERROR job job.crashed", self.text())

    def test_a_missing_file_is_normal_and_a_refused_one_is_not(self):
        logs.swallowed("shim", "shim.unreadable", FileNotFoundError("x"))
        self.assertEqual(self.text(), "")
        logs.swallowed("shim", "shim.unreadable", PermissionError(13, "denied"))
        self.assertIn("WARN shim shim.unreadable", self.text())

    def test_an_operation_says_how_far_it_got(self):
        with self.assertRaises(PlayBlocked):
            with logs.op("play", "play"):
                logs.step("server")
                logs.step("checks")
                raise PlayBlocked("Shim not installed.", "no_shim")
        line = [x for x in self.text().splitlines() if "play.blocked" in x][0]
        self.assertIn("WARN play play.blocked", line)
        self.assertRegex(line, r"steps=server:\d+,checks:\d+")
        self.assertIn("code=no_shim", line)
        self.assertIn("error='PlayBlocked: Shim not installed.", line)

    def test_an_unexpected_failure_in_an_operation_has_its_stack_and_a_success_a_total(self):
        with logs.op("play", "play"):
            logs.step("one")
        self.assertRegex(self.text(), r"play\.done run=\S+ elapsed_ms=\d+ steps=one:\d+")
        with self.assertRaises(ZeroDivisionError):
            with logs.op("play", "play"):
                1 / 0
        self.assertIn("ERROR play play.failed", self.text())
        self.assertIn("  Traceback", self.text())

    def test_a_settings_file_that_cannot_be_read_is_an_error_not_a_quiet_reset(self):
        p = os.path.join(self.home, "s.json")
        with open(p, "w") as f:
            f.write('{"azimuth_dir": "C:/secret-looking"')
        s = settings.Settings(p)
        self.assertEqual(s["azimuth_dir"], "")
        self.assertIn("ERROR settings settings.unreadable", self.text())
        self.assertNotIn("secret-looking", self.text())

    def test_no_settings_file_is_a_first_start(self):
        settings.Settings(os.path.join(self.home, "none.json"))
        self.assertEqual(self.text(), "")

    def test_a_damaged_key_file_is_an_error_and_its_content_is_not_logged(self):
        with open(keystore._file(), "w") as f:
            f.write('{"v": 1, "seed": "NOTBASE64SEEDVALUE!!!')
        self.assertIsNone(keystore.load())
        text = self.text()
        self.assertIn("ERROR keys keys.file_unreadable", text)
        self.assertNotIn("NOTBASE64", text)

    def test_a_sign_in_says_whether_it_ends_a_session_and_the_scrubber_leaves_the_line_readable(self):
        lc = mock.MagicMock(token="fh2.AbCdEfGhIjKlMnOp", account=None)
        lc.api.return_value.login.return_value = {"token": "fh2.ZyXwVuTsRqPoNmLk", "account": {"xuid": 0x90, "gamertag": "x"}}
        lc.game_running.return_value = True
        core.Launcher._sign_in(lc)
        line = self.text().strip()
        self.assertIn("auth.signed_in", line)
        self.assertIn("replacing_session=True game_running=True", line)      # a field called *_token= would be eaten by scrub()
        self.assertNotIn("redacted", line)
        self.assertNotIn("ZyXwVu", line)

    def test_exit_codes_read_as_ntstatus(self):
        lc = mock.Mock(game_started_at=None, offline_run=False)
        core.Launcher.note_game_exit(lc, -1073741819)
        self.assertIn("WARN game game.exited", self.text())
        self.assertIn("code_hex=0xc0000005", self.text())


class RunId(Case):
    def test_each_game_start_gets_its_own(self):
        a = logs.new_run()
        self.assertRegex(a, r"^[0-9a-f]{8}$")
        self.assertNotEqual(a, logs.new_run())

    def test_the_session_file_carries_it_to_the_shim(self):
        d = tempfile.mkdtemp(dir=self.home)
        shim.write_session(d, 0x90, "fh2.tok", "https://x.example", run="ab12cd34")
        text = Path(shim.bin_dir(d), shim.SESSION_FILE).read_text()
        self.assertIn("run=ab12cd34\n", text)
        self.assertRegex(logs.run(), r"^[0-9a-f]{8}$")


class ApiCalls(Case):
    """Against a real HTTP server on loopback: the id arrives as a header, a refusal is logged with what the server said."""

    def setUp(self):
        super().setUp()
        seen = self.seen = []

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append({k.lower(): v for k, v in self.headers.items()})
                code, body = (200, b'{"ok": 1}') if self.path == "/ok" else (401, b'{"error": "invalid_token"}')
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_a):
                pass
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.api = api.Api(f"http://127.0.0.1:{self.srv.server_address[1]}")

    def test_the_run_id_goes_with_the_call_and_a_refusal_is_logged_with_it(self):
        run = logs.new_run()
        self.assertEqual(self.api._call("GET", "/ok"), {"ok": 1})
        self.assertEqual(self.seen[0]["x-fh2mp-run"], run)
        with self.assertRaises(ApiError):
            self.api._call("GET", "/v1/me?name=x", token="fh2.AbCdEfGhIjKlMnOp")
        line = self.text().strip()
        self.assertIn(f"WARN api api.http_error run={run} method=GET path=/v1/me status=401 error=invalid_token elapsed_ms=", line)
        self.assertNotIn("AbCdEf", line)

    def test_a_url_of_another_host_gets_no_run_id(self):
        sent = []

        class Opener:
            def open(self, request, timeout=None):
                sent.append(request)
                raise OSError("stop here")
        with mock.patch.object(self.api, "_opener", return_value=Opener()):
            with self.assertRaises(ApiError):
                self.api._call("GET", "https://example.org/x")
            self.assertFalse(sent[-1].has_header("X-fh2mp-run"))
            with self.assertRaises(ApiError):
                self.api._call("GET", "/v1/status")
            self.assertTrue(sent[-1].has_header("X-fh2mp-run"))         # the check above can fail: our own API does get it


if __name__ == "__main__":
    unittest.main()
