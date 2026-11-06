"""release/versions.py and release/make_manifest.py: full releases, hotfixes that carry a component, and what the launcher reads from them."""
import base64
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402
from fh2mp_launcher import updater  # noqa: E402

RELEASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "release")


def load_versions():
    spec = importlib.util.spec_from_file_location("release_versions", os.path.join(RELEASE, "versions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class VersionRules(unittest.TestCase):
    def setUp(self):
        self.v = load_versions()

    def plan(self, *a, **k):
        return self.v.plan(*a, **k)

    def test_a_full_release_builds_both(self):
        p = self.plan("v1.2.0", "1.2.0", "1.2.0", full=True)
        self.assertEqual((p["build_shim"], p["build_launcher"], p["kind"]), ("true", "true", "release"))
        self.assertEqual(self.plan("v1.2.0-rc1", "1.2.0-rc1", "1.2.0-rc1")["kind"], "release")

    def test_a_hotfix_builds_only_the_component_set_to_the_tag(self):
        p = self.plan("v1.1.6", "1.1.6", "1.1.5")
        self.assertEqual((p["build_shim"], p["build_launcher"], p["kind"]), ("true", "false", "hotfix"))
        p = self.plan("v1.1.7", "1.1.6", "1.1.7")
        self.assertEqual((p["build_shim"], p["build_launcher"], p["kind"]), ("false", "true", "hotfix"))
        self.assertEqual(self.plan("v1.1.7", "1.1.5", "1.1.5")["kind"], "rollback")       # shim/VERSION set back: the release's shim again

    def test_what_is_refused(self):
        for args in [("v1.1.6", "1.1.6", "1.1.5", True),         # --full needs both
                     ("v1.2.1", "1.2.1", "1.1.5", False),        # another line: a full release
                     ("v1.1.6", "1.1.7", "1.1.6", False),        # above the tag
                     ("v1.1.6-rc1", "1.1.6-rc1", "1.1.5", False),  # a candidate carries nothing
                     ("v1.1.6", "1.1.6", "1.1.5-rc2", False),
                     ("1.1.6", "1.1.6", "1.1.6", False),
                     ("v1.1.6", "1.1.6", "nope", False)]:
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.plan(*args[:3], full=args[3])

    def test_the_repository_versions_are_readable(self):
        shim, launcher = self.v.read()
        self.assertRegex(shim, self.v.VER_RE)
        self.assertRegex(launcher, self.v.VER_RE)


class ManifestCarry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        sk = Ed25519PrivateKey.generate()
        self.key = os.path.join(self.tmp.name, "signing.key")
        with open(self.key, "w") as f:
            f.write(base64.b64encode(sk.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                                      serialization.NoEncryption())).decode())
        self.pub = base64.b64encode(sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)).decode()

    def dist(self, tag):
        d = os.path.join(self.tmp.name, tag)
        os.makedirs(d, exist_ok=True)
        return d

    def shim_zip(self, d, ver):
        with zipfile.ZipFile(os.path.join(d, f"fh2mp-shim-{ver}.zip"), "w") as z:
            z.writestr("fh2-mods/FH2MP.dll", f"MZ FH2MP-SHIM-VERSION:{ver}:END".encode())

    def launcher_files(self, d, ver):
        for name in (f"FH2MP-Launcher-{ver}-windows-x64-setup.exe", f"fh2mp-launcher_{ver}_amd64.deb"):
            with open(os.path.join(d, name), "wb") as f:
                f.write(f"launcher {ver} {name}".encode())

    def make(self, tag, *extra, ok=True):
        r = subprocess.run([sys.executable, "-I", os.path.join(RELEASE, "make_manifest.py"), "--tag", tag, "--dist", self.dist(tag),
                            "--key", self.key, *extra], capture_output=True, text=True)
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(os.path.join(self.dist(tag), "manifest.json"), "rb") as f:
                raw = f.read()
            with open(os.path.join(self.dist(tag), "manifest.json.sig")) as f:
                return updater.verify_manifest(raw, f.read(), self.pub)
        self.assertNotEqual(r.returncode, 0)
        return r.stderr

    def carry(self, from_tag, to_tag, names):
        """What the release workflow does: the carried release's manifest into carry/, its files into the new release's dist."""
        src, dst = self.dist(from_tag), self.dist(to_tag)
        cd = os.path.join(self.tmp.name, "carry", from_tag)
        os.makedirs(cd, exist_ok=True)
        for n in ("manifest.json", "manifest.json.sig"):
            with open(os.path.join(src, n), "rb") as f, open(os.path.join(cd, n), "wb") as g:
                g.write(f.read())
        for n in names:
            with open(os.path.join(src, n), "rb") as f, open(os.path.join(dst, n), "wb") as g:
                g.write(f.read())

    def full(self, ver):
        self.shim_zip(self.dist("v" + ver), ver)
        self.launcher_files(self.dist("v" + ver), ver)
        return self.make("v" + ver)

    def test_full_release(self):
        m = self.full("1.1.5")
        self.assertEqual((m["kind"], m["line"], m["minLauncher"], m["shim"]["version"], m["launcher"]["version"]),
                         ("release", "1.1", "1.1.5", "1.1.5", "1.1.5"))
        self.assertEqual(sorted(m["launcher"]["assets"]), ["linux-deb", "windows-x64-installer"])

    def test_shim_hotfix_carries_the_launcher_and_keeps_min_launcher(self):
        base = self.full("1.1.5")
        self.carry("v1.1.5", "v1.1.6", [e["name"] for e in base["launcher"]["assets"].values()])
        self.shim_zip(self.dist("v1.1.6"), "1.1.6")
        m = self.make("v1.1.6", "--shim-version", "1.1.6", "--launcher-version", "1.1.5", "--carry-dir", os.path.join(self.tmp.name, "carry"))
        self.assertEqual((m["kind"], m["minLauncher"], m["shim"]["version"], m["launcher"]["version"]), ("hotfix", "1.1.5", "1.1.6", "1.1.5"))
        self.assertEqual(m["launcher"]["assets"], base["launcher"]["assets"])
        self.assertEqual(updater.shim_entry(m, "1.1.5")["version"], "1.1.6")          # a 1.1.5 launcher takes the hotfix
        self.assertFalse(updater.newer(m["launcher"]["version"], "1.1.5"))          # and is offered no launcher update

    def test_launcher_hotfix_carries_the_shim(self):
        base = self.full("1.1.5")
        self.carry("v1.1.5", "v1.1.6", [base["shim"]["name"]])
        self.launcher_files(self.dist("v1.1.6"), "1.1.6")
        m = self.make("v1.1.6", "--shim-version", "1.1.5", "--launcher-version", "1.1.6", "--carry-dir", os.path.join(self.tmp.name, "carry"))
        self.assertEqual((m["kind"], m["minLauncher"], m["shim"]), ("hotfix", "1.1.5", base["shim"]))
        self.assertTrue(updater.newer(m["launcher"]["version"], "1.1.5"))
        self.assertEqual(updater.shim_entry(m, "1.1.6")["version"], "1.1.5")

    def test_carried_files_must_be_the_signed_ones(self):
        base = self.full("1.1.5")
        self.carry("v1.1.5", "v1.1.6", [e["name"] for e in base["launcher"]["assets"].values()])
        self.shim_zip(self.dist("v1.1.6"), "1.1.6")
        with open(os.path.join(self.dist("v1.1.6"), "fh2mp-launcher_1.1.5_amd64.deb"), "ab") as f:
            f.write(b"tampered")
        err = self.make("v1.1.6", "--shim-version", "1.1.6", "--launcher-version", "1.1.5", "--carry-dir",
                        os.path.join(self.tmp.name, "carry"), ok=False)
        self.assertIn("not the file the carried manifest names", err)

    def test_a_carried_manifest_signed_by_another_key_is_refused(self):
        self.full("1.1.5")
        self.carry("v1.1.5", "v1.1.6", ["fh2mp-shim-1.1.5.zip"])
        sig = os.path.join(self.tmp.name, "carry", "v1.1.5", "manifest.json.sig")
        with open(os.path.join(self.tmp.name, "carry", "v1.1.5", "manifest.json"), "rb") as f:
            other = Ed25519PrivateKey.generate().sign(f.read())
        with open(sig, "w") as f:
            f.write(base64.b64encode(other).decode())
        self.launcher_files(self.dist("v1.1.6"), "1.1.6")
        err = self.make("v1.1.6", "--shim-version", "1.1.5", "--launcher-version", "1.1.6", "--carry-dir", os.path.join(self.tmp.name, "carry"),
                        ok=False)
        self.assertIn("not signed with this key", err)

    def test_rollback_carries_both(self):
        base = self.full("1.1.5")
        self.carry("v1.1.5", "v1.1.7", [base["shim"]["name"], *(e["name"] for e in base["launcher"]["assets"].values())])
        m = self.make("v1.1.7", "--shim-version", "1.1.5", "--launcher-version", "1.1.5", "--carry-dir", os.path.join(self.tmp.name, "carry"))
        self.assertEqual((m["kind"], m["shim"]["version"], m["minLauncher"]), ("rollback", "1.1.5", "1.1.5"))
        self.assertEqual(updater.shim_entry(m, "1.1.5")["tag"], "v1.1.7")

    def test_a_debug_shim_is_still_refused(self):
        d = self.dist("v1.1.6")
        with zipfile.ZipFile(os.path.join(d, "fh2mp-shim-1.1.6.zip"), "w") as z:
            z.writestr("fh2-mods/FH2MP.dll", b"MZ FH2MP-SHIM-VERSION:1.1.6:END fh2mp-dbg")
        self.assertIn("debug build", self.make("v1.1.6", ok=False))


if __name__ == "__main__":
    unittest.main()
