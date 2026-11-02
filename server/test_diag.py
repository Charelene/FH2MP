"""Log levels, per-subsystem verbosity, rate limiting, exception text and secret scrubbing (diag.py) as the game server and the LSP use them."""
import contextlib
import io
import os
import sys
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import diag  # noqa: E402
import fh2mp_lsp as L  # noqa: E402
import fh2mp_server as S  # noqa: E402


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


def lines(fn):
    out = io.StringIO()
    with redirect_stdout(out):
        fn()
    return [x[9:] for x in out.getvalue().splitlines()]          # without "HH:MM:SS "


class Levels(unittest.TestCase):
    def setUp(self):
        self.old = (S.VERBOSITY, L.VERBOSITY)
        self.w = S.Server(0, None, sock=FakeSock(), log_file=None, tag="[freeroam:abc] ")
        S.VERBOSITY = L.VERBOSITY = diag.Verbosity("INFO", "")
        self.addCleanup(lambda: setattr(S, "VERBOSITY", self.old[0]) or setattr(L, "VERBOSITY", self.old[1]))

    def test_info_has_no_level_word_and_the_others_do(self):
        got = lines(lambda: (self.w.log("a"), self.w.warn("b"), self.w.error("c"), self.w.debug("grid", "d")))
        self.assertEqual(got, ["[freeroam:abc] a", "WARN [freeroam:abc] b", "ERROR [freeroam:abc] c"])      # DEBUG is off

    def test_one_subsystem_can_be_raised_alone(self):
        S.VERBOSITY = diag.Verbosity("INFO", "grid=DEBUG")
        got = lines(lambda: (self.w.debug("grid", "d"), self.w.debug("reliable", "e")))
        self.assertEqual(got, ["DEBUG [freeroam:abc] d"])

    def test_an_exception_line_says_what_was_being_done_where_and_why(self):
        def boom():
            raise ValueError("bad bits")
        try:
            boom()
        except ValueError as e:
            got = lines(lambda: self.w.exc("decoding GameOptions", e))
        self.assertTrue(got[0].startswith("ERROR [freeroam:abc] decoding GameOptions: ValueError: bad bits at test_diag.py:"), got[0])
        self.assertIn("in boom", got[0])
        self.assertTrue(any("Traceback" in x for x in got[1:]))

    def test_secrets_never_reach_a_line(self):
        got = lines(lambda: self.w.log("refused X-FH2MP-Token: fh2.abcdefghijklmnop for Bearer abc.def"))
        self.assertNotIn("fh2.abcdef", got[0])
        self.assertNotIn("abc.def", got[0])

    def test_a_repeating_condition_is_written_once_per_interval_and_counts_the_rest(self):
        t = [0.0]
        S.RATE = diag.RateLimit(clock=lambda: t[0])
        got = lines(lambda: [self.w.log_every("k", 10, "stalled") for _ in range(5)])
        self.assertEqual(got, ["WARN [freeroam:abc] stalled"])
        t[0] = 11
        got = lines(lambda: self.w.log_every("k", 10, "stalled"))
        self.assertEqual(got, ["WARN [freeroam:abc] stalled (and 4 more in the last 10 s)"])

    def test_the_lsp_uses_the_same_shape(self):
        L.CTX.seq = None
        got = lines(lambda: (L.log("x"), L.warn("y"), L.log("  indented"), L.log("z", "DEBUG")))
        self.assertEqual(got, ["x", "WARN y", "  indented"])


class ModuleLoggers(unittest.TestCase):
    def test_a_module_logger_writes_through_the_process_sink(self):
        got = []
        old = diag._sink
        diag.set_sink(lambda level, sub, msg: got.append((level, sub, msg)))
        self.addCleanup(lambda: diag.set_sink(old))
        log = diag.get("parts")
        log.warn("owner=3 slot 8 unknown tyre id")
        try:
            raise KeyError("x")
        except KeyError as e:
            log.exc("repairing parts", e, level="WARN")
        self.assertEqual(got[0], ("WARN", "parts", "parts: owner=3 slot 8 unknown tyre id"))
        self.assertTrue(got[1][2].startswith("parts: repairing parts: KeyError: 'x' at test_diag.py:"))


class Helpers(unittest.TestCase):
    def test_kv_quotes_only_what_needs_it(self):
        self.assertEqual(diag.kv(owner=3, why="no packets", gone=None, empty=""), "owner=3 why='no packets' empty=''")


class RunIdsAndTrace(unittest.TestCase):
    def test_a_run_id_is_8_hex_digits_with_an_optional_request_number(self):
        for good in ("ab12cd34", "ab12cd34.57", "00000000.4294967295"):
            self.assertEqual(diag.run_id(good), good)
        for bad in ("", "AB12CD34", "ab12cd3", "ab12cd345", "ab12cd34.", "ab12cd34.5 x", "ab12cd34\nfake line", "ab12cd34.12345678901"):
            self.assertEqual(diag.run_id(bad), "")

    def test_trace_is_off_at_every_ordinary_level_and_on_for_one_subsystem(self):
        self.assertFalse(diag.Verbosity("DEBUG", "").enabled("TRACE", "packet"))
        on = diag.Verbosity("INFO", "packet=TRACE")
        self.assertTrue(on.enabled("TRACE", "packet"))
        self.assertFalse(on.enabled("TRACE", "grid"))

    def test_a_failing_log_write_costs_a_note_on_stderr_not_the_caller(self):
        def full(_text):
            raise OSError(28, "No space left on device")
        diag._LOST.state.clear()
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            for _ in range(5):
                diag.write_line(full, "x")      # must not raise
        self.assertEqual(err.getvalue().count("log.write_failed"), 1)
        self.assertIn("No space left", err.getvalue())


if __name__ == "__main__":
    unittest.main()
