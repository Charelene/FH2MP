"""/v1/hotfix (launcher_api.HotfixFile): the signed hotfix document is served as it is, re-read when the file changes, the last good copy stays
when the file is broken, and nothing is served without a file. The signature is the launcher's to check (launcher/tests/test_hotfix.py)."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import launcher_api  # noqa: E402


class HotfixRoute(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "prod-hotfix.json")
        self.ctx = launcher_api.Ctx(None, None, launcher_api.LauncherConfig(), lambda: 0)
        self.orig = launcher_api.HOTFIX
        launcher_api.HOTFIX = launcher_api.HotfixFile(self.path)
        self.addCleanup(setattr, launcher_api, "HOTFIX", self.orig)

    def get(self):
        st, obj, ctype = launcher_api.handle(self.ctx, "GET", "/v1/hotfix", {}, b"", "203.0.113.5")
        self.assertEqual((st, ctype), (200, "application/json"))
        return obj

    def write(self, text, mtime):
        with open(self.path, "w") as f:
            f.write(text)
        os.utime(self.path, (mtime, mtime))

    def test_no_file_serves_nothing(self):
        self.assertEqual(self.get(), {})
        launcher_api.HOTFIX = launcher_api.HotfixFile("")
        self.assertEqual(self.get(), {})

    def test_the_file_is_served_and_reread_when_it_changes(self):
        self.write(json.dumps({"doc": "ZG9j", "sig": "c2ln", "extra": 1}), 1000)
        self.assertEqual(self.get(), {"doc": "ZG9j", "sig": "c2ln"})
        self.write(json.dumps({"doc": "bmV3", "sig": "c2ln"}), 2000)
        self.assertEqual(self.get()["doc"], "bmV3")

    def test_a_broken_file_keeps_the_last_good_one(self):
        self.write(json.dumps({"doc": "ZG9j", "sig": "c2ln"}), 1000)
        self.get()
        for bad, t in (("{not json", 2000), (json.dumps({"doc": 5, "sig": "x"}), 3000), (json.dumps(["doc"]), 4000)):
            self.write(bad, t)
            self.assertEqual(self.get(), {"doc": "ZG9j", "sig": "c2ln"}, bad)

    def test_removing_the_file_withdraws_it(self):
        self.write(json.dumps({"doc": "ZG9j", "sig": "c2ln"}), 1000)
        self.get()
        os.remove(self.path)
        self.assertEqual(self.get(), {})


if __name__ == "__main__":
    unittest.main()
