"""Tests for the launcher home data (server/home.py) and the launcher assets endpoint: python3 -I server/test_home.py
(needs psycopg 3 importable like test_ops.py, no database server; the stores are test_ops.py's fakes)."""
import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import home  # noqa: E402
import launcher_api  # noqa: E402
from test_ops import make_lsp, submit  # noqa: E402

RSS = b"""<?xml version="1.0" encoding="UTF-8" ?><rss version="2.0"><channel><title>Azimuth Devlog</title>
<item><title>0.10.0 progress</title><link>https://example.itch.io/azimuth/devlog/3/p</link><pubDate>Sun, 18 Oct 2026 10:26:28 GMT</pubDate>
<description><![CDATA[<p>Work on 0.10.0 &amp; more</p>]]></description></item>
<item><title>Bug reports</title><link>https://example.itch.io/azimuth/devlog/2/b</link><pubDate>Sat, 17 Oct 2026 08:19:57 GMT</pubDate>
<description><![CDATA[<p>Hi!</p>]]></description></item>
<item><title>Update 0.9.10</title><link>https://example.itch.io/azimuth/devlog/1/u</link><pubDate>Thu, 15 Oct 2026 21:57:32 GMT</pubDate>
<description><![CDATA[<p>New triggers</p>]]></description></item>
<item><title>Update 0.9.9</title><link>https://example.itch.io/azimuth/devlog/0/u</link><pubDate>Mon, 12 Oct 2026 21:57:32 GMT</pubDate>
<description>old</description></item>
</channel></rss>"""
RELEASES = json.dumps([{"draft": True, "tag_name": "v9"}, {"tag_name": "v1.0.0-rc5", "name": "FH2MP v1.0.0-rc5", "body": "- a\n- b",
                                                           "html_url": "https://github.com/x/y/releases/tag/v1.0.0-rc5"}]).encode()


class FeedTests(unittest.TestCase):
    def test_devlog(self):
        d = home.parse_devlog(RSS)
        self.assertEqual(d["latest"], "0.9.10")
        self.assertEqual([p["title"] for p in d["posts"]], ["0.10.0 progress", "Bug reports", "Update 0.9.10"])
        self.assertEqual(d["posts"][0]["summary"], "Work on 0.10.0 & more")
        self.assertEqual(d["posts"][0]["date"], 1792319188)

    def test_release_skips_drafts(self):
        self.assertEqual(home.parse_release(RELEASES)["tag"], "v1.0.0-rc5")

    def test_hourly_with_retry_and_last_good_copy(self):
        now, calls, fail = [1000.0], [], [False]

        def fetch(url, **kw):
            calls.append(url)
            if fail[0]:
                raise OSError("down")
            return RELEASES if "github" in url else RSS
        f = home.Feeds(fetch, clock=lambda: now[0])
        self.assertEqual(f.get("release")["tag"], "v1.0.0-rc5")
        f.get("release")
        self.assertEqual(len(calls), 1)                          # cached
        now[0] += home.FEED_TTL + 1
        fail[0] = True
        self.assertEqual(f.get("release")["tag"], "v1.0.0-rc5")  # the site is down: the old copy stays
        now[0] += 60
        f.get("release")
        self.assertEqual(len(calls), 2)                          # and it is not asked again until the retry time
        now[0] += home.FEED_RETRY
        fail[0] = False
        f.get("release")
        self.assertEqual(len(calls), 3)


    def test_first_fetch_is_waited_for_and_retried_soon(self):
        import threading
        gate, calls = threading.Event(), []

        def slow(url, **kw):
            calls.append(url)
            gate.wait(2)
            return RSS
        f = home.Feeds(slow)
        got = []
        threads = [threading.Thread(target=lambda: got.append(f.get("azimuth"))) for _ in range(3)]
        for t in threads:
            t.start()
        time.sleep(0.1)
        gate.set()
        for t in threads:
            t.join()
        self.assertEqual([g["latest"] for g in got], ["0.9.10"] * 3)        # nobody got an empty answer
        self.assertEqual(len(calls), 1)
        now, n = [0.0], []

        def down(url, **kw):
            n.append(url)
            raise OSError("down")
        f = home.Feeds(down, clock=lambda: now[0])
        self.assertIsNone(f.get("release"))
        now[0] += home.FIRST_RETRY + 1
        f.get("release")
        self.assertEqual(len(n), 2)                                          # nothing yet: tried again after a minute


class BuildTests(unittest.TestCase):
    def test_home_document(self):
        lsp = make_lsp()
        lsp.LAUNCHER.config.overlay = {"motd": "Hello", "latestAzimuth": "0.9.10"}
        sc = lsp.SB
        submit(sc, 1, "Alice", 91.5)
        submit(sc, 2, "Bob", 88.25)
        submit(sc, 3, "Cara", 90.0, event=1001)
        f = home.Feeds(lambda url, **kw: RELEASES if "github" in url else RSS)
        d = home.build(lsp, f)
        self.assertEqual(d["motd"], "Hello")
        self.assertEqual((d["release"]["tag"], d["azimuth"]["latest"], d["azimuth"]["supported"]), ("v1.0.0-rc5", "0.9.10", "0.9.10"))
        self.assertEqual([len(e["rows"]) for e in d["events"]], [2, 1])
        self.assertEqual({(r["gamertag"], r["event"]) for r in d["records"]}, {("Bob", "Event 0"), ("Cara", "Event 1")})
        self.assertEqual(d["cars"], [{"car": "Test Car", "runs": 3}])
        self.assertEqual(d["stats"]["runs"], 3)
        json.dumps(d)                                           # it must go out as JSON as it is

    def test_outside_sites_down(self):
        def down(url, **kw):
            raise OSError("no network")
        d = home.build(make_lsp(), home.Feeds(down))
        self.assertIsNone(d["release"])
        self.assertEqual(d["azimuth"]["posts"], [])


class AssetTests(unittest.TestCase):
    def test_only_plain_known_files(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "t1.wav"), "wb") as f:
                f.write(b"RIFF....WAVE")
            with open(os.path.join(d, "notes.txt"), "w") as f:
                f.write("x")
            ctx = launcher_api.Ctx(None, None, launcher_api.LauncherConfig(), lambda: 0, assets_dir=d)

            def get(name):
                st, obj, ctype = launcher_api.handle(ctx, "GET", "/v1/launcher/assets/" + name, {}, b"", "203.0.113.5")
                return st, ctype
            self.assertEqual(get("t1.wav"), (200, "audio/wav"))
            for bad in ("notes.txt", "missing.wav", "..%2Ft1.wav", "../t1.wav", "T1.WAV", ".hidden.wav"):
                self.assertEqual(get(bad)[0], 404, bad)
            ctx.assets_dir = ""
            self.assertEqual(get("t1.wav")[0], 404)


if __name__ == "__main__":
    unittest.main()
