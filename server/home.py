"""What the launcher's home page (and the website, if it likes) shows: GET /v1/home, public, one JSON document.

    motd, news, heroImage          the operator's settings (launcher.json / PUT /ops/v1/config)
    release                        the newest GitHub release of the project: {tag, name, url, published, body (markdown)}
    azimuth                        {latest, supported, posts: [{title, url, date, summary}]}: the newest "Update x.y.z" post of the Azimuth
                                   devlog on itch.io, the build the shim supports, the three newest devlog posts
    rotation, events               this month's Monthly Rivals events with their best three real times (ops_api.challenges)
    records                        the newest #1 times on those events, newest first
    cars                           the most raced cars of the last seven days (Rivals submissions) with their run counts
    stats                          {online, accounts, clubs, runs, designs}

GitHub and itch.io are asked by the server (at most hourly, again after a failure; the last good answer is kept), so players'
launchers never call them and GitHub's 60-calls-an-hour limit for one address does not matter. Built only from public data.
"""
import html
import json
import os
import re
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
from email.utils import parsedate_to_datetime

import ops_api

GITHUB_REPO = os.environ.get("FH2MP_GITHUB_REPO", "Charelene/FH2MP")
ITCH_FEED = "https://almost-there-games.itch.io/azimuth/devlog.rss"
FEED_TTL = 3600
FEED_RETRY = 600
FIRST_RETRY = 60
CAR_DAYS = 7
_UPDATE_RE = re.compile(r"^\s*(?:update|azimuth)\s+v?(\d+\.\d+\.\d+)\b", re.I)


def _get(url, limit=1_000_000, accept=None):
    req = urllib.request.Request(url, headers={"User-Agent": "fh2mp-server", **({"Accept": accept} if accept else {})})
    with urllib.request.urlopen(req, timeout=6) as r:
        return r.read(limit)


def _plain(fragment, limit=400):
    """HTML snippet -> plain text."""
    text = html.unescape(re.sub(r"<[^>]+>", " ", fragment or ""))
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "…"


def parse_release(raw):
    for r in json.loads(raw):
        if not r.get("draft"):
            return {"tag": str(r.get("tag_name") or ""), "name": str(r.get("name") or ""), "url": str(r.get("html_url") or ""),
                    "published": str(r.get("published_at") or ""), "body": str(r.get("body") or "")[:20000]}
    return None


def parse_devlog(raw):
    root = ET.fromstring(raw)
    posts = []
    for it in root.iter("item"):
        title, link = (it.findtext("title") or "").strip(), (it.findtext("link") or "").strip()
        try:
            date = int(parsedate_to_datetime(it.findtext("pubDate") or "").timestamp())
        except (TypeError, ValueError):
            date = 0
        if title and link.startswith("https://"):
            posts.append({"title": title, "url": link, "date": date, "summary": _plain(it.findtext("description"))})
    latest = next((m.group(1) for m in (_UPDATE_RE.match(p["title"]) for p in posts) if m), "")
    return {"latest": latest, "posts": posts[:3]}


class Feeds:
    """The two outside sources, each refreshed at most hourly. While one thread refreshes, the others serve the last good copy; before
    there is any copy they wait for that first fetch (so the first launchers after a restart do not get a home page without them).
    A failed fetch is retried after FEED_RETRY, or FIRST_RETRY while there is nothing yet."""

    def __init__(self, fetch=_get, clock=time.time):
        self.fetch, self.clock = fetch, clock
        self.data = {"release": None, "azimuth": None}
        self.due = {"release": 0.0, "azimuth": 0.0}
        self.locks = {k: threading.Lock() for k in self.data}

    def _load(self, key):
        if key == "release":
            return parse_release(self.fetch(f"https://api.github.com/repos/{GITHUB_REPO}/releases?per_page=5",
                                            accept="application/vnd.github+json"))
        return parse_devlog(self.fetch(ITCH_FEED))

    def get(self, key):
        if self.clock() < self.due[key]:
            return self.data[key]
        lock = self.locks[key]
        if not lock.acquire(blocking=self.data[key] is None):
            return self.data[key]                      # someone else is refreshing it
        try:
            if self.clock() < self.due[key]:           # it was fetched while this thread waited
                return self.data[key]
            try:
                self.data[key] = self._load(key)
                self.due[key] = self.clock() + FEED_TTL
            except Exception:  # noqa: BLE001 - an outside site being down must never break /v1/home
                self.due[key] = self.clock() + (FEED_RETRY if self.data[key] is not None else FIRST_RETRY)
            return self.data[key]
        finally:
            lock.release()


FEEDS = Feeds()


def top_cars(lsp, days=CAR_DAYS, limit=5):
    names = lsp.RIV.names()["car"]
    rows = lsp.DB.query("SELECT car, COUNT(*) AS n FROM score_submissions WHERE submitted > ? GROUP BY car ORDER BY n DESC, car LIMIT ?",
                        (time.time() - days * 86400, limit * 2))
    return [{"car": names[r["car"]], "runs": int(r["n"])} for r in rows if names.get(r["car"])][:limit]


def build(lsp, feeds=FEEDS):
    c = lsp.LAUNCHER.config.get()
    ch = ops_api.challenges(lsp, 3)
    records = sorted(({"gamertag": e["rows"][0]["gamertag"], "xuid": e["rows"][0]["xuid"], "event": e["name"], "score": e["rows"][0]["score"],
                       "car": e["rows"][0]["car"], "class": e["rows"][0]["class"], "at": e["rows"][0]["at"]}
                      for e in ch["events"] if e["rows"]), key=lambda r: -r["at"])[:3]
    az = dict(feeds.get("azimuth") or {"latest": "", "posts": []})
    az["supported"] = c.get("latestAzimuth", "")
    return {
        "motd": c.get("motd", ""), "news": c.get("news", []), "heroImage": c.get("heroImage", ""),
        "release": feeds.get("release"), "azimuth": az,
        "rotation": ch["rotation"], "events": ch["events"], "records": records, "cars": top_cars(lsp),
        "stats": {"online": lsp.online_count(), "accounts": lsp.AUTH.stats()["accounts"],
                  "clubs": lsp.DB.one("SELECT COUNT(*) FROM clubs")[0], "runs": lsp.SB.count(), "designs": lsp.UGC.count()},
        "time": int(time.time()),
    }
