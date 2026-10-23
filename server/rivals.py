"""Monthly Rivals event catalog for the LSP stand-in.

Turn 10 curated the Monthly Rivals events and that service is gone, so events are generated: each month (and on every admin
re-roll) a seeded random pick of closed circuits, each paired with a random restriction theme drawn from the game's own
categories. Everything comes from the user's install, read in place (nothing is stored in the repo):
- circuits and their lengths: `CarDb.route_info` (route files; closed = start and finish within 150 m),
- names: `strtab` (track names, brands, body styles, countries, drive types, car names),
- restrictions: attributes of the game's `<CarRestrictions>` (CarClassId, BodyFamilyId, MakeId, ModelId, YearMin/YearMax,
  DriveTypeId, CountryId) [names taken from the exe's option table; which ones the game honours is still being tested].

Event ids are 16-bit on the wire (ForzaTimeTrial.id is a u16) and double as the scoreboard key, so every rotation gets a fresh
id range: 1000 + (months since 2026-01) * 160 + reroll * 16 + index (reroll < 10, index < 16).
"""
import datetime
import json
import random
import time

import rentals

CLASS_LETTERS = ("D", "C", "B", "A", "S1", "S2")
ERAS = ((None, 1969, "Classics"), (1970, 1989, "Retro Icons"), (1990, 1999, "90s Heroes"), (2000, 2009, "2000s Icons"),
        (2010, None, "Modern Machines"))
MIN_CARS = 4          # a theme must match at least this many cars of the game
CLASS_ENFORCED = {"body", "make", "era", "drive", "country"}    # themes whose card class (average of the matching cars) is also enforced
EVENTS_PER_ROTATION = 12
TODS = (600, 900, 1200, 1500, 1800, 2000, 2300, 300)          # HHMM, as the career data stores it
WEATHER_WORDS = {"Clear": "clear", "CloudyA": "cloudy", "CloudyB": "overcast", "CloudyFog": "foggy", "ThreateningFog": "misty"}


def tod_word(tod):
    """HHMM -> a natural word for the time of day."""
    h = (tod // 100) % 24
    for lo, hi, word in ((0, 3, "midnight"), (3, 6, "pre-dawn"), (6, 9, "morning"), (9, 11, "late morning"), (11, 14, "midday"),
                         (14, 17, "afternoon"), (17, 20, "evening"), (20, 22, "night"), (22, 24, "midnight")):
        if lo <= h < hi:
            return word
    return "day"


def tod_seconds(tod):
    """HHMM -> seconds since midnight: the unit of GameOptions.TimeOfDay (verified in game: 1200 started at ~00:20)."""
    return (tod // 100) * 3600 + (tod % 100) * 60


def conditions(tod, weather_adj):
    """'a stormy midday', 'a clear night', 'an overcast day', 'morning' ... or None."""
    if tod is None and weather_adj is None:
        return None
    if tod is None:
        return f"{weather_adj} skies"
    word = tod_word(tod)
    if weather_adj is None:
        return word
    return f"{'an' if weather_adj[0] in 'aeiou' else 'a'} {weather_adj} {word}"
ID_BASE = 1000
EPOCH = (2026, 1)


# CarRestrictions attribute -> (v_cars column, SQL operator). Names follow the exe's option table [inferred].
ATTR_COLUMN = {"CarClassId": ("class_id", "="), "BodyFamilyId": ("body_id", "="), "MakeId": ("make_id", "="),
               "ModelId": ("model_id", "="), "YearMin": ("year", ">="), "YearMax": ("year", "<="),
               "DriveTypeId": ("drive_id", "="), "CountryId": ("country_id", "=")}


def where_for(attrs):
    """SQL condition (and args) on v_cars for a set of CarRestrictions attributes."""
    cond = [f"{ATTR_COLUMN[k][0]} {ATTR_COLUMN[k][1]} ?" for k in attrs]
    return (" AND ".join(cond) or "1=1"), tuple(attrs.values())


class RivalCatalog:
    """`cars` is a CarDb opened on the game catalog (server/catalog.py): its views v_cars, v_tracks, v_makes, ... carry the names."""

    def __init__(self, cars, mount=None, database=None):
        self.cars, self.database = cars, database
        self._names = None
        self._themes = None
        self._cache = None

    # ---------------------------------------------------------------- names (from the catalog views)
    def names(self):
        if self._names is None:
            q = self.cars.db.execute
            self._names = {
                "track": {r[0]: (r[1] or "", r[2] or "") for r in q("SELECT id, name, short_name FROM v_tracks")},
                "make": {r[0]: r[1] or "" for r in q("SELECT id, name FROM v_makes")},
                "body": {r[0]: r[1] or "" for r in q("SELECT id, name FROM v_bodies")},
                "country": {r[0]: r[1] or "" for r in q("SELECT id, name FROM v_countries")},
                "drive": {r[0]: r[1] or "" for r in q("SELECT id, name FROM v_drive_types")},
                "car": {r[0]: r[1] or "" for r in q("SELECT id, name FROM v_cars")},
            }
        return self._names

    # ---------------------------------------------------------------- cars and themes
    def cars_for(self, attrs):
        """[(car id, class id, PI, top speed m/s)] of the cars a restriction allows (all cars for {})."""
        where, args = where_for(attrs or {})
        return [tuple(r) for r in self.cars.db.execute(
            f"SELECT id, class_id, pi, COALESCE(top_speed_ms, 0) FROM v_cars WHERE {where} ORDER BY pi", args)]

    def rentals(self):
        """Car ids of the game's RentalCars table (empty when the catalog has none)."""
        if getattr(self, "_rentals", None) is None:
            try:
                self._rentals = {r[0] for r in self.cars.db.execute("SELECT CarId FROM RentalCars")}
            except Exception:  # noqa: BLE001 - a catalog without the table: no rental check
                self._rentals = set()
        return self._rentals

    def usable_themes(self):
        """Themes a new event may use: the class can be enforced and the car selector offers a rental to anybody who owns no fitting car
        (rentals.enough: 2 rentals, or all cars of a smaller theme). Without rentals in the catalog, or when no theme passes, every theme
        that can be enforced (the stock 22 rentals fit almost none: tools/make_rentals_db.py adds the missing ones)."""
        if getattr(self, "_usable", None) is None:
            themes = [t for t in self.themes() if not t.get("avoid")]
            rent = self.rentals()
            if rent:
                ok = [t for t in themes if rentals.enough({r[0] for r in self.cars_for(t["attrs"])}, rent)]
                themes = ok or themes
            self._usable = themes
        return self._usable

    def _count(self, attrs):
        where, args = where_for(attrs)
        r = self.cars.db.execute(f"SELECT COUNT(*), AVG(class_id) FROM v_cars WHERE {where}", args).fetchone()
        return r[0], (round(r[1]) if r[1] is not None else None)

    def themes(self):
        """Candidate restrictions: [{"kind", "attrs", "title", "phrase", "cars", "cls"}], each matching >= MIN_CARS cars."""
        if self._themes is not None:
            return self._themes
        n, out = self.names(), []

        def add(kind, attrs, title, phrase, cls=None):
            count, avg = self._count(attrs)
            if count >= MIN_CARS or kind == "model" and count >= 1:
                theme = {"kind": kind, "attrs": attrs, "title": title, "phrase": phrase, "cars": count,
                         "cls": cls if cls is not None else avg}
                if kind in CLASS_ENFORCED and cls is None:
                    # the card shows a class (the average of the matching cars); enforce it too, so the car selector agrees with the card
                    counts = {c: self._count(dict(attrs, CarClassId=c))[0] for c in range(len(CLASS_LETTERS))}
                    pick = avg if counts.get(avg, 0) >= MIN_CARS else max(counts, key=counts.get)    # the average class, else the most common one
                    if counts[pick] >= MIN_CARS:
                        theme.update(attrs=dict(attrs, CarClassId=pick), cars=counts[pick], cls=pick,
                                     phrase=f"{phrase}, {CLASS_LETTERS[pick]} class", base_phrase=phrase)
                    else:
                        theme["avoid"] = True       # the shown class cannot be enforced: no new events use it; kept so old events still resolve
                out.append(theme)
        out.append({"kind": "open", "attrs": {}, "title": "Open Class Dash", "phrase": "Any car",
                    "cars": self._count({})[0], "cls": 4})
        for c in range(0, 5):
            add("class", {"CarClassId": c}, f"{CLASS_LETTERS[c]} Class Dash", f"{CLASS_LETTERS[c]} class cars only", c)
        for b, name in n["body"].items():
            if name and name.lower() != "none":
                add("body", {"BodyFamilyId": b}, f"{name} Cup", f"{name} cars only")
        for m, name in n["make"].items():
            if name:
                add("make", {"MakeId": m}, f"{name} Showdown", f"{name} cars only")
        for lo, hi, name in ERAS:
            attrs = {}
            if lo is not None:
                attrs["YearMin"] = lo
            if hi is not None:
                attrs["YearMax"] = hi
            add("era", attrs, f"{name} Cup",
                f"Cars from {lo} to {hi}" if lo and hi else (f"Cars up to {hi}" if hi else f"Cars from {lo}"))
        for d, name in n["drive"].items():
            if name:
                add("drive", {"DriveTypeId": d}, f"{name} Only Trial", f"{name} cars only")
        for cid, name in n["country"].items():
            if name:
                add("country", {"CountryId": cid}, f"Made in {name} Cup", f"Cars built in {name}")
        for model_id, car_id in self.cars.db.execute("SELECT model_id, MIN(id) FROM v_cars GROUP BY model_id"):
            name = n["car"].get(car_id, "")
            if name:
                add("model", {"ModelId": model_id}, f"One-Make: {name}"[:60], f"{name} only")
        self._themes = out
        return out

    # ---------------------------------------------------------------- rotation
    def serial(self, now=None):
        """(month index since 2026-01, re-roll counter of that month)."""
        d = now or datetime.date.today()
        month = (d.year - EPOCH[0]) * 12 + d.month - EPOCH[1]
        stored = self.database.one("SELECT value FROM meta WHERE key='rival_reroll'") if self.database else None
        n = 0
        if stored:
            m, _, c = stored["value"].partition(":")
            n = int(c) if int(m) == month else 0
        return month, min(n, 9)

    def rotate(self):
        """Admin re-roll: a different set right now (stays until the next re-roll or month)."""
        month, n = self.serial()
        if self.database is not None:
            with self.database.tx() as c:
                c.execute("INSERT INTO meta (key, value) VALUES ('rival_reroll', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                          (f"{month}:{min(n + 1, 9)}",))
        self._cache = None
        return self.events()

    def _row(self, r):
        return {"id": r["id"], "track": r["track"], "laps": r["laps"], "length": r["length"], "kind": r["kind"],
                "attrs": json.loads(r["attrs"]), "cls": r["cls"], "name": r["name"], "trackname": r["trackname"],
                "description": r["description"], "tod": r["tod"], "weather": r["weather"], "wetness": r["wetness"],
                "precip": r["precip"]}

    def events(self):
        """This rotation's events: loaded from the database, or generated once and stored."""
        month, n = self.serial()
        if self._cache and self._cache[0] == (month, n):
            return self._cache[1]
        if self.database is not None:
            rows = self.database.query("SELECT * FROM rival_events WHERE month=? AND reroll=? ORDER BY idx", (month, n))
            if rows:
                ev = [self._row(r) for r in rows]
                self._cache = ((month, n), ev)
                return ev
        ev = self._generate(month, n)
        if self.database is not None and ev:
            with self.database.tx() as c:
                for k, e in enumerate(ev):
                    c.execute("INSERT OR IGNORE INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, "
                              "name, trackname, description, created, tod, weather, wetness, precip) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                              (e["id"], month, n, k, e["track"], e["laps"], e["length"], e["kind"], json.dumps(e["attrs"]),
                               e["cls"], e["name"], e["trackname"], e["description"], time.time(), e["tod"], e["weather"],
                               e["wetness"], e["precip"]))
        self._cache = ((month, n), ev)
        return ev

    def _weather_combos(self):
        """(preset id, environment wetness, precipitation) combos the game's own career events use (known-good)."""
        if not hasattr(self, "_combos"):
            self._combos = [tuple(r) for r in self.cars.db.execute(
                "SELECT DISTINCT WeatherPreset, WeatherEnvironmentWetness, WeatherPrecipitationLevel FROM AnthemEventRaces "
                "WHERE CustomWeather=1 AND WeatherPreset IS NOT NULL ORDER BY 1, 2, 3")]
        return self._combos

    def weather_word(self, preset, precip):
        """Adjective for a weather preset ('stormy', 'foggy', 'ominous', ...)."""
        name = self.cars.db.execute("SELECT Name FROM WeatherPreset WHERE Id=?", (preset,)).fetchone()
        name = name[0] if name else str(preset)
        if name in WEATHER_WORDS:
            return WEATHER_WORDS[name]
        if "Tropical" in name:
            return "tropical stormy"
        if "Fog" in name:
            return "foggy and stormy"
        return "stormy" if precip and precip > 0.3 else "ominous"

    def _phrase(self, kind, attrs):
        for t in self.themes():
            if t["kind"] == kind and t["attrs"] == attrs:
                return t["phrase"]
        plain = {k: v for k, v in attrs.items() if k != "CarClassId"}          # events stored before the class was enforced
        for t in self.themes():
            if t["kind"] == kind and {k: v for k, v in t["attrs"].items() if k != "CarClassId"} == plain:
                return t.get("base_phrase", t["phrase"])
        return None

    def enforce_stored_classes(self):
        """Events stored before the card class was enforced (GitHub issue: "Made in Italy" shows A class but the car selector lets any Italian
        car in). Their restriction has no CarClassId although the card shows the class; add it (the same class the card shows) when at least
        MIN_CARS cars match (otherwise the theme's most common class, which also becomes the class on the card), so the selector agrees with the card. Times already on those boards stay. Idempotent; returns the number of
        events changed (their text is refreshed by refresh_texts)."""
        if self.database is None:
            return 0
        changed = 0
        with self.database.tx() as c:
            for r in c.execute("SELECT * FROM rival_events").fetchall():
                e = self._row(r)
                if e["kind"] not in CLASS_ENFORCED or "CarClassId" in e["attrs"] or e["cls"] is None:
                    continue
                pick = e["cls"]
                if self._count(dict(e["attrs"], CarClassId=pick))[0] < MIN_CARS:
                    # the class on the card has too few cars to be enforced: use the theme's most common class instead (as new events do)
                    counts = {k: self._count(dict(e["attrs"], CarClassId=k))[0] for k in range(len(CLASS_LETTERS))}
                    pick = max(counts, key=counts.get)
                    if counts[pick] < MIN_CARS:
                        continue
                c.execute("UPDATE rival_events SET attrs=?, cls=? WHERE id=?",
                          (json.dumps(dict(e["attrs"], CarClassId=pick)), pick, r["id"]))
                changed += 1
        self._cache = None
        return changed

    def refresh_texts(self):
        """Recomputes every stored event's description from its stored fields (wording fixes reach old rotations; ids,
        tracks, restrictions and conditions never change). Returns the number of rows updated."""
        if self.database is None:
            return 0
        changed = 0
        with self.database.tx() as c:
            for r in c.execute("SELECT * FROM rival_events").fetchall():
                e = self._row(r)
                text = self.describe(e)
                if text != r["description"]:
                    c.execute("UPDATE rival_events SET description=? WHERE id=?", (text, r["id"]))
                    changed += 1
        self._cache = None
        return changed

    def describe(self, e, tname=None):
        """Event description text from its stored fields."""
        names = self.names()
        full = tname or names["track"].get(e["track"], ("", ""))[0] or f"Circuit {e['track']}"
        theme = {"open": "Any car", "class": f"{CLASS_LETTERS[e['cls']] if e['cls'] is not None else ''} class cars only"}.get(e["kind"])
        phrase = e.get("phrase") or self._phrase(e["kind"], e["attrs"]) or theme or ""
        cond = conditions(e["tod"], self.weather_word(e["weather"], e["precip"]) if e["weather"] is not None else None)
        parts = [f"One lap of {full} ({e['length'] / 1000:.1f} km)."]
        if phrase:
            parts.append(phrase + ".")
        if cond:
            parts.append(f"Conditions: {cond}.")
        parts.append("Beat the rival's time.")
        return " ".join(parts)[:500]

    def _conditions(self, rng, k):
        """(time of day HHMM or None, weather preset or None, wetness, precipitation). Verified in game (2026-10-18): the stored HHMM is
        converted to seconds for GameOptions.TimeOfDay and the weather attributes are honoured."""
        tod = rng.choice(TODS) if rng.random() < 0.7 else None
        combos = self._weather_combos()
        w = rng.choice(combos) if combos and rng.random() < 0.5 else None
        return (tod,) + ((w[0], w[1], w[2]) if w else (None, None, None))

    def _generate(self, month, n):
        rng = random.Random(f"fh2mp-rivals-{month}-{n}")
        names = self.names()
        by_route = {}
        for tid, route, length in self.cars.db.execute(
                "SELECT id, route, length_m FROM v_tracks WHERE active=1 AND closed=1 AND length_m BETWEEN 1500 AND 3700 ORDER BY id"):
            by_route.setdefault(route, (tid, length))
        tracks = list(by_route.values())
        rng.shuffle(tracks)
        themes = self.usable_themes()
        kinds = sorted({t["kind"] for t in themes})
        rng.shuffle(kinds)
        events, used = [], set()
        for k, (tid, length) in enumerate(tracks[:EVENTS_PER_ROTATION]):
            kind = kinds[k % len(kinds)]
            pool = [t for t in themes if t["kind"] == kind and id(t) not in used] or themes
            theme = rng.choice(pool)
            used.add(id(theme))
            full, short = names["track"].get(tid, ("", ""))
            tname = (full or f"Circuit {tid}")[:60]
            tod, weather, wetness, precip = self._conditions(rng, k)
            events.append({"id": ID_BASE + month * 160 + n * 16 + k, "track": tid, "laps": 1, "length": length,
                           "kind": theme["kind"], "attrs": theme["attrs"], "cls": theme["cls"],
                           "name": f"{theme['title']}: {short or tname}"[:120], "trackname": tname,
                           "description": "", "phrase": theme["phrase"],
                           "tod": tod, "weather": weather, "wetness": wetness, "precip": precip})
            events[-1]["description"] = self.describe(events[-1], tname)
        return events

    def by_id(self, event_id):
        """Any stored event (also from earlier rotations, so old boards, ghosts and notifications keep resolving)."""
        for e in self.events():
            if e["id"] == event_id:
                return e
        if self.database is not None:
            r = self.database.one("SELECT * FROM rival_events WHERE id=?", (event_id,))
            if r:
                return self._row(r)
        return None
