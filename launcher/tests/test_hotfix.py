"""The hotfix document (fh2mp_launcher/hotfix.py): signature and serial rules, which rules apply to a setup, the mods it turns off and on
again, and what Play does with it (core.Launcher.apply_hotfix, shim.write_session's switches line)."""
import base64
import hashlib
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402
from fh2mp_launcher import core, hotfix, shim  # noqa: E402

KEY = Ed25519PrivateKey.generate()
PUB = base64.b64encode(KEY.public_key().public_bytes_raw()).decode()
HONK = b"the HonkHonk dll"
HONK_SHA = hashlib.sha256(HONK).hexdigest()
OTHER_SHA = "0" * 64


def signed(doc, key=KEY):
    raw = json.dumps(doc).encode()
    return {"doc": base64.b64encode(raw).decode(), "sig": base64.b64encode(key.sign(raw)).decode()}


def doc(serial=1, rules=None, **kw):
    d = {"schema": 1, "serial": serial, "rules": rules if rules is not None else [
        {"id": "honk-speedometer", "why": "crash at 0xdffe", "match": {"shim": "<=1.1.5", "mods": [HONK_SHA]},
         "switches": ["NO_HONK"], "disableMods": [{"sha256": HONK_SHA, "reason": "crashes on Windows"}]}]}
    d.update(kw)
    return d


class Base(unittest.TestCase):
    def setUp(self):
        p = mock.patch.object(hotfix, "HOTFIX_PUBKEY_B64", PUB)
        p.start()
        self.addCleanup(p.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.cache = os.path.join(self.tmp.name, "cache")
        os.makedirs(self.cache)
        self.az = os.path.join(self.tmp.name, "azimuth")
        os.makedirs(os.path.join(self.az, "fh2-mods"))
        with open(os.path.join(self.az, "fh2-mods", "HonkHonk.dll"), "wb") as f:
            f.write(HONK)


class Verify(Base):
    def test_a_signed_document_is_taken(self):
        self.assertEqual(hotfix.verify(signed(doc()))["serial"], 1)

    def test_another_key_or_a_changed_document_is_refused(self):
        with self.assertRaises(hotfix.HotfixError):
            hotfix.verify(signed(doc(), Ed25519PrivateKey.generate()))
        s = signed(doc())
        s["doc"] = base64.b64encode(json.dumps(doc(serial=9)).encode()).decode()
        with self.assertRaises(hotfix.HotfixError):
            hotfix.verify(s)

    def test_rules_of_the_wrong_shape_are_refused(self):
        bad = [{"id": "x", "switches": ["TOKEN"]}, {"id": "x", "switches": ["NO_X=1"]}, {"id": "x", "match": {"gpu": "amd"}},
               {"id": "x", "match": {"shim": "latest"}}, {"id": "x", "disableMods": [{"name": "HonkHonk.dll"}]}, {"switches": []},
               {"id": "x", "match": {"os": ["mac"]}}, {"id": "x", "refusePlay": ""}]
        for r in bad:
            with self.assertRaises(hotfix.HotfixError, msg=r):
                hotfix.verify(signed(doc(rules=[r])))
        for d in (doc(schema=2), doc(serial=0), doc(expires="next week")):
            with self.assertRaises(hotfix.HotfixError):
                hotfix.verify(signed(d))


class Evaluate(Base):
    def test_a_rule_applies_when_every_part_of_its_match_holds(self):
        mods = {HONK_SHA: "fh2-mods/HonkHonk.dll"}
        r = hotfix.evaluate(doc(), "1.1.5", "1.1.5", mods, "windows")
        self.assertEqual((r.rules, r.switches, list(r.disable)), (["honk-speedometer"], ["NO_HONK"], [HONK_SHA]))
        self.assertEqual(hotfix.evaluate(doc(), "1.1.5", "1.1.6", mods).rules, [])          # newer shim: fixed there
        self.assertEqual(hotfix.evaluate(doc(), "1.1.5", "1.1.5", {}).rules, [])            # no such mod installed
        self.assertEqual(hotfix.evaluate(doc(), "1.1.5", "", mods).rules, [])               # no shim: in no range

    def test_os_launcher_and_ranges(self):
        d = doc(rules=[{"id": "w", "match": {"os": ["windows"], "launcher": ">=1.1.3,<1.1.6"}, "switches": ["NO_A"]}])
        self.assertEqual(hotfix.evaluate(d, "1.1.5", "1.1.5", {}, "windows").switches, ["NO_A"])
        self.assertEqual(hotfix.evaluate(d, "1.1.5", "1.1.5", {}, "linux").switches, [])
        self.assertEqual(hotfix.evaluate(d, "1.1.6", "1.1.5", {}, "windows").switches, [])
        self.assertEqual(hotfix.evaluate(d, "1.1.2", "1.1.5", {}, "windows").switches, [])
        self.assertTrue(hotfix.version_in("1.1.5-debug", "==1.1.5"))

    def test_switches_are_listed_once_and_refusal_is_the_first(self):
        d = doc(rules=[{"id": "a", "switches": ["NO_A", "NO_B"], "refusePlay": "first"}, {"id": "b", "switches": ["NO_B"], "refusePlay": "second"}])
        r = hotfix.evaluate(d, "1.1.5", "1.1.5", {})
        self.assertEqual((r.switches, r.refuse), (["NO_A", "NO_B"], "first"))

    def test_a_document_for_another_environment_asks_for_nothing(self):
        mods = {HONK_SHA: "x"}
        self.assertEqual(hotfix.evaluate(doc(env="dev"), "1.1.5", "1.1.5", mods, env="prod").rules, [])
        self.assertEqual(hotfix.evaluate(doc(env="prod"), "1.1.5", "1.1.5", mods, env="prod").rules, ["honk-speedometer"])
        self.assertEqual(hotfix.evaluate(doc(), "1.1.5", "1.1.5", mods, env="prod").rules, ["honk-speedometer"])

    def test_an_expired_document_asks_for_nothing(self):
        d = doc(expires="2026-11-01T00:00:00Z")
        self.assertEqual(hotfix.evaluate(d, "1.1.5", "1.1.5", {HONK_SHA: "x"}, now=hotfix.expires_at(d) + 1).rules, [])
        self.assertEqual(hotfix.evaluate(d, "1.1.5", "1.1.5", {HONK_SHA: "x"}, now=hotfix.expires_at(d) - 1).rules, ["honk-speedometer"])


class StoreRules(Base):
    def test_newest_serial_wins_and_an_older_one_is_refused(self):
        st = hotfix.Store(self.cache)
        self.assertEqual(st.update(signed(doc(serial=3)))["serial"], 3)
        self.assertEqual(st.update(signed(doc(serial=2)))["serial"], 3)          # a replay of an older one
        self.assertEqual(st.update(signed(doc(serial=4)))["serial"], 4)

    def test_no_answer_keeps_the_last_good_copy(self):
        st = hotfix.Store(self.cache)
        st.update(signed(doc(serial=5)))
        self.assertEqual(hotfix.Store(self.cache).update(None)["serial"], 5)

    def test_a_broken_document_keeps_the_last_good_copy(self):
        st = hotfix.Store(self.cache)
        st.update(signed(doc(serial=5)))
        self.assertEqual(st.update(signed(doc(serial=6), Ed25519PrivateKey.generate()))["serial"], 5)

    def test_withdrawn_means_none_and_it_is_not_taken_again(self):
        st = hotfix.Store(self.cache)
        st.update(signed(doc(serial=5)))
        self.assertIsNone(st.update({}))
        self.assertIsNone(st.update(None))                                       # offline after the withdrawal: still none
        self.assertIsNone(st.update(signed(doc(serial=5))))                      # the withdrawn one replayed
        self.assertEqual(st.update(signed(doc(serial=6)))["serial"], 6)


class Mods(Base):
    def list_(self):
        try:
            with open(os.path.join(self.az, "fh2-mods", "platform-disabled.txt")) as f:
                return f.read().split()
        except FileNotFoundError:
            return []

    def test_a_named_mod_goes_off_and_comes_back_when_no_rule_names_it(self):
        mods = hotfix.mod_hashes(self.az)
        self.assertEqual(mods, {HONK_SHA: "fh2-mods/HonkHonk.dll"})
        r = hotfix.evaluate(doc(), "1.1.5", "1.1.5", mods)
        self.assertEqual(hotfix.apply_mods(self.az, r, mods), ["HonkHonk.dll"])
        self.assertEqual(self.list_(), ["HonkHonk.dll"])
        self.assertEqual(hotfix.apply_mods(self.az, r, mods), [])                 # already off: nothing new
        hotfix.apply_mods(self.az, hotfix.Result(), mods)                          # the rule is gone
        self.assertEqual(self.list_(), [])

    def test_a_loader_next_to_azimuth_is_not_put_on_the_platform_list(self):
        with open(os.path.join(self.az, "winmm.dll"), "wb") as f:
            f.write(b"a loader")
        loader = hashlib.sha256(b"a loader").hexdigest()
        mods = hotfix.mod_hashes(self.az)
        self.assertEqual(mods[loader], "winmm.dll")
        r = hotfix.evaluate(doc(rules=[{"id": "l", "disableMods": [{"sha256": loader}]}]), "1.1.5", "1.1.5", mods)
        self.assertEqual(hotfix.apply_mods(self.az, r, mods), [])
        self.assertEqual(self.list_(), [])

    def test_a_mod_the_player_turned_off_stays_off(self):
        with open(os.path.join(self.az, "fh2-mods", "platform-disabled.txt"), "w") as f:
            f.write("SellCar.dll\nHonkHonk.dll\n")
        mods = hotfix.mod_hashes(self.az)
        hotfix.apply_mods(self.az, hotfix.evaluate(doc(), "1.1.5", "1.1.5", mods), mods)
        hotfix.apply_mods(self.az, hotfix.Result(), mods)
        self.assertEqual(self.list_(), ["SellCar.dll", "HonkHonk.dll"])          # neither was ours to turn back on


class Play(Base):
    def launcher(self, served):
        lc = core.Launcher.__new__(core.Launcher)
        lc.settings = {"azimuth_dir": self.az}
        lc.server = {"env": "prod"}
        api = mock.Mock()
        if isinstance(served, Exception):
            api.hotfix.side_effect = served
        else:
            api.hotfix.return_value = served
        lc.api = lambda: api
        return lc

    def test_play_takes_the_switches_and_turns_the_mod_off(self):
        with mock.patch.object(hotfix.paths, "cache_dir", return_value=self.cache), mock.patch.object(core, "__version__", "1.1.5"):
            lc = self.launcher(signed(doc()))
            res = lc.apply_hotfix("1.1.5")
        self.assertEqual(res.switches, ["NO_HONK"])
        self.assertEqual(lc.hotfix_disabled, ["HonkHonk.dll"])

    def test_refuse_play(self):
        d = doc(rules=[{"id": "stop", "refusePlay": "Update the launcher: this version crashes on start."}])
        with mock.patch.object(hotfix.paths, "cache_dir", return_value=self.cache):
            lc = self.launcher(signed(d))
            with self.assertRaises(core.PlayBlocked) as e:
                lc.apply_hotfix("1.1.5")
        self.assertEqual(e.exception.code, "hotfix")

    def test_a_server_without_the_route_changes_nothing(self):
        with mock.patch.object(hotfix.paths, "cache_dir", return_value=self.cache):
            res = self.launcher(core.ApiError(404, "not_found", "no")).apply_hotfix("1.1.5")
        self.assertEqual((res.rules, res.switches), ([], []))

    def test_the_session_file_carries_the_switches(self):
        os.makedirs(os.path.join(self.az, "data", "bin"))
        shim.write_session(self.az, 0x9000011111111, "fh2.tok", "https://lsp", switches=["NO_HONK", "NO_LOGGUARD"])
        with open(os.path.join(self.az, "data", "bin", shim.SESSION_FILE)) as f:
            self.assertIn("switches=NO_HONK,NO_LOGGUARD\n", f.read())
        shim.write_session(self.az, 0x9000011111111, "fh2.tok", "https://lsp")
        with open(os.path.join(self.az, "data", "bin", shim.SESSION_FILE)) as f:
            self.assertNotIn("switches", f.read())


if __name__ == "__main__":
    unittest.main()
