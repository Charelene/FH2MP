"""Log levels, per-subsystem verbosity, rate limiting, exception text and secret scrubbing (diag.py) as the game server and the LSP use them."""
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


class Helpers(unittest.TestCase):
    def test_kv_quotes_only_what_needs_it(self):
        self.assertEqual(diag.kv(owner=3, why="no packets", gone=None, empty=""), "owner=3 why='no packets' empty=''")


if __name__ == "__main__":
    unittest.main()
