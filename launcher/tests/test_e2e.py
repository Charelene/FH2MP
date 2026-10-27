"""End to end against a running test server: FH2MP_TEST_SERVER=http://127.0.0.1:5190 (FH2MP_ENV=test, FH2MP_AUTH=required)."""
import os
import random
import string
import sys
import tempfile
import unittest
from unittest import mock
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SERVER = os.environ.get("FH2MP_TEST_SERVER", "")


@unittest.skipUnless(SERVER, "set FH2MP_TEST_SERVER")
class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory()
        self.addCleanup(self.home.cleanup)
        patch = mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=self.home.name, FH2MP_NO_KEYRING="1")
        patch.start()
        self.addCleanup(patch.stop)
        from fh2mp_launcher import api, core, identity
        from fh2mp_launcher.settings import Settings
        self.api, self.core, self.identity = api, core, identity
        s = Settings()
        s["api_base"] = SERVER
        s["allow_insecure"] = True
        self.lc = core.Launcher(s)
        # Random letters could accidentally contain a reserved word such as
        # "staff". This namespace remains valid while allowing repeated runs.
        self.tag = "E2E " + "".join(random.choices(string.digits, k=10))

    def test_full_flow(self):
        lc = self.lc
        cfg = lc.refresh_server()
        self.assertEqual(cfg["env"], "test")
        self.assertIn("website", lc.links)
        lc.identity = self.identity.Identity.generate()
        with self.assertRaises(self.api.ApiError) as cm:
            lc.sign_in()
        self.assertEqual(cm.exception.code, "unknown_account")
        self.assertTrue(lc.api().check_name(self.tag)["available"])
        acct = lc.sign_up(self.tag)
        self.assertEqual(acct["gamertag"], self.tag)
        self.assertFalse(lc.api().check_name(self.tag)["available"])
        a2 = lc.sign_in()
        self.assertEqual(a2["xuid"], acct["xuid"])
        me = lc.api().me(lc.token)
        self.assertEqual(me["gamertag"], self.tag)
        # a signed-in game request works, an anonymous one does not
        def game(token):
            r = urllib.request.Request(SERVER + "/titles/1/achievements", headers={"X-FH2MP-Host": "achievements.xboxlive.com",
                                                                                 **({"X-FH2MP-Token": token} if token else {})})
            try:
                return urllib.request.urlopen(r, timeout=10).status
            except urllib.error.HTTPError as e:
                return e.code
        self.assertEqual(game(""), 401)
        self.assertEqual(game(lc.token), 404)
        # the restored key is the same account
        code = lc.identity.recovery_code()
        lc2 = self.core.Launcher(lc.settings)
        lc2.restore_identity(code)
        self.assertEqual(lc2.sign_in()["xuid"], acct["xuid"])
        self.assertEqual(game(lc.token), 401)                       # ...and signing in elsewhere ended the first session
        second_token = lc2.token
        lc2.sign_out()
        self.assertEqual(game(second_token), 401)                  # revoke the actual token, not merely clear the local variable


if __name__ == "__main__":
    unittest.main()
