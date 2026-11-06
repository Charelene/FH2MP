"""Drivatar data for the LSP stand-in.

* Reads the game's shipped Drivatar records (media/drivatarglobalcache/DefaultCache.bin: u32 version,
  u32 count, then `count` x (0x2650-byte native struct + 8 trailing bytes)) with the field offsets from
  the exe's own descriptors, giving wire-ready dicts for server/xls.py.
* Keeps the Drivatars players upload (SaveDrivatarInformation) in the `drivatars` table (db.py).
* Builds a Drivatar for a player profile from a shipped one used as a template.
"""
import json
import math
import os
import random
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db  # noqa: E402
import diag  # noqa: E402
import xls_schema as S  # noqa: E402
try:
    from car_policy import KNOWN_CARS  # noqa: E402     # tools/gamedata/make_car_policy.py
except ImportError:
    KNOWN_CARS = frozenset()

LOG = diag.get("drivatar")
_RATE = diag.RateLimit()

# GetDrivatarsForCache passes 120 as count and capacity (0xa22ee0); a longer reply fails to read (0xee5450) and the client keeps no cache at all, so it
# never reached DownloadCache.bin (0 of 20 rigs have one). A good reply is saved (0xa30310) and feeds SelectFromSkillMap 0xa2eff0, what a player races offline.
# The file persists on the client, so each skill keeps a floor of shipped records: a thin or wrong reply cannot leave a skill empty there.
CACHE_CAPACITY = 120
CACHE_SHIPPED_PER_SKILL = 12


def cache_limit():
    """FH2MP_DRIVATAR_CACHE_LIMIT: fewer Drivatars than the client's 120 (0 serves none). A test switch for dev: with a bigger car cache on the client, the
    world's Drivatars take every stand-in car id (0..11) before a player's car can."""
    try:
        return min(CACHE_CAPACITY, max(0, int(os.environ["FH2MP_DRIVATAR_CACHE_LIMIT"])))
    except (KeyError, ValueError):
        return CACHE_CAPACITY
SKILLS = range(5)
CACHE_PLAYERS = os.environ.get("FH2MP_DRIVATAR_CACHE_PLAYERS", "1") != "0"    # 0 serves shipped records only, which replaces the client's saved file
CARS = os.environ.get("FH2MP_DRIVATAR_CARS", "0") == "1"                      # real cars, tunes and liveries on player Drivatars
MAX_INFO_VALUE = 1e6         # the client does not check these floats after download (o3 1.4)

NATIVE = {0: "<B", 1: "<H", 2: "<h", 3: "<i", 4: "<I", 5: "<q", 6: "<Q", 0x13: "<Q", 7: "<f", 0x11: "<?"}
REC_SIZE = 0x2650
REC_STRIDE = REC_SIZE + 8

# Every shipped Drivatar carries the same gamertag ("ton3s") in the exe's data, so a lobby of shipped ones looked like a
# crowd of clones. Each template gets its own name instead: from a private local list when there is one (see _local_names),
# otherwise two plain words plus a number (at most 15 characters, the
# gamertag limit), derived from the template's index so it keeps its name across calls and every client sees the same one.
_ADJECTIVES = ("Swift", "Rapid", "Silent", "Brave", "Crimson", "Golden", "Iron", "Lucky", "Wild", "Neon", "Rusty", "Sunny",
               "Cosmic", "Frozen", "Turbo", "Smooth", "Fierce", "Mellow", "Dusty", "Electric", "Hidden", "Noble", "Quick",
               "Stormy", "Velvet", "Amber", "Bold", "Clever", "Daring", "Misty")
_NOUNS = ("Falcon", "Comet", "Badger", "Otter", "Raven", "Tiger", "Piston", "Rocket", "Lynx", "Mustang", "Cobra", "Bison",
          "Panda", "Hornet", "Gecko", "Viper", "Pelican", "Marlin", "Wolf", "Drifter", "Rider", "Sparrow", "Ember",
          "Phoenix", "Bandit", "Fox", "Heron", "Jaguar", "Kestrel", "Orca")


def _local_names():
    """Names from a private list (one per line): env FH2MP_DRIVATAR_NAMES, else server/data/drivatar_names.txt. That folder is
    not part of the repository, so a list of your own stays local; without one the generated names below are used."""
    path = os.environ.get("FH2MP_DRIVATAR_NAMES") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data",
                                                                    "drivatar_names.txt")
    try:
        with open(path, encoding="utf-8") as f:
            return [ln.strip()[:15] for ln in f if ln.strip()]
    except OSError:
        return []


_LOCAL_NAMES = _local_names()


def generated_tag(i):
    """Stable, distinct name for the i-th shipped Drivatar."""
    if _LOCAL_NAMES:
        return _LOCAL_NAMES[i % len(_LOCAL_NAMES)]
    adjective = _ADJECTIVES[i % len(_ADJECTIVES)]
    noun = _NOUNS[(i // len(_ADJECTIVES) + i) % len(_NOUNS)]
    return (adjective + noun + str(i % 100))[:15]


def read_native(sc, fields, buf, base):
    out = {}
    for f in fields:
        a = base + f.offset
        t = f.type
        if t in NATIVE:
            out[f.name] = struct.unpack_from(NATIVE[t], buf, a)[0]
        elif t == 9:
            out[f.name] = bytes(buf[a:a + f.size]).decode("utf-16-le", "replace").split("\0")[0]
        elif t == 0xC:
            out[f.name] = read_native(sc, sc.sub(f), buf, a)
        elif t == 0xE:
            n = struct.unpack_from("<I", buf, base + f.count_off)[0]
            el = sc.field(f.elem)
            items = []
            for i in range(n):
                ea = a + i * el.size
                if el.type == 0xC:
                    items.append(read_native(sc, sc.sub(el), buf, ea))
                else:
                    items.append(struct.unpack_from(NATIVE[el.type], buf, ea)[0])
            out[f.name] = items
        else:
            raise ValueError(f"unsupported native type {t:#x} ({f.name})")
    return out


def shipped_from_cache(sc, path):
    """The 125 Drivatars the game ships (media/drivatarglobalcache/DefaultCache.bin), as wire-ready dicts with generated gamertags.
    Only the exporter calls this: the server reads the result from the table `drivatar_shipped` of the catalog."""
    arr = sc.call_struct("ForzaWebServicesDrivatarGetDrivatarsForCache_Results")[0]
    rec_fields = sc.sub(sc.field(arr.elem))
    out = []
    with open(path, "rb") as f:
        d = f.read()
    ver, n = struct.unpack_from("<II", d, 0)
    for i in range(n):
        b = 8 + i * REC_STRIDE
        if b + REC_SIZE > len(d):
            break
        out.append(read_native(sc, rec_fields, d, b))
    for i, t in enumerate(out):
        t["GamerTag"] = generated_tag(i)
    return out


def sane_info(value):
    """Every number in an uploaded Drivatar is finite and in range."""
    if isinstance(value, dict):
        return all(sane_info(v) for v in value.values())
    if isinstance(value, list):
        return all(sane_info(v) for v in value)
    if isinstance(value, float):
        return math.isfinite(value) and abs(value) <= MAX_INFO_VALUE
    return True


class DrivatarStore:
    def __init__(self, sc, templates, db):
        """`templates`: the shipped Drivatars (list of dicts, see shipped_from_cache / the `drivatar_shipped` table)."""
        self.sc = sc
        self.db = db
        arr = sc.call_struct("ForzaWebServicesDrivatarGetDrivatarsForCache_Results")[0]
        self.rec_fields = sc.sub(sc.field(arr.elem))
        self.templates = list(templates)
        self.car_override = {}

    # ---- persistence (table `drivatars`: wire-shaped record as JSON plus the columns we select on)
    def count(self):
        return self.db.one("SELECT COUNT(*) FROM drivatars")[0]

    def bot_name(self, xuid):
        """Name of a shipped Drivatar bot, '' if the id is not one."""
        if not hasattr(self, "_tpl_names"):
            self._tpl_names = {t.get("Xuid"): t["GamerTag"] for t in self.templates}
        return self._tpl_names.get(xuid, "")

    def tag_for_xuid(self, xuid):
        """Name of a bot or a stored player Drivatar, '' for an unknown id: the profile service shows these names in lobbies."""
        name = self.bot_name(xuid)
        if name:
            return name
        r = self.db.one("SELECT gamertag FROM drivatars WHERE xuid=?", (xuid,))
        return r["gamertag"] if r else ""

    def gamertags(self):
        return [r[0] for r in self.db.query("SELECT gamertag FROM drivatars ORDER BY seq")]

    @staticmethod
    def _rec(row):
        rec = json.loads(row["record"])
        if row["locked"]:
            rec["_locked"] = True
        return rec

    def _get(self, c, xuid):
        r = c.execute("SELECT record, locked FROM drivatars WHERE xuid=?", (xuid,)).fetchone()
        return self._rec(r) if r else None

    @staticmethod
    def _put(c, rec):
        """Insert or replace a player's Drivatar (the lock flag travels as rec["_locked"])."""
        stored = {k: v for k, v in rec.items() if k != "_locked"}
        c.execute("INSERT INTO drivatars (xuid, gamertag, skill, wristband, locked, record, updated) VALUES (?,?,?,?,?,?,?) "
                  "ON CONFLICT(xuid) DO UPDATE SET gamertag=excluded.gamertag, skill=excluded.skill, "
                  "wristband=excluded.wristband, locked=excluded.locked, record=excluded.record, updated=excluded.updated",
                  (stored["Xuid"], stored["GamerTag"], stored["UserSkillLevel"], stored["wristbandLevel"],
                   int(bool(rec.get("_locked"))), json.dumps(stored), time.time()))

    # ---- records
    # A player's own Drivatar borrows its driving style from the shipped one closest to these. The
    # shipped set is 25 car-value buckets ("wristbandLevel") x 5 skill levels (0..4); 240000 is about
    # a Lamborghini Huracan's value.
    DEFAULT_SKILL = 3
    DEFAULT_WRISTBAND = 240000

    def template(self, skill=None, wristband=None):
        if not self.templates:
            return None
        skill = self.DEFAULT_SKILL if skill is None else skill
        wristband = self.DEFAULT_WRISTBAND if wristband is None else wristband
        return min(self.templates, key=lambda t: (abs(t["UserSkillLevel"] - skill),
                                                  abs(t["wristbandLevel"] - wristband)))

    def _for_player(self, c, xuid, gamertag):
        rec = self._get(c, xuid)
        before = json.dumps(rec, sort_keys=True)
        if rec is None:
            t = self.template()
            if t is None:
                return None
            rec = json.loads(json.dumps(t))
            rec["IsFriend"] = False
        rec["GamerTag"] = gamertag or rec["GamerTag"]
        rec["Xuid"] = xuid
        if str(xuid) in self.car_override:
            rec["CarId"] = self.car_override[str(xuid)]
        if json.dumps(rec, sort_keys=True) != before:
            db.touch_player(c, xuid, gamertag)
            self._put(c, rec)
        return rec

    def for_player(self, xuid, gamertag):
        """The Drivatar record for a player: their upload when we have one, else a profile-based fake."""
        with self.db.tx() as c:
            return self._for_player(c, xuid, gamertag)

    def add_player(self, xuid, gamertag, skill=None, wristband=None):
        """Creates (or replaces) a stored player Drivatar from the nearest shipped template."""
        t = self.template(skill, wristband)
        if t is None:
            return None
        rec = json.loads(json.dumps(t))
        rec.update({"GamerTag": gamertag[:15], "Xuid": xuid, "IsFriend": False})
        with self.db.tx() as c:
            db.touch_player(c, xuid, rec["GamerTag"])
            self._put(c, rec)
        return rec

    def remove(self, xuid):
        with self.db.tx() as c:
            return c.execute("DELETE FROM drivatars WHERE xuid=?", (xuid,)).rowcount > 0

    def edit(self, xuid, sets, lock=None):
        """Hand-tune a stored Drivatar. `sets` maps UserInfo field names (e.g. sideswipeMean, or base fields
        like timeRatioMean) or UserSkillLevel / wristbandLevel / CarId to values. lock=True keeps the
        game's own training uploads from overwriting the player's stats; lock=False releases it."""
        with self.db.tx() as c:
            rec = self._get(c, xuid)
            if rec is None:
                return None
            ui = rec["UserInfo"]
            for k, v in sets.items():
                if k in ui and not isinstance(ui[k], dict):
                    ui[k] = float(v)
                elif k in ui["base"]:
                    ui["base"][k] = float(v)
                elif k in ("UserSkillLevel", "wristbandLevel", "CarId", "BucketId"):
                    rec[k] = int(v)
                else:
                    raise KeyError(k)       # rolls the transaction back, nothing half-applied
            if lock is not None:
                rec["_locked"] = bool(lock)
            self._put(c, rec)
            return rec

    def save_upload(self, xuid, gamertag, info, curves, segments):
        if not (sane_info(info) and sane_info(curves) and sane_info(segments)):
            bad = [name for name, v in (("info", info), ("curves", curves), ("segments", segments)) if not sane_info(v)]
            LOG.warn(f"Drivatar upload refused: {', '.join(bad)} hold a value that is not finite or beyond +-{MAX_INFO_VALUE:g}, xuid={xuid:#x}")
            return False
        with self.db.tx() as c:
            rec = self._for_player(c, xuid, gamertag)
            if rec is None or rec.get("_locked"):
                LOG.info("Drivatar upload not stored: " + ("no shipped template to build a record from" if rec is None else "the stored Drivatar is locked by the operator")
                         + f" xuid={xuid:#x}")
                return
            rec["UserInfo"] = info
            rec["Curves"] = curves
            rec["Segments"] = segments
            self._put(c, rec)
        return True

    def set_wristband(self, xuid, wristband):
        """The car-value bucket of the car the player drives now (kept on their stored Drivatar)."""
        with self.db.tx() as c:
            rec = self._get(c, xuid)
            if rec is not None and rec["wristbandLevel"] != wristband:
                rec["wristbandLevel"] = wristband
                self._put(c, rec)

    def bucket_wristband(self, bucket):
        """Car-value bucket index (as the client sends it) -> the shipped Drivatars' wristband value.
        The shipped set has 25 distinct values; the index is their ascending order."""
        w = sorted({t["wristbandLevel"] for t in self.templates})
        return w[max(0, min(int(bucket), len(w) - 1))] if w else self.DEFAULT_WRISTBAND

    def _spread_wristband(self, rec):
        """The client shows a Drivatar's wristbandLevel as the level on its name tag. Free Roam sends bucket 0, so players'
        stored values piled up on the lowest one (46000) and every tag read the same. Such untouched records get a stable
        value from the shipped set's own spread, picked by their id; locked or already different ones are kept."""
        low = min((t["wristbandLevel"] for t in self.templates), default=None)
        if low is None or rec.get("_locked") or rec["wristbandLevel"] != low:
            return rec
        w = sorted({t["wristbandLevel"] for t in self.templates})
        rec["wristbandLevel"] = w[zlib.crc32(str(rec["Xuid"]).encode()) % len(w)]
        return rec

    def pool(self, limit, skill=None, near_wristband=None, exclude_xuid=None, exclude_xuids=(),
             any_skill_players=True):
        """Real players' Drivatars first (whatever their skill: there are few of them), then the shipped
        ones of the requested `skill`, nearest car-value bucket first."""
        skip = set(exclude_xuids)
        where, args = ["xuid <> ?", "xuid <> ALL(?::bigint[])"], [exclude_xuid or 0, sorted(skip)]
        if not any_skill_players and skill is not None:
            where.append("skill = ?")
            args.append(skill)
        rows = self.db.query(f"SELECT record, locked FROM drivatars WHERE {' AND '.join(where)} ORDER BY seq LIMIT ?",
                             args + [max(0, limit)])
        recs = list(map(self._rec, rows))
        players = [self._spread_wristband(rec) for rec in recs if sane_info(rec.get("UserInfo"))]
        self._note_skipped(len(recs) - len(players), "pool")
        tpl = [t for t in self.templates if skill is None or t["UserSkillLevel"] == skill]
        tpl = [t for t in tpl if t.get("Xuid") not in skip]
        if near_wristband is not None:
            tpl.sort(key=lambda t: abs(t["wristbandLevel"] - near_wristband))
        room = max(0, limit - len(players))
        window = tpl if near_wristband is None else tpl[:max(room * 4, 40)]   # nearest car values, but not always the same ones
        chosen = random.sample(window, min(room, len(window)))
        out = self.with_cars((players + chosen)[:limit])
        random.shuffle(out)
        return out

    def for_cache(self, limit, exclude_xuid=0):
        """The global cache: never more than the client holds, a floor of shipped records per skill, then recent players spread across the skills,
        then more shipped ones."""
        limit = min(limit, cache_limit())
        shipped = []
        for skill in SKILLS:
            of_skill = [t for t in self.templates if t["UserSkillLevel"] == skill]
            shipped += random.sample(of_skill, min(CACHE_SHIPPED_PER_SKILL, len(of_skill)))
        shipped = shipped[:limit]
        room = limit - len(shipped)
        players = []
        if CACHE_PLAYERS and room:
            rows = self.db.query("SELECT record, locked FROM drivatars WHERE xuid <> ? ORDER BY updated DESC LIMIT ?", (exclude_xuid, room * 4))
            recs = [rec for rec in map(self._rec, rows) if rec.get("GamerTag") and sane_info(rec.get("UserInfo"))]
            self._note_skipped(len(rows) - len(recs), "cache")
            per_skill = [[r for r in recs if r["UserSkillLevel"] == skill] for skill in SKILLS]
            while len(players) < room and any(per_skill):
                for of_skill in per_skill:
                    if of_skill and len(players) < room:
                        players.append(self._spread_wristband(of_skill.pop(0)))
        used = {t.get("Xuid") for t in shipped}
        rest = [t for t in self.templates if t.get("Xuid") not in used]
        fill = random.sample(rest, min(limit - len(shipped) - len(players), len(rest)))
        return self.with_cars(shipped + players + fill)

    @staticmethod
    def _note_skipped(n, where):
        """Stored Drivatars left out of a reply because their numbers fail sane_info (or they have no name): the client reads them without checking."""
        if n > 0:
            ok, skipped = _RATE.allow(("skipped", where), 600.0)
            if ok:
                LOG.warn(f"{n} stored Drivatar(s) left out of the {where} reply (no name, or a value not finite or beyond +-{MAX_INFO_VALUE:g})"
                         + (f"; {skipped} replies like it since the last line" if skipped else ""))

    def with_cars(self, recs):
        """A player's Drivatar in that player's own latest car, with their own tune and livery for it. The client checks the car against the bucket and,
        if it does not fit, swaps it and clears both ids (ValidateRecordCars 0xa30630), so a misfit costs nothing; a shipped record has no owner and is left alone."""
        bots = {t.get("Xuid") for t in self.templates}
        mine = [r for r in recs if r.get("Xuid") not in bots]
        if not CARS or not mine:
            return recs
        xuids = sorted({r["Xuid"] for r in mine})
        cars = {r["xuid"]: r["car"] for r in self.db.query(
            "SELECT DISTINCT ON (xuid) xuid, car FROM score_submissions WHERE xuid = ANY(?::bigint[]) AND car > 0 ORDER BY xuid, submitted DESC", (xuids,))}
        content = {(r["creator"], r["car_id"], r["kind"]): r["id"] for r in self.db.query(
            "SELECT DISTINCT ON (creator, car_id, kind) creator, car_id, kind, id FROM ugc_versioned WHERE creator = ANY(?::bigint[]) "
            "AND kind IN ('livery', 'tune') AND refs > 0 ORDER BY creator, car_id, kind, created DESC", (xuids,))}
        out = []
        for r in recs:
            car = cars.get(r.get("Xuid"))
            if car is not None and car in KNOWN_CARS:
                r = dict(r, CarId=car, TuneId=content.get((r["Xuid"], car, "tune"), -1), LiveryId=content.get((r["Xuid"], car, "livery"), -1))
            out.append(r)
        return out
