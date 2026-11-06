"""Shim hotfixes: within an X.Y line the launcher installs the shim the newest signed manifest offers it, not only its own release's."""
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import core, updater  # noqa: E402
from fh2mp_launcher.settings import Settings  # noqa: E402
from fh2mp_launcher.version import __version__  # noqa: E402

BASE = __version__.split("-")[0]
MAJOR, MINOR, PATCH = (int(x) for x in BASE.split("."))


def v(patch_delta=0, minor_delta=0):
    return f"{MAJOR}.{MINOR + minor_delta}.{PATCH + patch_delta if not minor_delta else 0}"


def manifest(release, shim_version, kind="hotfix", **shim_extra):
    shim = {"version": shim_version, "name": f"fh2mp-shim-{shim_version}.zip", "sha256": "a" * 64, "size": 10, "dllSha256": "b" * 64,
            **shim_extra}
    return {"schema": 1, "tag": "v" + release, "version": release, "kind": kind, "line": updater.line(release), "minLauncher": BASE,
            "shim": shim, "launcher": {"version": BASE, "tag": "v" + BASE, "assets": {}}}


class ShimEntryTests(unittest.TestCase):
    def test_line(self):
        self.assertEqual(updater.line("1.1.5"), "1.1")
        self.assertEqual(updater.line("v1.12.0-rc3"), "1.12")
        self.assertEqual(updater.line("nonsense"), "")

    def test_a_newer_shim_of_the_line_is_offered(self):
        e = updater.shim_entry(manifest(v(1), v(1)), BASE)
        self.assertEqual((e["version"], e["tag"]), (v(1), "v" + v(1)))         # no shim.tag: the manifest's own release holds the zip

    def test_a_carried_shim_is_fetched_from_the_release_that_carries_it(self):
        e = updater.shim_entry(manifest(v(2), BASE), BASE)                       # a withdrawn hotfix: the release's shim again
        self.assertEqual((e["version"], e["tag"]), (BASE, "v" + v(2)))

    def test_another_line_or_an_older_release_offers_nothing(self):
        self.assertIsNone(updater.shim_entry(manifest(v(minor_delta=1), v(minor_delta=1)), BASE))
        if PATCH:
            older = f"{MAJOR}.{MINOR}.{PATCH - 1}"
            self.assertIsNone(updater.shim_entry(manifest(older, older), BASE))  # GitHub's latest behind a launcher just published
        self.assertIsNone(updater.shim_entry(None, BASE))
        self.assertIsNone(updater.shim_entry({"tag": "v" + BASE}, BASE))

    def test_min_launcher_of_the_shim_holds(self):
        self.assertIsNone(updater.shim_entry(manifest(v(2), v(2), minLauncher=v(1)), BASE))
        self.assertIsNotNone(updater.shim_entry(manifest(v(2), v(2), minLauncher=v(1)), v(1)))

    def test_a_suspicious_tag_is_refused(self):
        m = manifest(v(1), v(1))
        m["tag"] = "../../evil"
        self.assertIsNone(updater.shim_entry(m, BASE))

    def test_shim_release_falls_back_to_the_launchers_own_release(self):
        up = mock.Mock()
        up.fetch_manifest.return_value = manifest(BASE, BASE, kind="release")
        m, e = updater.shim_release(up, manifest(v(minor_delta=1), v(minor_delta=1)), BASE)
        up.fetch_manifest.assert_called_once_with(tag="v" + BASE)
        self.assertEqual(e["version"], BASE)
        up.fetch_manifest.reset_mock()
        m, e = updater.shim_release(up, manifest(v(1), v(1)), BASE)
        up.fetch_manifest.assert_not_called()
        self.assertEqual(e["version"], v(1))
        up.fetch_manifest.return_value = {"tag": "v" + BASE, "shim": {"version": "9.9.9"}}
        with self.assertRaises(updater.UpdateError):
            updater.shim_release(up, None, BASE)


class DevChannelTagTests(unittest.TestCase):
    def test_the_highest_version_wins_not_the_newest_published(self):
        rels = [{"tag_name": "v" + v(1), "assets": [{"name": "manifest.json"}]},                 # a stable hotfix, published last
                {"tag_name": f"v{v(minor_delta=1)}-rc1", "assets": [{"name": "manifest.json"}]},
                {"tag_name": "v9.0.0", "draft": True, "assets": [{"name": "manifest.json"}]},
                {"tag_name": "v8.0.0", "assets": []}]
        up = updater.Updater(channel="dev")
        with mock.patch.object(up, "_get", return_value=json.dumps(rels).encode()):
            self.assertEqual(up._release_tag(), f"v{v(minor_delta=1)}-rc1")


class LauncherShimTargetTests(unittest.TestCase):
    def setUp(self):
        os.environ.pop("FH2MP_DEV", None)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lc = core.Launcher(Settings(os.path.join(self.tmp.name, "settings.json")), load_identity=False)
        self.lc.settings["azimuth_dir"] = self.tmp.name
        self.lc.game_running = lambda: False

    def state(self, ver):
        self.lc.shim_state = lambda: ("ours", ver)

    def test_a_shim_hotfix_makes_the_release_shim_outdated(self):
        self.lc.manifest = manifest(v(1), v(1))
        self.state(BASE)
        self.assertEqual(self.lc.wanted_shim(), v(1))
        self.assertTrue(self.lc.shim_outdated())
        self.assertEqual(self.lc.update_status()["shim"], f"Install FH2MP {v(1)} for this launcher.")
        self.state(v(1))
        self.assertFalse(self.lc.shim_outdated())
        self.assertIsNone(self.lc.update_status()["shim"])

    def test_a_launcher_hotfix_keeps_the_release_shim(self):
        with mock.patch.object(core, "__version__", v(1)):                      # launcher v(1), its release carries the shim of BASE
            self.lc.manifest = manifest(v(1), BASE)
            self.state(BASE)
            self.assertEqual(self.lc.wanted_shim(), BASE)
            self.assertFalse(self.lc.shim_outdated())

    def test_without_a_manifest_the_last_signed_install_counts(self):
        self.state(v(1))
        self.assertTrue(self.lc.shim_outdated())                                # nothing known: the launcher's own version
        self.lc.settings["shim_signed"] = {"launcher": __version__, "version": v(1), "tag": "v" + v(1)}
        self.assertFalse(self.lc.shim_outdated())                               # offline after a hotfix install: still fine
        self.lc.settings["shim_signed"] = {"launcher": "0.0.1", "version": v(1), "tag": "v" + v(1)}
        self.assertTrue(self.lc.shim_outdated())                                # recorded by another launcher version

    def test_the_server_minimum_still_applies(self):
        self.lc.manifest = manifest(v(1), v(1))
        self.lc.server = {"minShim": v(2)}
        self.state(v(1))
        self.assertTrue(self.lc.shim_outdated())

    def test_auto_update_installs_a_hotfix_and_rolls_one_back(self):
        installed = []
        self.lc.install_shim = lambda *a, **k: installed.append(self.lc.wanted_shim())
        self.lc.manifest = manifest(v(1), v(1))
        self.state(BASE)
        self.assertTrue(self.lc.auto_update_shim())
        self.state(v(1))
        self.assertFalse(self.lc.auto_update_shim())
        self.lc.manifest = manifest(v(2), BASE)                                 # v(1) withdrawn: back to the release's shim
        self.assertTrue(self.lc.auto_update_shim())
        self.assertEqual(installed, [v(1), BASE])

    def test_install_takes_the_carried_shim_of_the_newest_release(self):
        dll = f"MZ FH2MP-SHIM-VERSION:{BASE}:END".encode()
        self.lc.manifest = manifest(v(2), BASE)
        self.state(v(1))
        with mock.patch.object(updater, "Updater") as factory, mock.patch.object(core, "read_shim_zip", return_value=dll), \
                mock.patch.object(core.shim, "install", return_value=[]) as install, \
                mock.patch.object(self.lc, "platform_files", return_value=None), mock.patch.object(self.lc, "repair_tls_helper", return_value=[]), \
                mock.patch.object(self.lc, "ensure_extra_options", return_value=[]), mock.patch.object(self.lc, "ensure_honk", return_value=[]), \
                mock.patch.object(self.lc, "ensure_controls", return_value=[]):
            self.lc.install_shim()
            up = factory.return_value
            self.assertEqual(up.download.call_args[0][0], "v" + v(2))
            up.fetch_manifest.assert_not_called()
            install.assert_called_once()
        self.assertEqual(self.lc.settings["shim_signed"], {"launcher": __version__, "version": BASE, "tag": "v" + v(2)})

    def test_install_refuses_a_dll_of_another_version(self):
        self.lc.manifest = manifest(v(1), v(1))
        with mock.patch.object(updater, "Updater"), mock.patch.object(core, "read_shim_zip", return_value=f"MZ FH2MP-SHIM-VERSION:{BASE}:END".encode()), \
                mock.patch.object(core.shim, "install") as install:
            with self.assertRaises(updater.UpdateError):
                self.lc.install_shim()
            install.assert_not_called()


class ReplayTests(unittest.TestCase):
    def test_an_older_manifest_than_one_seen_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(os.path.join(d, "settings.json")), load_identity=False)
            with mock.patch.object(updater.Updater, "fetch_manifest", return_value=manifest(v(2), v(2))):
                lc.check_updates()
            self.assertEqual(lc.settings["seen_release"], {"stable": "v" + v(2)})
            with mock.patch.object(updater.Updater, "fetch_manifest", return_value=manifest(v(1), v(1))):
                with self.assertRaises(updater.UpdateError):
                    lc.check_updates()
            self.assertEqual(lc.manifest["tag"], "v" + v(2))                    # the refused one replaced nothing
            with mock.patch.object(updater.Updater, "fetch_manifest", return_value=manifest(v(3), v(3))):
                lc.check_updates()
            self.assertEqual(lc.settings["seen_release"]["stable"], "v" + v(3))


class HotfixNoticeTests(unittest.TestCase):
    def test_the_notice_shows_only_when_a_rule_applies(self):
        from fh2mp_launcher import hotfix
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(os.path.join(d, "settings.json")), load_identity=False)
            self.assertEqual(lc.hotfix_notice, "")
            r = hotfix.Result({"serial": 1, "notice": "Honks are off for a crash fix."})
            lc.hotfix = r
            self.assertEqual(lc.hotfix_notice, "")
            r.add({"id": "x", "switches": ["NO_HONK"]})
            self.assertEqual(lc.hotfix_notice, "Honks are off for a crash fix.")


if __name__ == "__main__":
    unittest.main()
