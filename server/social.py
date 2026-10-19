"""Friends, profile settings and gamerpics: the data behind Xbox Live's social services as the client's XSAPI asks for them.

The client reads its friend list, the profiles of those friends and their presence through XSAPI (social, profile and userpresence
services; the game has FriendsModule, GamerPicCacheModule and a presence module). There is no Xbox Live to ask, so the LSP answers
those calls from this module: a mutual friend graph (managed through the admin API / the future launcher, there is no friend
request screen in the game), per-player profile settings and gamerpics. A player without an uploaded gamerpic gets a generated
identicon, so every xuid has a picture. Presence is a lease: a player is online while their client keeps polling the LSP.
"""
import hashlib
import time

import pngtool
import ugc

MAX_GAMERPIC = 1_000_000          # bytes of an uploaded PNG
ONLINE_SECONDS = 40               # a client polls the Message Center every 5 s
FH2_TITLE_ID = "446059611"
LAST_POLL = {}                    # xuid -> time of the last request, for presence (in memory on purpose: a lease)


def touch(xuid):
    if xuid:
        LAST_POLL[xuid] = time.time()


def online(xuid):
    return time.time() - LAST_POLL.get(xuid, 0) < ONLINE_SECONDS


def identicon(xuid, size=160):
    """A deterministic 5x5 mirrored block picture (PNG) in the colour of the xuid's hash."""
    h = hashlib.sha256(b"fh2mp-gamerpic-%d" % xuid).digest()
    colour = (60 + h[0] % 160, 60 + h[1] % 160, 60 + h[2] % 160)
    cells = [[(h[3 + y * 3 + min(x, 4 - x)] & 1) for x in range(5)] for y in range(5)]
    bg, cell = (240, 240, 240), size // 7
    off = (size - cell * 5) // 2
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            cx, cy = (px - off) // cell, (py - off) // cell
            on = 0 <= cx < 5 and 0 <= cy < 5 and px >= off and py >= off and cells[cy][cx]
            row += bytes(colour if on else bg)
        rows.append(row)
    return pngtool.encode(size, size, 3, rows)


class SocialStore:
    def __init__(self, db):
        self.db = db

    # ---------------------------------------------------------------- friends
    def friends(self, xuid):
        return [r[0] for r in self.db.query(
            "SELECT CASE WHEN a=? THEN b ELSE a END FROM friendships WHERE a=? OR b=? ORDER BY created", (xuid, xuid, xuid))]

    def are_friends(self, x, y):
        a, b = min(x, y), max(x, y)
        return bool(self.db.one("SELECT 1 FROM friendships WHERE a=? AND b=?", (a, b)))

    def add_friend(self, x, y):
        if x == y:
            raise ValueError("a player cannot befriend themselves")
        with self.db.tx() as c:
            c.execute("INSERT INTO friendships (a, b, created) VALUES (?,?,?) ON CONFLICT DO NOTHING", (min(x, y), max(x, y), time.time()))

    def remove_friend(self, x, y):
        with self.db.tx() as c:
            return c.execute("DELETE FROM friendships WHERE a=? AND b=?", (min(x, y), max(x, y))).rowcount > 0

    # ---------------------------------------------------------------- profiles
    def profile(self, xuid):
        """Everything XSAPI's profile settings can ask for, as a dict of strings (the wire values)."""
        p = self.db.one("SELECT display_name, gamerscore, tier, gamerpic_at FROM player_profiles WHERE xuid=?", (xuid,))
        pl = self.db.one("SELECT gamertag FROM players WHERE xuid=?", (xuid,))
        tag = (pl["gamertag"] if pl else "") or f"Player{xuid % 100000}"
        name = (p["display_name"] if p else "") or tag
        return {"Gamertag": tag, "GameDisplayName": name, "AppDisplayName": name, "Gamerscore": str(p["gamerscore"] if p else 0),
                "AccountTier": p["tier"] if p else "Gold", "RealName": "", "Bio": "", "Location": "", "TenureLevel": "0",
                "XboxOneRep": "GoodPlayer", "PreferredColor": "", "WatermarkLabels": "", "ShowUserAsAvatar": "0",
                "_pic_version": int(p["gamerpic_at"] or 0) if p else 0}

    def set_gamerpic(self, xuid, png):
        if png[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError("the gamerpic must be a PNG")
        if len(png) > MAX_GAMERPIC:
            raise ValueError(f"the gamerpic is larger than {MAX_GAMERPIC} bytes")
        with self.db.tx() as c:
            c.execute("INSERT INTO player_profiles (xuid, gamerpic, gamerpic_at) VALUES (?,?,?) ON CONFLICT (xuid) DO UPDATE SET "
                      "gamerpic=excluded.gamerpic, gamerpic_at=excluded.gamerpic_at", (xuid, ugc.pack(png), time.time()))

    def clear_gamerpic(self, xuid):
        with self.db.tx() as c:
            c.execute("UPDATE player_profiles SET gamerpic=NULL, gamerpic_at=? WHERE xuid=?", (time.time(), xuid))

    def set_profile(self, xuid, display_name=None, gamerscore=None, tier=None):
        with self.db.tx() as c:
            c.execute("INSERT INTO player_profiles (xuid) VALUES (?) ON CONFLICT DO NOTHING", (xuid,))
            for col, v in (("display_name", display_name), ("gamerscore", gamerscore), ("tier", tier)):
                if v is not None:
                    c.execute(f"UPDATE player_profiles SET {col}=? WHERE xuid=?", (v, xuid))

    def gamerpic(self, xuid):
        """PNG bytes: the uploaded one, else a generated identicon."""
        r = self.db.one("SELECT gamerpic FROM player_profiles WHERE xuid=?", (xuid,))
        return ugc.unpack(r["gamerpic"]) if r and r["gamerpic"] else identicon(xuid)

    def players(self):
        return self.db.query("SELECT p.xuid, p.gamertag, p.career_level, (SELECT COUNT(*) FROM friendships f WHERE f.a=p.xuid OR f.b=p.xuid) AS friends, "
                             "(pp.gamerpic IS NOT NULL) AS has_gamerpic FROM players p LEFT JOIN player_profiles pp ON pp.xuid=p.xuid "
                             "WHERE p.gamertag <> '' ORDER BY p.last_seen DESC")


# ---------------------------------------------------------------- the XSAPI answers (plain dicts, the LSP serialises them)

def profile_user(store, xuid, settings, pic_url):
    prof = store.profile(xuid)
    url = pic_url(xuid, prof["_pic_version"])
    values = dict(prof, GameDisplayPicRaw=url, AppDisplayPicRaw=url)
    return {"id": str(xuid), "hostId": str(xuid), "settings": [{"id": s, "value": values.get(s, "")} for s in settings],
            "isSponsoredUser": False}


def people(store, xuid):
    return {"people": [{"xuid": str(f), "isFavorite": False, "isFollowingCaller": True, "socialNetworks": []} for f in store.friends(xuid)],
            "totalCount": len(store.friends(xuid))}


def summary(store, xuid):
    n = len(store.friends(xuid))
    return {"targetFollowingCount": n, "targetFollowerCount": n, "isCallerFollowingTarget": False, "isTargetFollowingCaller": False,
            "hasCallerMarkedTargetAsFavorite": False, "hasCallerMarkedTargetAsIdentityShared": False, "legacyFriendStatus": "None",
            "availablePeopleSlots": 1000, "recentChangeCount": 0, "watermark": "0"}


def presence(xuid):
    if not online(xuid):
        return {"xuid": str(xuid), "state": "Offline"}
    return {"xuid": str(xuid), "state": "Online", "devices": [{"type": "XboxOne", "titles": [
        {"id": FH2_TITLE_ID, "name": "Forza Horizon 2", "placement": "Full", "state": "Active",
         "lastModified": time.strftime("%Y-%m-%dT%H:%M:%S.0000000Z", time.gmtime())}]}]}
