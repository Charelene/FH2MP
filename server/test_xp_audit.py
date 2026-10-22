"""xp_audit: the per-award rule, the rare-and-repeated suspect finder, and the lsp.log parser (no database needed)."""
import datetime
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xp_audit as X  # noqa: E402


def log_lines(entries):
    """lsp.log excerpt in the real format: entries are (HH:MM:SS, xuid, xp, total)."""
    out = []
    for i, (clock, xuid, xp, total) in enumerate(entries):
        out.append(f"{clock} #{1000 + i} POST gameservices.FH2.forzamotorsport.net/Services.FH2Release/o.xtsw Forza.WebServices.Scoreboard, "
                   f"Forza.WebServices SubmitXpScore body=4 user={xuid:#x}\n")
        out.append(f"{clock}   SubmitXpScore +{xp:g} -> total {total:g}\n")
    return out


class Judge(unittest.TestCase):
    def test_real_payouts_count(self):
        for xp in (0, 50, 7500, 15000, 34312, X.MAX_AWARD):
            self.assertEqual(X.judge(float(xp))[0], X.OK, xp)

    def test_impossible_awards_are_rejected(self):
        for xp in (X.MAX_AWARD + 1, 12_500_000.0, 130_000_000.0, -1.0, math.nan, math.inf):
            self.assertEqual(X.judge(xp)[0], X.REJECTED, xp)


class Suspects(unittest.TestCase):
    def test_a_rare_amount_repeated_by_one_account_is_a_suspect(self):
        awards = [(1, 12500.0)] * 168 + [(2, 12500.0)] * 2
        self.assertEqual(X.suspects(awards), {1: [(12500.0, 168, 2)]})

    def test_a_common_payout_is_not_a_suspect_however_often_one_account_gets_it(self):
        awards = [(1, 7500.0)] * 40 + [(u, 7500.0) for u in range(2, 40)]
        self.assertEqual(X.suspects(awards), {})

    def test_a_rare_amount_seen_a_few_times_is_not_a_suspect(self):
        self.assertEqual(X.suspects([(1, 10000.0)] * 3 + [(2, 10000.0)] * 2), {})


class Log(unittest.TestCase):
    def test_parse_attributes_awards_and_crosses_midnight(self):
        lines = log_lines([("23:59:58", 0x900001, 50, 50), ("00:00:03", 0x900002, 1.25e7, 1.25e7)])
        aw = X.parse_log(lines, datetime.date(2026, 10, 18))
        self.assertEqual([(a[0], a[1]) for a in aw], [(0x900001, 50.0), (0x900002, 12_500_000.0)])
        day1 = datetime.datetime.fromtimestamp(aw[0][3])          # the log's clock is the server's local time
        day2 = datetime.datetime.fromtimestamp(aw[1][3])
        self.assertEqual((day1.day, day2.day), (18, 19))

    def test_interleaved_requests_are_attributed_by_the_running_total(self):
        post = lambda c, x: f"{c} #1 POST g/o.xtsw Forza.WebServices.Scoreboard, Forza.WebServices SubmitXpScore body=4 user={x:#x}\n"   # noqa: E731
        award = lambda c, xp, total: f"{c}   SubmitXpScore +{xp:g} -> total {total:g}\n"                                         # noqa: E731
        lines = [post("10:00:00", 1), award("10:00:00", 100, 100), post("10:00:01", 2), award("10:00:01", 50, 50),
                 post("10:00:05", 1), post("10:00:05", 2), award("10:00:05", 1.25e7, 1.25e7 + 50), award("10:00:05", 7500, 7600)]
        got = [(u, xp) for u, xp, _, _ in X.parse_log(lines)]
        self.assertEqual(got, [(1, 100.0), (2, 50.0), (2, 12_500_000.0), (1, 7500.0)])

    def test_an_award_that_cannot_be_told_apart_is_left_unattributed(self):
        post = lambda x: f"10:00:00 #1 POST g SubmitXpScore body=4 user={x:#x}\n"                                               # noqa: E731
        lines = [post(1), post(2), "10:00:00   SubmitXpScore +50 -> total 50\n"]
        self.assertEqual(X.parse_log(lines)[0][0], None)
        self.assertIn("could not be attributed", X.report(X.audit(X.parse_log(lines))))

    def test_the_new_award_line_names_its_account(self):
        lines = ["10:00:00 #1 POST g SubmitXpScore body=4 user=0x1\n", "10:00:00 #2 POST g SubmitXpScore body=4 user=0x2\n",
                 "10:00:00   SubmitXpScore +50 -> total 50 user=0x2\n"]
        self.assertEqual(X.parse_log(lines)[0][0], 2)

    def test_rejected_lines_of_the_new_format_are_not_read_as_awards(self):
        lines = log_lines([("10:00:00", 1, 50, 50)])
        lines.append("10:00:01   SubmitXpScore +1.25e+07 REJECTED (above the largest possible award (56000)), total stays 50\n")
        self.assertEqual(len(X.parse_log(lines)), 1)

    def test_audit_keeps_legit_and_removes_impossible(self):
        aw = X.parse_log(log_lines([("10:00:00", 7, 7500, 7500), ("10:00:05", 7, 1.25e7, 1.25075e7), ("10:00:09", 7, 50, 1.250755e7)]))
        a = X.audit(aw)[7]
        self.assertEqual((a["counted"], a["rejected"], a["rejected_n"]), (7550.0, 12_500_000.0, 1))
        self.assertIn("legit total 7550", X.report(X.audit(aw)))


if __name__ == "__main__":
    unittest.main()
