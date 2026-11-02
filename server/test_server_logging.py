"""Game server logging: the lines that say why a client was refused, why it was removed, what an event start decided, and that a failure which can
repeat on every tick or datagram is logged once with a count and never leaks a secret."""
import contextlib
import io
import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
import fh2mp_server as S  # noqa: E402
import test_security as T  # noqa: E402
from test_freeroam_tag_grant import options, world  # noqa: E402


def logged_world():
    """A world that writes real log lines (with their level word) into a buffer."""
    w, peers = world()
    w.log_file = io.StringIO()
    w.tag = "[test:%x] " % id(w)           # rate limits are keyed by world; tests must not share them
    return w, peers


def lines(w):
    return w.log_file.getvalue().splitlines()


def hub_with_log():
    hub = T.make_hub()
    hub.log_file = hub.idle.log_file = io.StringIO()
    return hub


def hub_lines(hub):
    return hub.log_file.getvalue().splitlines()


class Fresh(unittest.TestCase):
    def setUp(self):
        S.RATE.state.clear()        # rate limits are per process; another test's line must not hide this one


class Refusals(Fresh):
    def test_a_wrong_version_is_a_warning_naming_both_versions(self):
        hub = hub_with_log()
        pkt = S.build_packet([lambda w: T.open_msg(w, 0x9000000000001, version="OLDVERS1")])
        hub.handle(pkt, ("10.5.0.1", 4000))
        got = [x for x in hub_lines(hub) if "REFUSED" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])
        self.assertIn("'OLDVERS1'", got[0])
        self.assertIn(repr(S.VERSION_STRING), got[0])
        self.assertIn("-> result 1", got[0])

    def test_a_revoked_account_is_refused_with_its_reason_once_per_address(self):
        hub = hub_with_log()
        hub.revoked[0x9000000000002] = time.monotonic() + 100
        for _ in range(3):
            hub.handle(T.open_packet(0x9000000000002), ("10.5.0.2", 4000))
        got = [x for x in hub_lines(hub) if "refused OpenConnection" in x]
        self.assertEqual(len(got), 1)
        self.assertIn("revoked", got[0])
        self.assertIn("0x9000000000002", got[0])

    def test_a_handshake_that_never_completes_is_reported_when_it_is_forgotten(self):
        hub = hub_with_log()
        addr = ("10.5.0.3", 4000)
        hub.handle(T.open_packet(0x9000000000003), addr)
        world_ = hub.route[addr]
        world_.peers[addr].last_rx -= S.PENDING_SECONDS + 5
        world_.tick()
        self.assertNotIn(addr, world_.peers)
        got = [x for x in hub_lines(hub) if "handshake abandoned" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])
        self.assertIn("0x9000000000003", got[0])
        self.assertIn("result 0", got[0])

    def test_a_flood_from_one_address_is_refused_with_the_limits(self):
        hub = hub_with_log()
        for i in range(S.MAX_PENDING_PER_IP + 3):
            hub.handle(T.open_packet(0x9000000000100 + i), ("10.5.0.4", 4000 + i))
        got = [x for x in hub_lines(hub) if "too many half-open" in x]
        self.assertEqual(len(got), 1)                       # one line per address per 10 s
        self.assertIn(str(S.MAX_PENDING_PER_IP), got[0])


class Removal(Fresh):
    def test_the_removal_line_has_the_age_of_the_last_packet_and_the_state(self):
        w, (a, b) = logged_world()
        a.xuid = 0x9000000000001
        a.connected_at = time.monotonic() - 42
        a.last_rx = time.monotonic() - 19.5
        w.drop_peer(a, "no packets for 20 s")
        got = [x for x in lines(w) if " removed (" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])                    # a client that vanished without closing is not routine
        self.assertIn("xuid 0x9000000000001", got[0])
        self.assertIn("last packet 19.5 s ago", got[0])
        self.assertIn("connected 42 s", got[0])
        self.assertIn("free roam", got[0])

    def test_a_clean_close_is_info_and_a_participant_is_said_to_be_in_the_event(self):
        w, (a, b) = logged_world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        w.drop_peer(a, "CloseConnection")
        got = [x for x in lines(w) if " removed (CloseConnection)" in x]
        self.assertEqual(len(got), 1)
        self.assertNotIn(" WARN ", got[0])
        self.assertIn("in the event", got[0])

    def test_a_kick_says_who_and_why(self):
        w, (a, b) = logged_world()
        a.kick_reason = "car 1234 (Hoonigan)"
        a.xuid = 0x9000000000001
        w.kick_for_cars()
        got = [x for x in lines(w) if "kick:" in x]
        self.assertEqual(len(got), 1)
        self.assertIn("owner 1", got[0])
        self.assertIn("car 1234 (Hoonigan)", got[0])
        self.assertIn(" WARN ", got[0])

    def test_malformed_messages_are_logged_once_and_counted_in_the_removal_line(self):
        w, (a, b) = logged_world()
        w.rx = (a, 120, [3, 7, 8])
        for _ in range(5):
            w.malformed(a, "object router=2 id=70 type=None")
        self.assertEqual(len([x for x in lines(w) if "malformed" in x and "dropped with the rest" in x]), 1)
        first = [x for x in lines(w) if "malformed" in x][0]
        self.assertIn("120 bytes", first)
        self.assertIn("[3, 7, 8]", first)
        w.drop_peer(a, "CloseConnection")
        self.assertIn("5 malformed message(s)", [x for x in lines(w) if " removed (" in x][0])


class Failures(Fresh):
    def test_a_truncated_message_is_one_warning_per_address_naming_the_type(self):
        w, (a, b) = logged_world()
        data = S.build_packet([lambda m: m.bits(S.MSG_OBJECT_REQUEST_OWNERSHIP, 7)])
        with self.assertRaises(ValueError) as caught:
            w.handle(data, a.addr)
        for _ in range(4):
            w.handle_failed(caught.exception, data, a.addr)
        got = [x for x in lines(w) if "truncated message" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])
        self.assertIn(f"failed on type {S.MSG_OBJECT_REQUEST_OWNERSHIP}", got[0])
        self.assertIn(f"{len(data)} bytes", got[0])

    def test_an_unexpected_error_is_an_error_with_a_stack_and_is_not_repeated(self):
        w, (a, b) = logged_world()

        def boom():
            raise RuntimeError("secret state broke")
        errors = []
        for _ in range(3):
            try:
                boom()
            except RuntimeError as exc:
                errors.append(exc)
                w.handle_failed(exc, b"\x00" * 12, a.addr)
        text = lines(w)
        heads = [x for x in text if "error handling a datagram" in x]
        self.assertEqual(len(heads), 1)
        self.assertIn(" ERROR ", heads[0])
        self.assertIn("RuntimeError: secret state broke", heads[0])
        self.assertTrue(any("boom" in x for x in text[1:]))       # the stack follows

    def test_a_tick_that_keeps_failing_is_reported_with_the_state_and_a_count(self):
        w, _ = logged_world()
        for _ in range(4):
            try:
                {}["missing"]
            except KeyError as exc:
                w.exc_every(("tick", S.diag.where(exc)), 10.0, "tick failed (2 peer(s), event none)", exc)
        heads = [x for x in lines(w) if "tick failed" in x]
        self.assertEqual(len(heads), 1)

    def test_a_road_trip_database_that_cannot_be_read_is_a_warning_not_silence(self):
        w, _ = logged_world()
        w.rt_gamedb = "/nonexistent/db.sqlite"
        self.assertEqual(w.rt_numlaps(5), 1)
        self.assertIsNone(w.rt_race_mode(5))
        got = [x for x in lines(w) if "unreadable" in x]
        self.assertEqual(len(got), 2)
        self.assertIn(" WARN ", got[0])
        self.assertIn("/nonexistent/db.sqlite", got[0])


class Events(Fresh):
    def test_an_event_that_ends_while_loading_is_a_warning_with_who_was_in_it(self):
        w, (a, b) = logged_world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        w.end_event("nobody loaded the event in time")
        got = [x for x in lines(w) if "event end:" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])
        self.assertIn("loading", got[0])
        self.assertIn("[1, 2]", got[0])

    def test_the_start_names_who_was_excluded_and_why(self):
        w, (a, b) = logged_world()
        b.console_vars.pop(4)                                     # b never answered the invite
        w.game_options = options("FR_KING")
        w.start_event(a)
        text = "\n".join(lines(w))
        self.assertIn("event start: 1 participant(s): [1]", text)
        self.assertIn("1 of 1 other client(s) excluded", text)
        self.assertIn("other client owner 2", text)
        self.assertIn("excluded: never answered the invite with Ready", text)

    def test_a_load_timeout_gives_the_limit_and_what_each_dropped_client_was_doing(self):
        w, (a, b) = logged_world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        a.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.note_loaded(a)
        b.last_rx = time.monotonic() - 7.0
        w.drop_unloaded(time.monotonic())
        got = [x for x in lines(w) if "event load timeout" in x]
        self.assertEqual(len(got), 1)
        self.assertIn(" WARN ", got[0])
        self.assertIn(f"limit {S.EVENT_LOAD_TIMEOUT_SECONDS:.0f} s", got[0])
        self.assertIn("owner1+", got[0])                          # who did load, and how long it took
        self.assertIn("owner2 xuid 0x0 last packet 7.0 s ago, HasLoadedCars=None", got[0])

    def test_the_grid_line_says_how_long_each_client_took_to_load(self):
        w, (a, b) = logged_world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        for q in (a, b):
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
            w.note_loaded(q)
        w.maybe_begin_race()
        got = [x for x in lines(w) if "loaded cars -> starting grid" in x]
        self.assertEqual(len(got), 1)
        self.assertRegex(got[0], r"loaded after owner1\+\d+\.\ds owner2\+\d+\.\ds")


class Control(Fresh):
    def test_a_broken_control_message_names_the_command_and_never_quotes_a_key(self):
        hub = hub_with_log()
        hub.ctl_text("auth 1 10.0.0.1 SECRETKEYSECRETKEY")
        hub.ctl_text("frobnicate now please")
        got = "\n".join(hub_lines(hub))
        self.assertIn("control: auth: auth message unreadable", got)
        self.assertIn("control: frobnicate: unknown or malformed control message (3 field(s))", got)
        self.assertNotIn("SECRETKEY", got)

    def test_an_auth_for_a_revoked_account_is_said_to_be_ignored(self):
        hub = hub_with_log()
        hub.revoked[0xAB] = time.monotonic() + 100
        hub.ctl_text("auth ab 10.0.0.1")
        got = [x for x in hub_lines(hub) if "sign-in of 0xab ignored" in x]
        self.assertEqual(len(got), 1)
        self.assertNotIn(0xAB, hub.authed)

    def test_a_control_port_that_cannot_be_bound_is_an_error(self):
        taken = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        taken.bind(("127.0.0.1", 0))
        self.addCleanup(taken.close)
        out = io.StringIO()
        with unittest.mock.patch.dict(os.environ, {"FH2MP_CONTROL_PORT": str(taken.getsockname()[1]), "FH2MP_UDP_AUTH": "off"}):
            with contextlib.redirect_stdout(out):
                hub = S.Hub(0, None)
        hub.sock.close()
        self.assertIsNone(hub.ctl)
        self.assertRegex(out.getvalue(), r"ERROR .*control port udp/\d+ on 127\.0\.0\.1 not bound \(OSError")

    def test_an_unreadable_cheater_pool_file_is_reported_once_the_log_exists(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "cheaters.json"), "w") as f:
                f.write("{not json")
            out = io.StringIO()
            with unittest.mock.patch.dict(os.environ, {"FH2MP_CONTROL_PORT": "0", "FH2MP_UDP_AUTH": "off"}):
                with contextlib.redirect_stdout(out):
                    hub = S.Hub(0, os.path.join(d, "server.log"))
            hub.sock.close()
            if hub.ctl:
                hub.ctl.close()
            hub.log_file.flush()
            with open(os.path.join(d, "server.log")) as f:
                text = f.read()
        self.assertRegex(text, r"WARN .*cheater pool file .*cheaters\.json not read \(JSONDecodeError")


import unittest.mock  # noqa: E402

class Correlation(Fresh):
    """The run id reaches the game server over the control port and is on every line that ends or starts a player's connection."""

    def test_a_run_announced_by_the_lsp_is_on_the_join_the_removal_and_the_event(self):
        hub = hub_with_log()
        got = []
        hub.events = type("E", (), {"emit": lambda _s, kind, **f: got.append((kind, f))})()
        hub.idle.events = hub.events
        xuid, addr = 0x9000000000011, ("10.5.0.11", 4000)
        hub.ctl_text("run %x ab12cd34" % xuid)
        hub.handle(T.open_packet(xuid), addr)
        w = hub.route[addr]
        w.events = hub.events
        peer = w.peers[addr]
        w.handle(S.build_packet([lambda m: (m.bits(S.MSG_OPEN_CONNECTION_RESPONSE, 7), m.bits(0, 2), m.bits(S.RESPONSE_ACK, 3), m.bits(w.server_counter, 32), m.bits(0, 32))]), addr)
        self.assertTrue(peer.connected)
        w.drop_peer(peer, "test")
        text = "\n".join(hub_lines(hub))
        self.assertIn(f"xuid {xuid:#x} run=ab12cd34, lobby type", text)
        self.assertRegex(text, r"removed \(test\).*run=ab12cd34")
        ends = [f for kind, f in got if kind == "session_end"]
        self.assertEqual(ends[0]["run"], "ab12cd34")

    def test_a_run_id_that_is_not_one_stores_nothing_usable(self):
        hub = hub_with_log()
        hub.ctl_text("run 1 not-a-run")
        hub.ctl_text("run zz ab12cd34")
        self.assertEqual(hub.runs.get(1), "")
        self.assertEqual(len(hub.runs), 1)
        self.assertTrue(any("run message unreadable" in x for x in hub_lines(hub)))


class FailuresThatLookedLikeNothing(Fresh):
    def test_a_full_disk_does_not_stop_a_world(self):
        w, _peers = logged_world()

        class Full:
            def write(self, _text):
                raise OSError(28, "No space left on device")
        w.log_file = Full()
        with contextlib.redirect_stderr(io.StringIO()):
            w.log("anything")           # must not raise: this runs inside the receive loop's exception handler too

    def test_a_check_that_came_up_without_its_data_is_an_error_at_start(self):
        S.STARTUP_PROBLEMS.append("parts (test): lobby cars get no part check")
        self.addCleanup(S.STARTUP_PROBLEMS.pop)
        with contextlib.redirect_stdout(io.StringIO()) as out:
            T.make_hub()
        self.assertRegex(out.getvalue(), r"ERROR .*startup.data_missing parts \(test\)")

    def test_trace_of_received_datagrams_is_off_unless_asked_for(self):
        self.assertFalse(S.TRACE_PACKETS)


if __name__ == "__main__":
    unittest.main()
