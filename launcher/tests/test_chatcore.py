"""The launcher's glue to the chat core: off by default, command lines, targets, dev flag."""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import chatcore  # noqa: E402


class ChatCore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.exe = os.path.join(self.tmp.name, chatcore.EXE_NAME)
        with open(self.exe, "wb"):
            pass
        self.addCleanup(self.tmp.cleanup)

    def env(self, **kw):
        base = {"FH2MP_CHAT_EXE": self.exe, "FH2MP_CHAT_CORE": "1"}
        base.update(kw)
        return mock.patch.dict(os.environ, {k: v for k, v in base.items() if v is not None}, clear=False)

    def test_off_by_default(self):
        with mock.patch.dict(os.environ, {"FH2MP_CHAT_EXE": self.exe}), mock.patch.object(chatcore, "_spawn") as spawn:
            os.environ.pop("FH2MP_CHAT_CORE", None)
            self.assertFalse(chatcore.start_background())
            self.assertFalse(chatcore.open_chat("club"))
            self.assertFalse(chatcore.quit_core())
            spawn.assert_not_called()

    def test_command_lines(self):
        with self.env(FH2MP_DEV="0"), mock.patch.object(chatcore, "_spawn", return_value=True) as spawn:
            self.assertTrue(chatcore.start_background())
            self.assertTrue(chatcore.open_chat("club"))
            self.assertTrue(chatcore.open_chat("friend:9000027d5236b"))
            self.assertTrue(chatcore.open_chat("../../etc"))          # an unknown target opens the window without one
            self.assertTrue(chatcore.quit_core())
            got = [c.args[0][1:] for c in spawn.call_args_list]
            self.assertEqual(got, [["--background"], ["--open", "club"], ["--open", "friend:9000027d5236b"], ["--open"], ["--quit"]])

    def test_dev_launcher_passes_dev(self):
        with self.env(FH2MP_DEV="1"), mock.patch.object(chatcore, "_spawn", return_value=True) as spawn:
            chatcore.open_chat()
            self.assertEqual(spawn.call_args.args[0][1:], ["--open", "--dev"])

    def test_missing_core_falls_back(self):
        with self.env(FH2MP_CHAT_EXE=os.path.join(self.tmp.name, "nope")), mock.patch.object(chatcore, "_spawn") as spawn:
            self.assertFalse(chatcore.start_background())
            self.assertFalse(chatcore.open_chat("club"))
            spawn.assert_not_called()

    def test_targets(self):
        for ok in ("club", "requests", "friend:1", "friend:9000027d5236b"):
            self.assertTrue(chatcore.valid_target(ok), ok)
        for bad in ("", "friend:", "friend:XYZ", "friend:" + "f" * 17, "settings", None, 5):
            self.assertFalse(chatcore.valid_target(bad), bad)


if __name__ == "__main__":
    unittest.main()
