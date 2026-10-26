"""Authenticated tool reports and transport-level pool isolation; no database required."""
import os
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
import fh2mp_lsp as L
import fh2mp_server as S
from test_security import make_hub, open_packet


class ToolReports(unittest.TestCase):
    def setUp(self):
        self.replies = []
        self.request = SimpleNamespace(headers={}, client_ip=lambda: "127.0.0.1",
                                       _gamedb_ok=lambda a: True,
                                       _reply_json=lambda status, obj: self.replies.append((status, obj)))
        self.acct = SimpleNamespace(xuid=123, gamertag="Player")
        L._TOOLS_ANNOUNCED.clear()

    def ask(self, report):
        self.request.headers = {"X-FH2MP-Token": "session", "X-UserId": "999"}
        if report is not None:
            self.request.headers["X-FH2MP-Tools"] = report
        auth = SimpleNamespace(mode="required", authenticate=mock.Mock(return_value=self.acct))
        with mock.patch.dict(os.environ, FH2MP_RESTRICTED_TOOLS="quarantine"), \
                mock.patch.object(L, "AUTH", auth), mock.patch.object(L, "LAUNCHER", None), \
                mock.patch.object(L, "announce_auth"):
            return L.Handler._identify(self.request)

    def test_only_authenticated_sender_is_flagged_and_reports_are_throttled(self):
        with mock.patch.object(L.xbl, "quarantine_tools", return_value="ok") as flag:
            self.assertTrue(self.ask("v1,redline,nitrosystem"))
            self.assertTrue(self.ask("v1,nitrosystem,redline"))
            flag.assert_called_once_with(123, ("nitrosystem", "redline"))

    def test_clean_client_and_unknown_tool_are_not_flagged(self):
        with mock.patch.object(L.xbl, "quarantine_tools") as flag:
            self.assertTrue(self.ask("v1"))
            self.assertFalse(self.ask("v1,carpack"))
            self.assertFalse(self.ask(None))
            flag.assert_not_called()
        self.assertEqual([s for s, _ in self.replies], [400, 426])

    def test_missing_hub_ack_cannot_allow_a_tool_client_through(self):
        with mock.patch.object(L.xbl, "quarantine_tools", return_value=None):
            self.assertFalse(self.ask("v1,redline"))
        self.assertEqual(self.replies[0][0], 503)

    def test_mid_session_detection_requires_rejoin_once(self):
        with mock.patch.object(L.xbl, "quarantine_tools", return_value="rejoin"), \
                mock.patch.object(L.xbl, "drop_player") as drop:
            self.assertFalse(self.ask("v1,nitrosystem"))
            self.assertTrue(self.ask("v1,nitrosystem"))
            drop.assert_called_once_with(123)


class TransportPools(unittest.TestCase):
    def setUp(self):
        self.hub = make_hub()

    def join(self, xuid, port, name=None):
        addr = ("10.9.0.1", port)
        if name:
            self.hub.assign[xuid] = name
        self.hub.handle(open_packet(xuid), addr)
        return self.hub.route[addr]

    def test_tool_report_sets_pool_and_survives_a_fresh_signin(self):
        self.hub.ctl_text("tools 123 redline,nitrosystem")
        self.assertIn(0x123, self.hub.flagged_now())
        self.hub.ctl_text("allow 123")
        self.assertIn(0x123, self.hub.flagged_now())
        self.hub.ctl_text("tools 456 carpack")
        self.assertNotIn(0x456, self.hub.flagged_now())

    def test_direct_join_and_stale_assignment_cannot_enter_clean_world(self):
        clean = self.join(1, 5001, "clean")
        self.hub.flag(2, "restricted tools: redline")
        restricted = self.join(2, 5002, "clean")
        self.assertIsNot(clean, restricted)
        self.hub.flag(3, "restricted tools: nitrosystem")
        self.assertIs(self.join(3, 5003), restricted)
        self.assertIs(self.join(4, 5004), clean)

    def test_mid_session_tool_client_is_removed_from_mixed_world(self):
        self.join(1, 5001, "clean")
        self.join(2, 5002, "clean")
        self.hub.ctl_text("tools 2 redline")
        self.assertIn(2, self.hub.flagged_now())
        self.assertIn(2, self.hub.revoked)
        self.assertNotIn(1, self.hub.revoked)


if __name__ == "__main__":
    unittest.main()
