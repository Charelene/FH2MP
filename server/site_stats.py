"""The website's statistics beyond the plain boards (forza.scuffed.af; ops_api.py routes them): records and their history, the Monthly
Rivals hall of fame, speed traps, car and make usage, the shared-design storefront, population history and online event results.

Everything is read from stored rows (score_submissions, rival_events, ugc_files, accounts, stats_samples, mode_results). Writes are
only sample() (players online, once a minute, from the LSP's sampler thread) and add_result() (an event result the game server posts
to /admin/result). The record history is one pass over every time and speed run, kept until a new run is stored.

    GET /ops/v1/records[?limit=50]                 record feed (every new #1, newest first) + longest held, most #1s, closest gaps
    GET /ops/v1/hall-of-fame                       every Monthly Rivals month with each event's podium and the month's champion
    GET /ops/v1/traps[?limit=10]                   speed trap and speed zone boards (score in m/s)
    GET /ops/v1/cars[?days=&class=&limit=]         most raced cars, makes and classes (days=0: all time)
    GET /ops/v1/cars/<id>                          one car: catalog facts, best time per board, records, drivers, runs per week
    GET /ops/v1/garage[?kind=&sort=&limit=&offset=]  shared liveries / tunes / photos, by likes, downloads or newest
    GET /ops/v1/garage/<id>/preview.png            a design's preview picture (the PNG the creator's game uploaded)
    GET /ops/v1/history[?range=24h|7d|30d]         players online over time, peaks, sign-ups / runs / racers per day
    GET /ops/v1/totals                             all-time counters and the online event tallies
"""
import json
import threading
import time

import ops_api
import rivals
import scoreboard
import ugc

FESTIVAL = (1, 1)            # (scoreboardType, scoreType): Festival Routes, one board per route and class
MONTHLY = (3, 1)             # Monthly Rivals, one board per event
SPEED_KINDS = {5: "trap", 6: "zone"}      # scoreType of the speed boards (scoreboardType 9), one board per trap / zone id
TIME_KINDS = ("route", "event")           # boards where the lower score (seconds) wins; on traps and zones the higher speed wins
RUN_FILTER = "sb IN (1, 3, 9)"            # the runs a car is raced in: Festival Routes, Monthly Rivals, speed traps and zones
ALL_CLASSES = rivals.CLASS_LETTERS + ("X", "R")
HISTORY_RECHECK = 15         # seconds between "has a run been stored?" checks of the cached record history
GARAGE_KINDS = ("livery", "tune", "photo")
GARAGE_SORTS = {"likes": "f.likes DESC, f.downloads DESC", "downloads": "f.downloads DESC, f.likes DESC", "new": "f.created DESC"}
RANGES = {"24h": (86400, 600), "7d": (7 * 86400, 3600), "30d": (30 * 86400, 4 * 3600)}     # span, bucket (seconds)
DAYS = 30                    # per-day series length
SAMPLE_SECONDS = 60
KEEP_SAMPLES = 90 * 86400
MODES = ("race", "team", "king", "infection")
RESULT_MAX = 16384           # bytes of one posted result
WEEKS = 12


def _letter(cls):
    return ALL_CLASSES[cls] if cls is not None and 0 <= cls < len(ALL_CLASSES) else ""


def _who(lsp, xuid, tag=""):
    return ops_api._gamertag(lsp, {"tag": tag, "xuid": xuid})


def _int(text, what):
    try:
        return int(text)
    except (TypeError, ValueError):
        raise ops_api.OpsError(404, "not_found", f"unknown {what}")


# ---------------------------------------------------------------- catalog

def catalog(lsp):
    """car id -> {"id", "name", "make", "year", "class", "country", "drive", "body", "topSpeed"} from the game catalog (cached)."""
    cached = getattr(lsp, "_site_cars", None)
    if cached is not None:
        return cached
    out = {}
    cars = getattr(lsp, "CARS", None)
    if cars is not None:
        for r in cars.db.query("SELECT id, name, make, year, class_id, country, drive, body, top_speed_ms FROM v_cars"):
            out[r["id"]] = {"id": r["id"], "name": f"{r['make'] or ''} {r['name'] or ''}".strip(), "make": r["make"] or "",
                            "year": r["year"], "class": _letter(r["class_id"]), "country": r["country"] or "", "drive": r["drive"] or "",
                            "body": r["body"] if r["body"] not in (None, "None") else "", "topSpeed": r["top_speed_ms"]}
    lsp._site_cars = out
    return out


def _car_name(lsp, car_id):
    c = catalog(lsp).get(car_id)
    return c["name"] if c else lsp.RIV.names()["car"].get(car_id, "")


# ---------------------------------------------------------------- record history

class _Boards:
    """What a run's board is called: route / Monthly event / trap / zone. Built with each record scan."""

    def __init__(self, lsp):
        self.route_of = {}                       # track id -> (destination, route)
        for d in ops_api.route_catalog(lsp):
            for r in d["routes"]:
                for t in r["tracks"]:
                    self.route_of[t] = (d["name"], r)
        self.routes = {r["id"]: v for v in self.route_of.values() for r in (v[1],)}
        self.events = {e["id"]: e for e in lsp.DB.query("SELECT id, month, name, trackname, cls FROM rival_events")}

    def key(self, r):
        """(kind, board id, class or -1) of a stored run, or None when the website shows no board for it."""
        sb, st = r["sb"], r["st"]
        if (sb, st) == FESTIVAL:
            found = self.route_of.get(r["track"])
            return ("route", found[1]["id"], r["cls"]) if found and 0 <= r["cls"] < len(rivals.CLASS_LETTERS) else None
        if (sb, st) == MONTHLY:
            return ("event", r["gsid"], -1) if r["gsid"] in self.events and r["cls"] not in scoreboard.MAXED_CLASSES else None
        if sb in scoreboard.SPEED_BOARDS and st in SPEED_KINDS and r["gsid"] > 0 and r["score"] <= scoreboard.SPEED_CAP:
            return (SPEED_KINDS[st], r["gsid"], -1)
        return None

    def info(self, key):
        kind, bid, cls = key
        if kind == "route":
            dest, route = self.routes[bid]
            return {"kind": kind, "id": bid, "name": f"{dest} {route['name']}", "class": _letter(cls), "unit": "s"}
        if kind == "event":
            e = self.events[bid]
            return {"kind": kind, "id": bid, "name": e["name"], "class": _letter(e["cls"]), "unit": "s", "month": e["month"]}
        return {"kind": kind, "id": bid, "name": f"Speed {'Trap' if kind == 'trap' else 'Zone'} {bid}", "class": "", "unit": "m/s"}


class _History:
    def __init__(self, boards):
        self.boards = boards
        self.records = []           # every new #1 in the order it happened
        self.per_board = {}         # key -> {"best": {xuid: run}, "record": record}


_HISTORY_LOCK = threading.Lock()


def _scan(lsp):
    h = _History(_Boards(lsp))
    rows = lsp.DB.query("SELECT s.id, s.xuid, s.sb, s.st, s.gsid, s.track, s.car, s.cls, s.score, s.submitted, COALESCE(p.gamertag, '') AS tag "
                        "FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid "
                        "WHERE (s.sb = ? AND s.st = ?) OR (s.sb = ? AND s.st = ?) OR s.sb = 9 ORDER BY s.submitted, s.id",
                        FESTIVAL + MONTHLY)
    for r in rows:
        key = h.boards.key(r)
        if key is None:
            continue
        lower = key[0] in TIME_KINDS
        run = {"xuid": r["xuid"], "tag": r["tag"], "score": r["score"], "car": r["car"], "cls": r["cls"], "at": r["submitted"]}
        b = h.per_board.setdefault(key, {"best": {}, "record": None})
        mine = b["best"].get(r["xuid"])
        if mine is None or (run["score"] < mine["score"] if lower else run["score"] > mine["score"]):
            b["best"][r["xuid"]] = run
        cur = b["record"]
        if cur is None or (run["score"] < cur["score"] if lower else run["score"] > cur["score"]):
            rec = {**run, "key": key, "prev": cur, "until": None}
            if cur is not None:
                cur["until"] = run["at"]
            b["record"] = rec
            h.records.append(rec)
    return h


def record_history(lsp):
    """The cached scan, redone when a run has been stored since (checked at most every HISTORY_RECHECK seconds)."""
    with _HISTORY_LOCK:
        cache = getattr(lsp, "_site_history", None)
        now = time.monotonic()
        if cache and now - cache["checked"] < HISTORY_RECHECK:
            return cache["data"]
        stamp = tuple(lsp.DB.one("SELECT COUNT(*), MAX(id) FROM score_submissions"))
        if not cache or cache["stamp"] != stamp:
            cache = {"stamp": stamp, "data": _scan(lsp)}
        cache["checked"] = now
        lsp._site_history = cache
        return cache["data"]


def _record_out(lsp, h, rec, now):
    prev = rec["prev"]
    return {"board": h.boards.info(rec["key"]), "xuid": rec["xuid"], "gamertag": _who(lsp, rec["xuid"], rec["tag"]), "score": rec["score"],
            "car": _car_name(lsp, rec["car"]), "carId": rec["car"], "class": _letter(rec["cls"]), "at": int(rec["at"]),
            "previous": {"xuid": prev["xuid"], "gamertag": _who(lsp, prev["xuid"], prev["tag"]), "score": prev["score"]} if prev else None,
            "margin": abs(prev["score"] - rec["score"]) if prev else None,
            "until": int(rec["until"]) if rec["until"] is not None else None,
            "held": int((rec["until"] if rec["until"] is not None else now) - rec["at"])}


def records(lsp, limit, top=10):
    h = record_history(lsp)
    now = time.time()
    current = [b["record"] for b in h.per_board.values() if b["record"]]
    held, total = {}, {}
    for rec in current:
        held[rec["xuid"]] = held.get(rec["xuid"], 0) + 1
    for rec in h.records:
        total[rec["xuid"]] = total.get(rec["xuid"], 0) + 1
    tags = {rec["xuid"]: rec["tag"] for rec in h.records}
    holders = sorted(held, key=lambda x: (-held[x], -total[x], _who(lsp, x, tags[x]).lower()))[:top]
    closest = []
    for key, b in h.per_board.items():
        if key[0] in TIME_KINDS and len(b["best"]) >= 2:
            first, second = sorted(b["best"].values(), key=lambda r: (r["score"], r["at"]))[:2]
            closest.append((second["score"] - first["score"], key, first, second))
    closest.sort(key=lambda c: (c[0], -max(c[2]["at"], c[3]["at"])))

    def run(r):
        return {"xuid": r["xuid"], "gamertag": _who(lsp, r["xuid"], r["tag"]), "score": r["score"], "car": _car_name(lsp, r["car"]), "carId": r["car"]}
    return {"feed": [_record_out(lsp, h, rec, now) for rec in reversed(h.records[-limit:])] if limit else [],
            "longest": [_record_out(lsp, h, rec, now) for rec in sorted(current, key=lambda r: r["at"])[:top]],
            "holders": [{"xuid": x, "gamertag": _who(lsp, x, tags[x]), "current": held[x], "total": total[x]} for x in holders],
            "closest": [{"board": h.boards.info(key), "gap": gap, "first": run(a), "second": run(b)} for gap, key, a, b in closest[:top]],
            "totals": {"records": len(h.records), "boards": len(current), "holders": len(held)}}


# ---------------------------------------------------------------- Monthly Rivals hall of fame

def hall_of_fame(lsp, podium=3):
    """Every month with Monthly Rivals times (and the running one): each event's podium and the champion, the driver with the most event
    wins that month (more podiums, then more second places break a tie)."""
    now_month, now_reroll = lsp.RIV.serial()
    names = lsp.RIV.names()["car"]
    months = {}
    for e in lsp.DB.query("SELECT id, month, reroll, name, trackname, laps, length, cls FROM rival_events ORDER BY month DESC, reroll DESC, idx"):
        top = lsp.SB.top(*MONTHLY, -1, e["id"], podium)
        live = (e["month"], e["reroll"]) == (now_month, now_reroll)
        if not top and not live:
            continue                                  # a re-rolled set nobody raced
        m = months.setdefault(e["month"], {"month": e["month"], "label": ops_api.month_label(e["month"]), "current": e["month"] == now_month,
                                           "events": []})
        m["events"].append({"id": e["id"], "name": e["name"], "track": e["trackname"], "laps": e["laps"], "km": round(e["length"] / 1000.0, 2),
                            "class": _letter(e["cls"]), "podium": [ops_api._board_row(lsp, r, names) for r in top]})
    for m in months.values():
        score = {}
        for e in m["events"]:
            for r in e["podium"]:
                s = score.setdefault(r["xuid"], [0, 0, 0, r["gamertag"]])
                s[r["position"] - 1] += 1
        best = sorted(score.items(), key=lambda kv: (-kv[1][0], -sum(kv[1][:3]), -kv[1][1], kv[1][3].lower()))
        m["champion"] = ({"xuid": best[0][0], "gamertag": best[0][1][3], "wins": best[0][1][0], "podiums": sum(best[0][1][:3])}
                         if best and best[0][1][0] else None)
    return {"months": [months[k] for k in sorted(months, reverse=True)]}


# ---------------------------------------------------------------- speed traps and zones

def traps(lsp, limit):
    names = lsp.RIV.names()["car"]
    out = {}
    for st, kind in SPEED_KINDS.items():
        ids = [r["gsid"] for r in lsp.DB.query("SELECT DISTINCT gsid FROM score_submissions WHERE sb = 9 AND st = ? AND gsid > 0 AND score <= ? "
                                               "ORDER BY gsid", (st, scoreboard.SPEED_CAP))]
        out[kind + "s"] = [{"id": g, "name": f"Speed {'Trap' if kind == 'trap' else 'Zone'} {g}",
                            "rows": [ops_api._board_row(lsp, r, names) for r in lsp.SB.top(9, st, -1, g, limit)]} for g in ids]
    return out


# ---------------------------------------------------------------- cars and makes

def cars(lsp, days, cls, limit):
    since = time.time() - days * 86400 if days > 0 else 0
    where, args = f"submitted >= ? AND {RUN_FILTER}", [since]
    letter = cls if cls in rivals.CLASS_LETTERS else ""
    if letter:
        where += " AND cls = ?"
        args.append(rivals.CLASS_LETTERS.index(letter))
    cat = catalog(lsp)
    per_car, per_make = {}, {}
    for r in lsp.DB.query(f"SELECT car, xuid, COUNT(*) AS n FROM score_submissions WHERE {where} GROUP BY car, xuid", args):
        name = _car_name(lsp, r["car"])
        if not name:
            continue                                  # a car id the catalog does not know (car pack): nothing to call it
        c = per_car.setdefault(r["car"], {"runs": 0, "drivers": 0})
        c["runs"] += r["n"]
        c["drivers"] += 1
        make = cat.get(r["car"], {}).get("make", "")
        if make:
            m = per_make.setdefault(make, {"runs": 0, "drivers": set(), "cars": set()})
            m["runs"] += r["n"]
            m["drivers"].add(r["xuid"])
            m["cars"].add(r["car"])
    held_car, held_make = {}, {}
    for b in record_history(lsp).per_board.values():
        rec = b["record"]
        if rec and (not letter or _letter(rec["cls"]) == letter):
            held_car[rec["car"]] = held_car.get(rec["car"], 0) + 1
            make = cat.get(rec["car"], {}).get("make", "")
            held_make[make] = held_make.get(make, 0) + 1
    top = sorted(per_car, key=lambda c: (-per_car[c]["runs"], -per_car[c]["drivers"], _car_name(lsp, c)))[:limit]
    country = {c["make"]: c["country"] for c in cat.values()}
    classes = {r["cls"]: r["n"] for r in lsp.DB.query(f"SELECT cls, COUNT(*) AS n FROM score_submissions WHERE submitted >= ? AND {RUN_FILTER} "
                                                      "GROUP BY cls", (since,))}
    return {"days": days, "class": letter, "total": sum(c["runs"] for c in per_car.values()),
            "cars": [{**_car_facts(lsp, c), "runs": per_car[c]["runs"], "drivers": per_car[c]["drivers"], "records": held_car.get(c, 0)}
                     for c in top],
            "makes": [{"make": k, "country": country.get(k, ""), "runs": v["runs"], "drivers": len(v["drivers"]), "cars": len(v["cars"]),
                       "records": held_make.get(k, 0)} for k, v in sorted(per_make.items(), key=lambda kv: (-kv[1]["runs"], kv[0]))],
            "classes": [{"class": letter, "runs": classes.get(i, 0)} for i, letter in enumerate(rivals.CLASS_LETTERS)]}


def _car_best(lsp, h, key, r):
    rec = h.per_board.get(key, {}).get("record")
    return {"board": h.boards.info(key), "xuid": r["xuid"], "gamertag": _who(lsp, r["xuid"], r["tag"]), "score": r["score"],
            "class": _letter(r["cls"]), "at": int(r["submitted"]),
            "record": rec is not None and (rec["car"], rec["xuid"], rec["score"]) == (r["car"], r["xuid"], r["score"])}


def _car_facts(lsp, car_id):
    c = catalog(lsp).get(car_id)
    return dict(c) if c else {"id": car_id, "name": _car_name(lsp, car_id), "make": "", "year": None, "class": "", "country": "", "drive": "",
                              "body": "", "topSpeed": None}


def car(lsp, car_id):
    car_id = _int(car_id, "car")
    stats = lsp.DB.one(f"SELECT COUNT(*) AS runs, COUNT(DISTINCT xuid) AS drivers, MIN(submitted) AS first FROM score_submissions "
                       f"WHERE car = ? AND {RUN_FILTER}", (car_id,))
    if car_id not in catalog(lsp) and not stats["runs"]:
        raise ops_api.OpsError(404, "not_found", "unknown car")
    h = record_history(lsp)
    best = {}
    for r in lsp.DB.query("SELECT s.xuid, s.sb, s.st, s.gsid, s.track, s.car, s.cls, s.score, s.submitted, COALESCE(p.gamertag, '') AS tag "
                          f"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.car = ? AND s.{RUN_FILTER}", (car_id,)):
        key = h.boards.key(r)
        if key is None:
            continue
        cur = best.get(key)
        lower = key[0] in TIME_KINDS
        if cur is None or (r["score"] < cur["score"] if lower else r["score"] > cur["score"]) or (r["score"] == cur["score"] and r["submitted"] < cur["submitted"]):
            best[key] = r
    order = {"route": 0, "event": 1, "trap": 2, "zone": 3}
    boards = sorted(best, key=lambda k: (order[k[0]], h.boards.info(k)["name"], k[2]))
    now = time.time()
    held = [rec for b in h.per_board.values() for rec in (b["record"],) if rec and rec["car"] == car_id]
    start = int(now) - WEEKS * 7 * 86400
    weeks = {r["w"]: r["n"] for r in lsp.DB.query(f"SELECT (CAST(submitted AS BIGINT) - ?) / 604800 AS w, COUNT(*) AS n FROM score_submissions "
                                                  f"WHERE car = ? AND submitted >= ? AND {RUN_FILTER} GROUP BY w", (start, car_id, start))}
    drivers = lsp.DB.query("SELECT s.xuid, COALESCE(p.gamertag, '') AS tag, COUNT(*) AS n FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid "
                           f"WHERE s.car = ? AND s.{RUN_FILTER} GROUP BY s.xuid, p.gamertag ORDER BY n DESC, s.xuid LIMIT 10", (car_id,))
    return {"car": _car_facts(lsp, car_id), "runs": stats["runs"], "drivers": stats["drivers"],
            "firstRun": int(stats["first"]) if stats["first"] else None,
            "best": [_car_best(lsp, h, k, best[k]) for k in boards],
            "records": [_record_out(lsp, h, rec, now) for rec in sorted(held, key=lambda r: -r["at"])],
            "topDrivers": [{"xuid": r["xuid"], "gamertag": _who(lsp, r["xuid"], r["tag"]), "runs": r["n"]} for r in drivers],
            "weeks": [{"start": start + i * 604800, "runs": weeks.get(i, 0)} for i in range(WEEKS)]}


# ---------------------------------------------------------------- shared designs (storefront)

def garage(lsp, kind, sort, limit, offset, top=5):
    if kind not in GARAGE_KINDS:
        raise ops_api.OpsError(400, "bad_request", f"kind must be one of {', '.join(GARAGE_KINDS)}")
    if sort not in GARAGE_SORTS:
        raise ops_api.OpsError(400, "bad_request", f"sort must be one of {', '.join(GARAGE_SORTS)}")
    rows = lsp.DB.query("SELECT f.id, f.owner, f.title, f.description, f.created, f.car_id, f.class_id, f.downloads, f.likes, f.uses, "
                        "COALESCE(p.gamertag, '') AS tag, CASE WHEN f.preview IS NULL OR LENGTH(f.preview) = 0 THEN 0 ELSE 1 END AS has_preview "
                        "FROM ugc_files f LEFT JOIN players p ON p.xuid = f.owner WHERE f.removed = 0 AND f.kind = ? "
                        f"ORDER BY {GARAGE_SORTS[sort]}, f.id DESC LIMIT ? OFFSET ?", (kind, limit, offset))
    counts = {r["kind"]: r["n"] for r in lsp.DB.query("SELECT kind, COUNT(*) AS n FROM ugc_files WHERE removed = 0 GROUP BY kind")}
    creators = lsp.DB.query("SELECT f.owner, COALESCE(p.gamertag, '') AS tag, COUNT(*) AS n, SUM(f.downloads) AS downloads, SUM(f.likes) AS likes "
                            "FROM ugc_files f LEFT JOIN players p ON p.xuid = f.owner WHERE f.removed = 0 AND f.kind IN ('livery', 'tune') "
                            "GROUP BY f.owner, p.gamertag ORDER BY downloads DESC, likes DESC, n DESC, f.owner LIMIT ?", (top,))
    return {"kind": kind, "sort": sort, "total": counts.get(kind, 0), "counts": {k: counts.get(k, 0) for k in GARAGE_KINDS},
            "items": [{"id": r["id"], "title": r["title"], "description": r["description"], "xuid": r["owner"], "gamertag": _who(lsp, r["owner"], r["tag"]),
                       "car": _car_name(lsp, r["car_id"]) if r["car_id"] else "", "carId": r["car_id"], "class": _letter(r["class_id"]),
                       "downloads": r["downloads"], "likes": r["likes"], "uses": r["uses"], "created": int(r["created"]),
                       "preview": bool(r["has_preview"])} for r in rows],
            "creators": [{"xuid": r["owner"], "gamertag": _who(lsp, r["owner"], r["tag"]), "designs": r["n"], "downloads": int(r["downloads"] or 0),
                          "likes": int(r["likes"] or 0)} for r in creators if r["downloads"] or r["likes"]]}


def garage_preview(lsp, file_id):
    r = lsp.DB.one("SELECT preview FROM ugc_files WHERE id = ? AND removed = 0", (_int(file_id, "design"),))
    data = ugc.unpack(r["preview"]) if r else b""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ops_api.OpsError(404, "not_found", "no preview")
    return data


# ---------------------------------------------------------------- population

def sample(lsp, now=None):
    """One stats_samples row (called once a minute), the all-time peak in meta, and old samples dropped once an hour."""
    now = time.time() if now is None else now
    online, lobbies = lsp.online_count(), lsp.xbl.count()
    with lsp.DB.tx() as c:
        c.execute("INSERT OR IGNORE INTO stats_samples (at, online, lobbies) VALUES (?,?,?)", (now, online, lobbies))
        peak = c.execute("SELECT value FROM meta WHERE key = 'peak_online'").fetchone()
        if online > 0 and (peak is None or online > int(peak[0].split(":")[0])):
            c.execute("INSERT INTO meta (key, value) VALUES ('peak_online', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                      (f"{online}:{int(now)}",))
        if int(now) // SAMPLE_SECONDS % 60 == 0:
            c.execute("DELETE FROM stats_samples WHERE at < ?", (now - KEEP_SAMPLES,))
    return online, lobbies


def start_sampler(lsp):
    def run():
        while True:
            time.sleep(SAMPLE_SECONDS - time.time() % SAMPLE_SECONDS)
            try:
                sample(lsp)
            except Exception as e:  # noqa: BLE001 - a database hiccup must not end the sampler
                lsp.log(f"stats sampler: {e!r}")
    t = threading.Thread(target=run, name="stats-sampler", daemon=True)
    t.start()
    return t


def _peak(lsp):
    r = lsp.DB.one("SELECT value FROM meta WHERE key = 'peak_online'")
    if not r:
        return None
    n, _, at = r["value"].partition(":")
    return {"online": int(n), "at": int(at or 0)}


def _per_day(lsp, sql, start):
    return {r["d"]: r["n"] for r in lsp.DB.query(sql, (start, start))}


def history(lsp, rng):
    if rng not in RANGES:
        raise ops_api.OpsError(400, "bad_request", f"range must be one of {', '.join(RANGES)}")
    span, step = RANGES[rng]
    now = int(time.time())
    start = (now - span) // step * step
    points = [[start + r["b"] * step, int(r["online"]), int(r["lobbies"])] for r in lsp.DB.query(
        "SELECT (CAST(at AS BIGINT) - ?) / ? AS b, MAX(online) AS online, MAX(lobbies) AS lobbies FROM stats_samples WHERE at >= ? "
        "GROUP BY b ORDER BY b", (start, step, start))]
    peak = max(points, key=lambda p: (p[1], -p[0]), default=None)
    day0 = (now // 86400 - (DAYS - 1)) * 86400
    signups = _per_day(lsp, "SELECT (CAST(created AS BIGINT) - ?) / 86400 AS d, COUNT(*) AS n FROM accounts WHERE created >= ? GROUP BY d", day0)
    runs = _per_day(lsp, "SELECT (CAST(submitted AS BIGINT) - ?) / 86400 AS d, COUNT(*) AS n FROM score_submissions WHERE submitted >= ? GROUP BY d", day0)
    racers = _per_day(lsp, "SELECT (CAST(submitted AS BIGINT) - ?) / 86400 AS d, COUNT(DISTINCT xuid) AS n FROM score_submissions "
                           "WHERE submitted >= ? GROUP BY d", day0)
    return {"range": rng, "step": step, "start": start, "end": now, "points": points,
            "peak": {"online": peak[1], "at": peak[0]} if peak and peak[1] else None, "allTimePeak": _peak(lsp),
            "days": [{"day": day0 + i * 86400, "signups": signups.get(i, 0), "runs": runs.get(i, 0), "racers": racers.get(i, 0)}
                     for i in range(DAYS)]}


# ---------------------------------------------------------------- online event results (posted by the game server)

def add_result(lsp, body):
    """/admin/result: {"mode": race|team|king|infection, "detail": {...}} from the game server (loopback only). -> stored id."""
    if len(body) > RESULT_MAX:
        raise ValueError("result too large")
    d = json.loads(body or b"{}")
    if not isinstance(d, dict) or d.get("mode") not in MODES or not isinstance(d.get("detail"), dict):
        raise ValueError("expected {\"mode\": race|team|king|infection, \"detail\": {...}}")
    with lsp.DB.tx() as c:
        return c.execute("INSERT INTO mode_results (at, mode, detail) VALUES (?,?,?)",
                         (time.time(), d["mode"], json.dumps(d["detail"], separators=(",", ":")))).lastrowid


def _ladder(lsp, counts, top=5):
    return [{"xuid": x, "gamertag": _who(lsp, x), "count": n} for x, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:top]]


def modes(lsp):
    """Tallies of the online events: races and their winners (first finisher), red vs blue (the first finisher's team), King winners
    (longest crown time), crowns taken and the longest crown, Infection rounds, infections and survivors, the rentals people raced."""
    races, teams = {"n": 0, "players": 0, "wins": {}}, {"n": 0, "red": 0, "blue": 0}
    king = {"n": 0, "steals": {}, "wins": {}, "longest": None}
    inf = {"n": 0, "everyone": 0, "fastest": None, "hits": {}, "survived": {}}
    rentals = {}
    for r in lsp.DB.query("SELECT mode, detail FROM mode_results ORDER BY at"):
        try:
            d = json.loads(r["detail"])
        except ValueError:
            continue
        people = [p for p in d.get("players", []) if isinstance(p, dict)]
        for p in people:
            if p.get("rental") and p.get("car"):
                rentals[p["car"]] = rentals.get(p["car"], 0) + 1
        mode, winner = r["mode"], d.get("winner") or 0
        if mode in ("race", "team"):
            races["n"] += 1
            races["players"] += len(people)
            if winner:
                races["wins"][winner] = races["wins"].get(winner, 0) + 1
            if mode == "team":
                team = next((p.get("team") for p in people if p.get("xuid") == winner), None)
                teams["n"] += 1
                if team in (0, 1):
                    teams["blue" if team else "red"] += 1
        else:
            hits = {int(x): int(n) for x, n in d.get("hits", [])}
            if mode == "king":
                king["n"] += 1
                for x, n in hits.items():
                    king["steals"][x] = king["steals"].get(x, 0) + n
                if winner:
                    king["wins"][winner] = king["wins"].get(winner, 0) + 1
                for x, t in d.get("held", []):
                    if king["longest"] is None or t > king["longest"][1]:
                        king["longest"] = (int(x), t)
            else:
                inf["n"] += 1
                for x, n in hits.items():
                    inf["hits"][x] = inf["hits"].get(x, 0) + n
                for x in d.get("survivors", []):
                    inf["survived"][int(x)] = inf["survived"].get(int(x), 0) + 1
                if d.get("everyone"):
                    inf["everyone"] += 1
                    s = d.get("seconds")
                    if s and (inf["fastest"] is None or s < inf["fastest"]):
                        inf["fastest"] = s
    return {"races": {"count": races["n"], "avgPlayers": round(races["players"] / races["n"], 1) if races["n"] else None,
                      "winners": _ladder(lsp, races["wins"])},
            "teams": {"count": teams["n"], "red": teams["red"], "blue": teams["blue"]},
            "king": {"count": king["n"], "winners": _ladder(lsp, king["wins"]), "stealers": _ladder(lsp, king["steals"]),
                     "longestCrown": {"xuid": king["longest"][0], "gamertag": _who(lsp, king["longest"][0]), "seconds": king["longest"][1]}
                     if king["longest"] else None},
            "infection": {"count": inf["n"], "everyone": inf["everyone"], "fastest": inf["fastest"], "infectors": _ladder(lsp, inf["hits"]),
                          "survivors": _ladder(lsp, inf["survived"])},
            "rentals": [{"carId": c, "car": _car_name(lsp, c), "count": n} for c, n in sorted(rentals.items(), key=lambda kv: (-kv[1], kv[0]))[:5]]}


def totals(lsp):
    db = lsp.DB
    km_of = {t: r["km"] for d in ops_api.route_catalog(lsp) for r in d["routes"] for t in r["tracks"]}
    km = sum(km_of.get(r["track"], 0) * r["n"] for r in db.query(
        "SELECT track, COUNT(*) AS n FROM score_submissions WHERE sb = ? AND st = ? GROUP BY track", FESTIVAL))
    km += sum((r["length"] or 0) * (r["laps"] or 1) / 1000.0 * r["n"] for r in db.query(
        "SELECT e.length, e.laps, COUNT(*) AS n FROM score_submissions s JOIN rival_events e ON e.id = s.gsid WHERE s.sb = ? AND s.st = ? "
        "GROUP BY e.id, e.length, e.laps", MONTHLY))
    seconds = db.one("SELECT COALESCE(SUM(score), 0) FROM score_submissions WHERE st = 1 AND sb IN (1, 3)")[0]
    fast = db.one("SELECT s.xuid, s.score, s.car, s.gsid, COALESCE(p.gamertag, '') AS tag FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid "
                  "WHERE s.sb = 9 AND s.score <= ? ORDER BY s.score DESC, s.id LIMIT 1", (scoreboard.SPEED_CAP,))
    designs = {r["kind"]: r["n"] for r in db.query("SELECT kind, COUNT(*) AS n FROM ugc_files WHERE removed = 0 GROUP BY kind")}
    return {"runs": lsp.SB.count(), "km": round(km), "hours": round(seconds / 3600.0, 1),
            "drivers": db.one("SELECT COUNT(*) FROM players WHERE gamertag <> ''")[0],
            "racers": db.one("SELECT COUNT(DISTINCT xuid) FROM score_submissions")[0],
            "designs": {k: designs.get(k, 0) for k in GARAGE_KINDS},
            "downloads": int(db.one("SELECT COALESCE(SUM(downloads), 0) FROM ugc_files WHERE removed = 0")[0]),
            "topSpeed": {"xuid": fast["xuid"], "gamertag": _who(lsp, fast["xuid"], fast["tag"]), "score": fast["score"],
                         "car": _car_name(lsp, fast["car"]), "carId": fast["car"]} if fast else None,
            "records": len(record_history(lsp).records), "modes": modes(lsp)}


# ---------------------------------------------------------------- routing (from ops_api._route)

def route(lsp, what, rest, q, num):
    """-> (status, object or PNG bytes), or None when `what` is not one of these endpoints."""
    if what == "records" and not rest:
        return 200, records(lsp, num(q, "limit", 50, 0, 200))
    if what == "hall-of-fame" and not rest:
        return 200, hall_of_fame(lsp)
    if what == "traps" and not rest:
        return 200, traps(lsp, num(q, "limit", 10, 1, 50))
    if what == "cars":
        if not rest:
            return 200, cars(lsp, num(q, "days", 0, 0, 3650), (q.get("class") or [""])[0].upper(), num(q, "limit", 25, 1, 200))
        if len(rest) == 1:
            return 200, car(lsp, rest[0])
    if what == "garage":
        if not rest:
            return 200, garage(lsp, (q.get("kind") or ["livery"])[0], (q.get("sort") or ["likes"])[0], num(q, "limit", 24, 1, 100),
                               num(q, "offset", 0, 0, 10 ** 6))
        if len(rest) == 2 and rest[1] == "preview.png":
            return 200, garage_preview(lsp, rest[0])
    if what == "history" and not rest:
        return 200, history(lsp, (q.get("range") or ["24h"])[0])
    if what == "totals" and not rest:
        return 200, totals(lsp)
    return None
