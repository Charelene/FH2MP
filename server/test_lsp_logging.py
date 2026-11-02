"""The LSP's diagnostic lines: who and why a request was refused, rate limiting of repeated refusals, polls counted instead of listed, an exception
that leaves a handler. Portable (no PostgreSQL, no game catalog)."""
import contextlib
import io
import json
import os
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import auth  # noqa: E402
import diag  # noqa: E402
import fh2mp_lsp as L  # noqa: E402
import launcher_api  # noqa: E402
import launcher_lobbies  # noqa: E402
import ops_api  # noqa: E402


def lsp_lines(fn):
    """Lines the LSP writes to stdout while fn runs, without the clock."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        fn()
    return [x[9:] for x in out.getvalue().splitlines()]


class Capture:
    """Replaces the process sink of diag.get() loggers: [(level, sub, message)]."""

    def __init__(self):
        self.got = []

    def __enter__(self):
        self.old = diag._sink
        diag.set_sink(lambda level, sub, msg: self.got.append((level, sub, msg)))
        return self

    def __exit__(self, *exc):
        diag.set_sink(self.old)

    def text(self):
        return "\n".join(m for _l, _s, m in self.got)


class IdentifyRefusals(unittest.TestCase):
    """_identify refuses a request in several ways; each one now says who, which call and why (and never writes the token)."""

    def setUp(self):
        L.RATE.state.clear()
        self.before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim", "own_cars", "seq")}
        self.old = (L.LAUNCHER, L.AUTH, L.on_revoke)
        cfg = launcher_api.LauncherConfig()
        cfg.overlay = {"minShim": "1.1.2"}
        L.LAUNCHER = launcher_api.Ctx(None, None, cfg, lambda: 0)
        L.on_revoke = lambda xuid: None
        self.replies = []

        class Req:
            headers = {}
            client_ip = lambda self: "127.0.0.1"                                                     # noqa: E731
            _gamedb_ok = L.Handler._gamedb_ok
            _remove_old_client = L.Handler._remove_old_client
            _reply_json = lambda s, status, obj, *a, **k: self.replies.append((status, obj["error"]))  # noqa: E731
        self.Req = Req
        self.addCleanup(self.restore)

    def restore(self):
        L.LAUNCHER, L.AUTH, L.on_revoke = self.old
        for k, v in self.before.items():
            setattr(L.CTX, k, v)

    def ask(self, **headers):
        r = self.Req()
        r.headers = headers
        L.CTX.seq = 17
        return lsp_lines(lambda: self.replies.append(("identified", L.Handler._identify(r))))

    def test_an_old_shim_is_refused_with_the_versions_and_the_call_and_the_repeats_are_counted(self):
        L.AUTH = None
        first = self.ask(**{"X-FH2MP-Shim": "1.0.0", "X-ClassName": "Xls.WebServices.Notifications, X", "X-Method": "RetrieveForUser"})
        self.assertEqual(first, ["  #17 refused: shim '1.0.0' is older than 1.1.2 (426, call=Notifications.RetrieveForUser)"])
        again = self.ask(**{"X-FH2MP-Shim": "1.0.0", "X-ClassName": "Xls.WebServices.Notifications, X", "X-Method": "RetrieveForUser"})
        self.assertEqual(again, [])                                         # the same shim is not written again within a minute
        other = self.ask(**{"X-FH2MP-Shim": "1.1.0"})
        self.assertEqual(len(other), 1)                                     # another shim version is a different line
        self.assertIn("(none)", self.ask(**{"X-ClassName": "x"})[0])        # no shim header at all

    def test_a_dead_token_says_why_and_who_without_the_token(self):
        class FakeAuth:
            mode = "required"

            def authenticate(self, token, ip):
                raise auth.AuthError(401, "token_expired", "the session expired", xuid=0x900000000abc, detail="idle limit passed 340 s ago")
        L.AUTH = FakeAuth()
        lines = self.ask(**{"X-FH2MP-Shim": "1.1.2", "X-FH2MP-Token": "fh2.SECRETSECRETSECRET", "X-ClassName": "Forza.WebServices.Scoreboard", "X-Method": "SubmitScore"})
        self.assertEqual(self.replies[-1], ("identified", False))
        self.assertEqual(self.replies[0], (401, "token_expired"))
        self.assertEqual(len(lines), 1)
        for part in ("refused: 401 token_expired on Scoreboard.SubmitScore", "xuid=0x900000000abc", "shim=1.1.2", "detail='idle limit passed 340 s ago'"):
            self.assertIn(part, lines[0])
        self.assertNotIn("SECRET", lines[0])
        again = {"X-FH2MP-Shim": "1.1.2", "X-FH2MP-Token": "fh2.SECRETSECRETSECRET", "X-ClassName": "Forza.WebServices.Scoreboard", "X-Method": "SubmitScore"}
        self.assertEqual(self.ask(**again), [])                             # one line per code, player and call
        other = self.ask(**dict(again, **{"X-ClassName": "Forza.WebServices.Storefront, Forza.WebServices", "X-Method": "GetCuratedContent"}))
        self.assertEqual(len(other), 1)                                     # a refused request has no request line: each lost call is named
        self.assertIn("on Storefront.GetCuratedContent", other[0])
        self.assertEqual(len(self.ask(**dict(again, **{"X-Method": "MadeUp1"}))), 1)       # calls without a handler share one line,
        self.assertEqual(self.ask(**dict(again, **{"X-Method": "MadeUp2"})), [])          # so made-up names cannot fill the log

    def test_a_dead_unknown_token_names_the_claimed_user(self):
        class FakeAuth:
            mode = "required"

            def authenticate(self, token, ip):
                raise auth.AuthError(401, "invalid_token", "session ended", detail="no live session for this token")
        L.AUTH = FakeAuth()
        lines = self.ask(**{"X-FH2MP-Shim": "1.1.3", "X-FH2MP-Token": "fh2.SECRETSECRETSECRET", "X-UserId": "0x900000000abd",
                            "X-ClassName": "Xls.WebServices.Notifications, Xls.WebServices", "X-Method": "RetrieveForUser"})
        self.assertEqual(self.replies[0], (401, "invalid_token"))
        self.assertIn("claims='0x900000000abd (unverified)'", lines[0])

    def test_a_missing_token_names_the_call_the_shim_and_the_claimed_user(self):
        class FakeAuth:
            mode = "required"
        L.AUTH = FakeAuth()
        lines = self.ask(**{"X-FH2MP-Shim": "1.1.2", "X-UserId": "0x9000000001234", "X-FH2MP-Host": "momatch.xboxlive.com"})
        self.assertEqual(self.replies[0], (401, "login_required"))
        self.assertIn("refused: 401 login_required: no X-FH2MP-Token on momatch.xboxlive.com", lines[0])
        self.assertIn("claims user 0x9000000001234, unverified", lines[0])

    def test_a_banned_account_without_a_token_is_refused_by_name_and_through_which_header(self):
        class FakeAuth:
            mode = "optional"

            def ban_text(self, xuid):
                return "This account is banned permanently. Reason: griefing" if xuid == 0x900000000777 else None
        L.AUTH = FakeAuth()
        lines = self.ask(**{"X-FH2MP-Shim": "1.1.2", "X-UserId": "0x900000000777", "X-ClassName": "Forza.WebServices.Scoreboard", "X-Method": "SubmitXpScore"})
        self.assertEqual(self.replies[0], (403, "banned"))
        self.assertIn("refused: 403 banned (no token, named by X-UserId 0x900000000777) on Scoreboard.SubmitXpScore: This account is banned permanently. Reason: griefing", lines[0])


class AuthErrorsCarryTheirContext(unittest.TestCase):
    """AuthService.authenticate attaches the account and the compared values to what it raises (for the log, never the client)."""

    class FakeDb:
        def __init__(self, row):
            self.row = row

        def one(self, sql, args=()):
            return self.row if "auth_sessions" in sql else None

        def tx(self):
            raise AssertionError("not used")

        def meta_get(self, key):
            return "s"

    def service(self, row, now=1000.0, **kw):
        os.environ["FH2MP_AUTH_SECRET"] = "test"
        self.addCleanup(os.environ.pop, "FH2MP_AUTH_SECRET", None)
        return auth.AuthService(self.FakeDb(row), env="test", mode="required", clock=lambda: now, **kw)

    def session(self, **over):
        return {"xuid": 0x900000000aaa, "ip": "10.0.0.1", "expires": 2000.0, "hard_expires": 3000.0, "last_used": 900.0, "gamertag": "Tester",
                "banned_at": None, "ban_until": None, "ban_reason": "", **over}

    def test_expired_names_the_account_and_how_long_ago(self):
        svc = self.service(self.session(expires=660.0))
        with self.assertRaises(auth.AuthError) as ctx:
            svc.authenticate("fh2.abcdef", "10.0.0.1")
        e = ctx.exception
        self.assertEqual((e.status, e.code, e.xuid), (401, "token_expired", 0x900000000aaa))
        self.assertEqual(e.detail, "idle limit passed 340 s ago")

    def test_banned_and_changed_address_name_the_account(self):
        svc = self.service(self.session(banned_at=1.0, ban_reason="griefing"))
        with self.assertRaises(auth.AuthError) as ctx:
            svc.authenticate("fh2.abcdef", "10.0.0.1")
        self.assertEqual((ctx.exception.code, ctx.exception.xuid), ("banned", 0x900000000aaa))
        svc = self.service(self.session(), token_ip="exact")
        with self.assertRaises(auth.AuthError) as ctx:
            svc.authenticate("fh2.abcdef", "10.0.0.2")
        self.assertEqual((ctx.exception.code, ctx.exception.xuid), ("ip_changed", 0x900000000aaa))
        self.assertNotIn("10.0.0", ctx.exception.detail)                    # no addresses in log text

    def test_an_unknown_token_says_it_found_no_session_and_a_malformed_one_says_what_is_wrong(self):
        svc = self.service(None)
        with self.assertRaises(auth.AuthError) as ctx:
            svc.authenticate("fh2.abcdef", "10.0.0.1")
        self.assertIn("no live session for this token", ctx.exception.detail)
        with self.assertRaises(auth.AuthError) as ctx:
            svc.authenticate("Bearer-something", "10.0.0.1")
        self.assertEqual(ctx.exception.detail, "not a session token: 16 characters, no fh2. prefix")
        self.assertNotIn("something", ctx.exception.detail)


class LauncherApiRefusals(unittest.TestCase):
    def setUp(self):
        self.clock = [0.0]
        launcher_api._RATE = diag.RateLimit(clock=lambda: self.clock[0])
        self.addCleanup(setattr, launcher_api, "_RATE", diag.RateLimit())

        class FakeAuth:
            mode, env, registration = "required", "test", "open"

            def authenticate(self, token, ip):
                raise auth.AuthError(401, "invalid_token", detail="no live session for this token")

            def hit(self, *a):
                return True
        self.ctx = launcher_api.Ctx(FakeAuth(), None, launcher_api.LauncherConfig(), lambda: 0)

    def call(self, path="/v1/community"):
        return launcher_api.handle(self.ctx, "GET", path, {"Authorization": "Bearer fh2.TOPSECRETVALUE"}, b"", "127.0.0.1")

    def test_a_refusal_is_logged_with_its_reason_once_per_window_and_counts_the_repeats(self):
        with Capture() as cap:
            self.assertEqual(self.call()[0], 401)
            self.call()
            self.call()
            self.assertEqual(len(cap.got), 1)
            level, sub, msg = cap.got[0]
            self.assertEqual((level, sub), ("WARN", "launcher"))
            self.assertIn("refused 401 invalid_token: GET /v1/community", msg)
            self.assertIn("detail='no live session for this token'", msg)
            self.assertNotIn("TOPSECRET", msg)
            self.clock[0] += launcher_api.REFUSAL_LOG_SECONDS + 1
            self.call()
            self.assertIn("(and 2 more in the last 120 s)", cap.got[-1][2])

    def test_ids_in_the_path_do_not_make_every_request_a_new_line(self):
        with Capture() as cap:
            self.call("/v1/community/chat/101")
            self.call("/v1/community/chat/202")
        self.assertEqual(len(cap.got), 1)
        self.assertIn("/v1/community/chat/N", cap.got[0][2])

    def test_a_players_own_mistake_is_info_and_a_server_problem_warn(self):
        with Capture() as cap:
            launcher_api._note_refusal("POST", "/v1/community/friends", auth.AuthError(404, "player_not_found", "No player has that gamertag or ID.", detail="no unbanned account matches 'x' as a gamertag"))
            launcher_api._note_refusal("GET", "/v1/community/lobbies", auth.AuthError(503, "unavailable", "The game server did not answer.", xuid=0x9000000000001))
        self.assertEqual([l for l, _s, _m in cap.got], ["INFO", "WARN"])
        self.assertIn("xuid=0x9000000000001", cap.got[1][2])

    def test_a_config_file_that_no_longer_parses_is_reported_once_and_the_old_settings_stay(self):
        path = os.path.join(tempfile.mkdtemp(), "launcher.json")
        with open(path, "w") as f:
            json.dump({"minShim": "1.1.2"}, f)
        cfg = launcher_api.LauncherConfig(path)
        self.assertEqual(cfg.get()["minShim"], "1.1.2")
        with open(path, "w") as f:
            f.write('{"minShim": "1.1.3",')
        os.utime(path, (1, 2))
        with Capture() as cap:
            self.assertEqual(cfg.get()["minShim"], "1.1.2")
            cfg.get()
            cfg.get()
        self.assertEqual(len(cap.got), 1)
        self.assertEqual(cap.got[0][0], "WARN")
        self.assertIn("not reloaded, the last good settings stay in force (minShim '1.1.2'", cap.got[0][2])
        self.assertIn("JSONDecodeError", cap.got[0][2])


class OpsRefusals(unittest.TestCase):
    def test_a_wrong_token_is_logged_without_the_token_and_without_an_address(self):
        ops_api._RATE = diag.RateLimit()
        lsp = types.SimpleNamespace(AUTH=types.SimpleNamespace(hit=lambda *a: True), ApiError=L.ApiError, log=lambda m: None)
        cfg = ops_api.OpsConfig("the-real-token", allow=("10.0.0.0/8",))
        with Capture() as cap:
            status, _ = ops_api.handle(cfg, lsp, "GET", "/ops/v1/players?q=Alice", {"Authorization": "Bearer guess-guess-guess"}, b"", "10.1.2.3", False)
            ops_api.handle(cfg, lsp, "GET", "/ops/v1/players?q=Bob", {"Authorization": "Bearer another-guess"}, b"", "10.1.2.3", False)
        self.assertEqual(status, 401)
        self.assertEqual(len(cap.got), 1)                                   # one line per code and route
        level, sub, msg = cap.got[0]
        self.assertEqual((level, sub), ("WARN", "ops"))
        self.assertIn("refused 401 unauthorized: GET /ops/v1/players", msg)
        self.assertIn("the bearer token does not match (17 characters sent)", msg)
        for secret in ("guess-guess", "the-real-token", "10.1.2.3", "Alice"):
            self.assertNotIn(secret, msg)

    def test_a_request_through_the_proxy_says_so(self):
        ops_api._RATE = diag.RateLimit()
        lsp = types.SimpleNamespace(AUTH=None, ApiError=L.ApiError, log=lambda m: None)
        with Capture() as cap:
            ops_api.handle(ops_api.OpsConfig("t"), lsp, "GET", "/ops/v1/overview", {}, b"", "127.0.0.1", True)
        self.assertIn("came through the public proxy", cap.got[0][2])

    def test_an_unexpected_error_is_logged_with_its_stack_not_printed_to_stderr(self):
        ops_api._RATE = diag.RateLimit()
        broken = types.SimpleNamespace(AUTH=None, ApiError=L.ApiError, log=lambda m: None)         # no DB, SB ...: overview() raises AttributeError
        err = io.StringIO()
        with Capture() as cap, contextlib.redirect_stderr(err):
            status, obj = ops_api.handle(ops_api.OpsConfig("t"), broken, "GET", "/ops/v1/overview", {"Authorization": "Bearer t"}, b"", "127.0.0.1", False)
        self.assertEqual((status, obj["error"]), (500, "internal"))
        self.assertEqual(err.getvalue(), "")
        self.assertEqual(cap.got[0][0], "ERROR")
        self.assertIn("unhandled error in GET /ops/v1/overview, answered 500: AttributeError", cap.got[0][2])
        self.assertTrue(any("Traceback" in m for _l, _s, m in cap.got))


class PollsAreCountedNotListed(unittest.TestCase):
    def test_one_summary_line_per_period(self):
        now, lines = [0.0], []
        polls = L.PollShare(period=600, clock=lambda: now[0], sink=lines.append)
        for _ in range(5):
            polls.note("Notifications.RetrieveForUser", 1)
        polls.note("Notifications.RetrieveForUser", 2)
        polls.note("userpresence.xboxlive.com", 1)
        self.assertEqual(lines, [])
        now[0] = 601
        polls.note("Notifications.RetrieveForUser", 3)
        self.assertEqual(lines, ["polls: last 601 s: Notifications.RetrieveForUser 6 requests from 2 players, userpresence.xboxlive.com 1 requests from 1 players"])
        now[0] = 1300
        polls.note("Notifications.RetrieveForUser", 3)
        self.assertEqual(len(lines), 2)                                     # the next period starts with the poll that closed the last one

    def test_an_unchanged_answer_is_not_news(self):
        L._LAST_POLL.clear()
        self.assertFalse(L._unchanged(("momatch", 1, "/t/1"), (200, 5)))
        self.assertTrue(L._unchanged(("momatch", 1, "/t/1"), (200, 5)))
        self.assertFalse(L._unchanged(("momatch", 1, "/t/1"), (200, 6)))
        self.assertFalse(L._unchanged(("momatch", 2, "/t/1"), (200, 6)))     # another player


class AnExceptionThatLeavesAHandler(unittest.TestCase):
    def handler(self, boom):
        h = types.SimpleNamespace(command="POST", path="/titles/1/x?token=nope", headers={"X-ClassName": "Forza.WebServices.Scoreboard, F", "X-Method": "SubmitScore"})
        h._handle = boom
        return h

    def test_it_is_logged_with_the_request_number_the_call_and_the_time_then_reraised(self):
        def boom():
            L.CTX.seq = 99
            raise KeyError("scoreboardType")
        h = self.handler(boom)
        err = io.StringIO()
        raised = []

        def run():
            try:
                L.Handler._entry(h)
            except KeyError as e:
                raised.append(e)
        with contextlib.redirect_stderr(err):
            lines = lsp_lines(run)
        self.assertEqual(len(raised), 1)
        self.assertTrue(raised[0]._fh2mp_logged)
        self.assertTrue(lines[0].startswith("ERROR   #99 unhandled error in POST /titles/1/x Scoreboard.SubmitScore after 0.0 s;"), lines[0])
        self.assertIn("KeyError: 'scoreboardType'", lines[0])
        self.assertNotIn("token=nope", "\n".join(lines))                    # the query string stays out of the log
        self.assertTrue(any("Traceback" in x or "KeyError" in x for x in lines[1:]))

    def test_the_server_does_not_print_it_a_second_time(self):
        srv = L.BoundedHTTPServer.__new__(L.BoundedHTTPServer)
        try:
            raise ValueError("x")
        except ValueError as e:
            e._fh2mp_logged = True
            self.assertEqual(lsp_lines(lambda: L.BoundedHTTPServer.handle_error(srv, None, ("127.0.0.1", 1))), [])

    def test_a_client_that_hung_up_is_not_an_error(self):
        def gone():
            L.CTX.seq = 5
            raise BrokenPipeError(32, "Broken pipe")
        L.RATE.state.clear()
        lines = lsp_lines(lambda: self.assertRaises(BrokenPipeError, L.Handler._entry, self.handler(gone)))
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("  #5 client went away or stalled before the exchange finished (BrokenPipeError)"), lines[0])


class XlsAnswersExplainThemselves(unittest.TestCase):
    """handle_xls: an error result, an unknown call and an unimplemented call each say which call they are about."""

    class Schema:
        def call_struct(self, name):
            if name.startswith("Known"):
                return []
            raise KeyError(name)

    def setUp(self):
        self.old = (L.SC, dict(L.HANDLERS))
        L.SC = self.Schema()
        L.RATE.state.clear()
        self.addCleanup(setattr, L.CTX, "seq", getattr(L.CTX, "seq", None))
        L.CTX.seq = None
        self.addCleanup(self.restore)

    def restore(self):
        L.SC = self.old[0]
        L.HANDLERS.clear()
        L.HANDLERS.update(self.old[1])

    def test_an_error_result_carries_the_code_and_the_call(self):
        def refuse(req):
            raise L.XlsError("no such livery 5", 0x1)
        L.HANDLERS[("Known.Service", "Fetch")] = ("", refuse)
        lines = lsp_lines(lambda: L.handle_xls("Known.Service, Known", "Fetch", b""))
        self.assertEqual(lines, ["  -> error result: no such livery 5 (error 0x1, call=Service.Fetch)"])

    def test_an_unknown_call_is_reported_once_and_says_it_was_answered_empty(self):
        first = lsp_lines(lambda: L.handle_xls("Other.Service, X", "Mystery", b""))
        self.assertEqual(len(first), 1)
        self.assertTrue(first[0].startswith("WARN   no schema for Other.Service, X Mystery; answering OK with empty body"), first[0])
        self.assertEqual(lsp_lines(lambda: L.handle_xls("Other.Service, X", "Mystery", b"")), [])          # not again within ten minutes

    def test_an_unimplemented_call_names_itself(self):
        lines = lsp_lines(lambda: L.handle_xls("Forza.WebServices.Gifting, Forza", "SendCarGift", b""))
        self.assertEqual(lines, ["  not implemented -> error result (client keeps its local data) call=Gifting.SendCarGift"])


class ReservationsSayWhatBecameOfThem(unittest.TestCase):
    def test_an_unused_hold_is_reported_when_it_expires(self):
        launcher_lobbies.HOLDS.clear()
        launcher_lobbies.HOLDS["77"] = {"id": "abcdef12-0000", "kind": "freeroam", "expiresAt": 100, "key": None}
        with Capture() as cap:
            launcher_lobbies.prune(now=200)
        self.assertEqual(launcher_lobbies.HOLDS, {})
        self.assertEqual(cap.got[0][:2], ("INFO", "lobbies"))
        self.assertIn("reservation expired unused xuid=77 lobby=abcdef12 kind=freeroam held_s=300", cap.got[0][2])


class ModuleLinesCarryTheRequestNumber(unittest.TestCase):
    def test_a_diag_logger_line_written_during_a_request_gets_its_number(self):
        before = getattr(L.CTX, "seq", None)
        self.addCleanup(setattr, L.CTX, "seq", before)
        sink = diag._sink
        self.addCleanup(diag.set_sink, sink)
        diag.set_sink(L._module_sink)                   # other test modules point the process sink at a game server's log
        L.CTX.seq = 31
        lines = lsp_lines(lambda: diag.get("auth").warn("audit event 'login' not written"))
        self.assertEqual(lines, ["WARN   #31 auth: audit event 'login' not written"])
        L.CTX.seq = None
        self.assertEqual(lsp_lines(lambda: diag.get("auth").warn("x")), ["WARN auth: x"])


if __name__ == "__main__":
    unittest.main()
