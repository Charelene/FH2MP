"""Drivatar data for the LSP stand-in.

* Reads the game's shipped Drivatar records (media/drivatarglobalcache/DefaultCache.bin: u32 version,
  u32 count, then `count` x (0x2650-byte native struct + 8 trailing bytes)) with the field offsets from
  the exe's own descriptors, giving wire-ready dicts for server/xls.py.
* Keeps the Drivatars players upload (SaveDrivatarInformation) in server/data/drivatars.json.
* Builds a Drivatar for a player profile from a shipped one used as a template.
"""
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xls_schema as S  # noqa: E402

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
            el = S.Field(sc.exe, f.elem)
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


class DrivatarStore:
    def __init__(self, sc, default_cache_path, data_dir):
        self.sc = sc
        arr = sc.call_struct("ForzaWebServicesDrivatarGetDrivatarsForCache_Results")[0]
        self.rec_fields = sc.sub(S.Field(sc.exe, arr.elem))
        self.path = os.path.join(data_dir, "drivatars.json")
        self.templates = []
        if default_cache_path and os.path.exists(default_cache_path):
            d = open(default_cache_path, "rb").read()
            ver, n = struct.unpack_from("<II", d, 0)
            for i in range(n):
                b = 8 + i * REC_STRIDE
                if b + REC_SIZE > len(d):
                    break
                self.templates.append(read_native(sc, self.rec_fields, d, b))
            for i, t in enumerate(self.templates):
                t["GamerTag"] = generated_tag(i)
        self.players = {}   # xuid(str) -> record dict (wire shaped)
        self.car_override = {}
        self._load()

    # ---- persistence
    def _load(self):
        try:
            with open(self.path) as f:
                self.players = json.load(f).get("players", {})
        except FileNotFoundError:
            pass

    def _save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"players": self.players}, f)
        os.replace(tmp, self.path)

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

    def for_player(self, xuid, gamertag):
        """The Drivatar record for a player: their upload when we have one, else a profile-based fake."""
        key = str(xuid)
        rec = self.players.get(key)
        if rec is None:
            t = self.template()
            if t is None:
                return None
            rec = json.loads(json.dumps(t))
            rec["IsFriend"] = False
            self.players[key] = rec
        rec["GamerTag"] = gamertag or rec["GamerTag"]
        rec["Xuid"] = xuid
        if key in self.car_override:
            rec["CarId"] = self.car_override[key]
        self._save()
        return rec

    def add_player(self, xuid, gamertag, skill=None, wristband=None):
        """Creates (or replaces) a stored player Drivatar from the nearest shipped template."""
        t = self.template(skill, wristband)
        if t is None:
            return None
        rec = json.loads(json.dumps(t))
        rec.update({"GamerTag": gamertag[:15], "Xuid": xuid, "IsFriend": False})
        self.players[str(xuid)] = rec
        self._save()
        return rec

    def remove(self, xuid):
        return self.players.pop(str(xuid), None) is not None and (self._save() or True)

    def edit(self, xuid, sets, lock=None):
        """Hand-tune a stored Drivatar. `sets` maps UserInfo field names (e.g. sideswipeMean, or base fields
        like timeRatioMean) or UserSkillLevel / wristbandLevel / CarId to values. lock=True keeps the
        game's own training uploads from overwriting the player's stats; lock=False releases it."""
        rec = self.players.get(str(xuid))
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
                raise KeyError(k)
        if lock is not None:
            rec["_locked"] = bool(lock)
        self._save()
        return rec

    def save_upload(self, xuid, gamertag, info, curves, segments):
        rec = self.for_player(xuid, gamertag)
        if rec is None:
            return
        if rec.get("_locked"):
            return
        rec["UserInfo"] = info
        rec["Curves"] = curves
        rec["Segments"] = segments
        self._save()

    def bucket_wristband(self, bucket):
        """Car-value bucket index (as the client sends it) -> the shipped Drivatars' wristband value.
        The shipped set has 25 distinct values; the index is their ascending order."""
        w = sorted({t["wristbandLevel"] for t in self.templates})
        return w[max(0, min(int(bucket), len(w) - 1))] if w else self.DEFAULT_WRISTBAND

    def pool(self, limit, skill=None, near_wristband=None, exclude_xuid=None, exclude_xuids=(),
             any_skill_players=True):
        """Real players' Drivatars first (whatever their skill: there are few of them), then the shipped
        ones of the requested `skill`, nearest car-value bucket first."""
        skip = set(exclude_xuids)
        players = [r for r in self.players.values()
                   if r["Xuid"] != exclude_xuid and r["Xuid"] not in skip
                   and (any_skill_players or skill is None or r["UserSkillLevel"] == skill)]
        tpl = [t for t in self.templates if skill is None or t["UserSkillLevel"] == skill]
        tpl = [t for t in tpl if t.get("Xuid") not in skip]
        if near_wristband is not None:
            tpl.sort(key=lambda t: abs(t["wristbandLevel"] - near_wristband))
        room = max(0, limit - len(players))
        window = tpl[:max(room * 4, 40)]                 # nearest car values, but not always the same ones
        chosen = random.sample(window, min(room, len(window)))
        out = (players + chosen)[:limit]
        random.shuffle(out)
        return out
