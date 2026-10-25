"""The operator API the forza.scuffed.af website talks to (read the public stats, run the admin panel).

HTTP-free like launcher_api.py: handle() takes the request pieces and returns (status, JSON-able object), so it is tested without
sockets. The LSP maps its handler onto it (fh2mp_lsp.py, `_ops`) and hands over its own module as `lsp`, whose globals (AUTH, DB, SB,
RIV, NOTIF, ...) are the stores; nothing here keeps state except the saved operator settings (table `meta`, key `ops_config`).

Access: the API is off unless FH2MP_OPS_TOKEN is set. A request needs `Authorization: Bearer <token>`, must come straight from an
address in FH2MP_OPS_ALLOW (comma separated CIDRs, default loopback only; the website instance's private address goes here) and must
not carry proxy headers, so nothing reachable through the public reverse proxy can ever get in. Wrong tokens are rate limited per IP.

    GET    /ops/v1/lobbies                     occupied Free Roam / Road Trip worlds and their current stages
    GET    /ops/v1/reports[?limit=&offset=]    reports queue and permanently hidden submissions
    POST   /ops/v1/reports/<id>               {"browser": "<hash>", "ip": "<hash>}, duplicate protected
    POST   /ops/v1/reports/<id>/hide          permanently suppress a run; preserve its data and ghost
    POST   /ops/v1/reports/<id>/dismiss       archive reports, clearing the public warning
    GET    /ops/v1/overview                    counters for the dashboard and the public stats
    GET    /ops/v1/online                      players who polled the LSP in the last 40 s
    GET    /ops/v1/challenges[?top=3]          this rotation's Monthly Rivals events (+ best real times)
    GET    /ops/v1/leaderboards[?limit=25]     XP ladder and one board per event, real players only
    GET    /ops/v1/routes                      Festival Routes (regular Rivals) by destination, with how many players have a time per class
    GET    /ops/v1/routes/<id>[?limit=50]      one route: a board per car class (D .. S2), real players only
    GET    /ops/v1/recent[?limit=20]           the newest Rivals runs (Festival Routes and Monthly events)
    GET    /ops/v1/players[?q=&limit=&offset=] accounts (gamertag / xuid search), with online and ban state
    POST   /ops/v1/players/<xuid>/ban          {"reason": "..."}   |  /unban  |  /kick (drops the player's sessions)
    GET    /ops/v1/notification-types          \
    GET    /ops/v1/notifications                > the Message Center queue, same JSON as the loopback /api/notifications
    POST   /ops/v1/notifications               >   ({"kind"|"type", "text", "sra", "xuid", "fields": {...}})
    DELETE /ops/v1/notifications[/<id>]        /
    POST   /ops/v1/rivals/rotate               re-roll this month's events
    GET    /ops/v1/config                      launcher settings and registration mode
    PUT    /ops/v1/config                      {"motd", "maintenance", "minLauncher", "minShim", "links": {...}, "registration": open|invite|closed,
                                                "heroImage": "https://...", "news": [{"tag", "title", "text", "url", "image"}, ...]}
                                               blockedNames = gamertag blocklist (blocked anywhere in a name), blockedWords = whole words only
    POST   /ops/v1/invites                     {"code"?, "uses", "note"} -> {"code"}   (only a hash is stored, so codes cannot be listed)
    GET    /ops/v1/auth-events[?limit=]        recent sign-ups, logins, bans, rate-limit hits
    GET    /ops/v1/records, hall-of-fame, traps, cars[/<id>], garage[/<id>/preview.png], history, totals   website statistics (site_stats.py)

Errors: {"error": "<code>", "message": "..."}.
"""
import calendar
import hmac
import ipaddress
import json
import os
import re
import secrets
import time
from urllib.parse import parse_qs, urlparse

import auth
import rivals
import scoreboard
import score_reports
import site_stats
import social
import db

STARTED = time.time()
BLOCKLIST_MAX = 500
MONTHLY_BOARD = (3, 1)                  # (scoreboardType, scoreType) of a Monthly Rivals event; the pivot is the event id
FESTIVAL_BOARD = (1, 1)                 # (scoreboardType, scoreType) of a Festival Route; the track id is the route, the pivot the car class
DESTINATIONS = ("Castelletto", "Montellino", "Nice", "Saint-Martin", "San Giovanni", "Sisteron")      # the route names start with one
NOT_ROUTES = ("Showcase", "Road Trip", "Finale", "Arena")
LINK_KEYS = ("website", "patreon", "discord")
REGISTRATION_MODES = ("open", "invite", "closed")
INVITE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"        # no 0/O/1/I/L
NEWS_MAX = 12
NEWS_FIELDS = {"tag": 24, "title": 80, "text": 200, "url": 300, "image": 300}       # launcher home card fields and their length limits


class OpsError(Exception):
    def __init__(self, status, code, message=""):
        super().__init__(message or code)
        self.status, self.code, self.message = status, code, message or code


class OpsConfig:
    def __init__(self, token="", allow=("127.0.0.1/32", "::1/128")):
        self.token = token
        self.allow = [ipaddress.ip_network(a.strip(), strict=False) for a in allow if a.strip()]

    @classmethod
    def from_env(cls):
        raw = os.environ.get("FH2MP_OPS_ALLOW", "")
        return cls(os.environ.get("FH2MP_OPS_TOKEN", "").strip(), raw.split(",") if raw.strip() else ("127.0.0.1/32", "::1/128"))

    def permits(self, ip):
        try:
            addr = ipaddress.ip_address(ip)
        except ValueError:
            return False
        if addr.version == 6 and addr.ipv4_mapped:
            addr = addr.ipv4_mapped
        return any(addr in net for net in self.allow)


# ---------------------------------------------------------------- entry point

def handle(cfg, lsp, method, path, headers, body, peer, proxied):
    """-> (status, object). Never raises."""
    try:
        _authorize(cfg, lsp, headers, peer, proxied)
        return _route(lsp, method, path, body)
    except OpsError as e:
        return e.status, {"error": e.code, "message": e.message}
    except lsp.ApiError as e:                                  # the notification API's own errors
        return e.status, {"error": "bad_request", "message": e.msg}
    except Exception as e:  # noqa: BLE001 - a bug here must not kill the LSP's handler thread silently
        import traceback
        traceback.print_exc()
        return 500, {"error": "internal", "message": type(e).__name__}


def _authorize(cfg, lsp, headers, peer, proxied):
    if not cfg.token:
        raise OpsError(404, "disabled", "the operator API is not enabled on this server")
    if proxied or not cfg.permits(peer):
        raise OpsError(403, "forbidden")
    h = headers.get("Authorization", "")
    given = h[7:].strip() if h[:7].lower() == "bearer " else ""
    if not hmac.compare_digest(given.encode(), cfg.token.encode()):
        if lsp.AUTH is not None and not lsp.AUTH.hit(("ops-fail", peer), 10, 60):
            raise OpsError(429, "rate_limited", "too many wrong tokens")
        raise OpsError(401, "unauthorized")


def _json(body):
    try:
        d = json.loads(body or b"{}")
    except ValueError:
        raise OpsError(400, "bad_request", "invalid JSON")
    if not isinstance(d, dict):
        raise OpsError(400, "bad_request", "expected a JSON object")
    return d


def _int(q, key, default, lo, hi):
    try:
        return max(lo, min(hi, int((q.get(key) or [default])[0])))
    except ValueError:
        raise OpsError(400, "bad_request", f"{key} must be a number")


def _route(lsp, method, path, body):
    u = urlparse(path)
    q = parse_qs(u.query)
    parts = [p for p in u.path.split("/") if p]                    # ["ops", "v1", ...]
    if parts[:2] != ["ops", "v1"] or len(parts) < 3:
        raise OpsError(404, "not_found")
    what, rest = parts[2], parts[3:]

    if what == "reports":
        if not rest and method == "GET":
            return 200, score_reports.queue(lsp, _int(q, "limit", 50, 1, 100), _int(q, "offset", 0, 0, 10**9))
        if rest and rest[0].isdigit():
            sid = int(rest[0])
            if len(rest) == 1 and method == "POST":
                return 200, score_reports.report(lsp, sid, _json(body))
            if len(rest) == 2 and method == "POST":
                return 200, score_reports.moderate(lsp, sid, rest[1])
    if what == "lobbies" and not rest and method == "GET":
        result = lsp.xbl.hub_lobbies()
        if result is None:
            raise OpsError(503, "unavailable", "the game server did not answer")
        # Resolve private protocol IDs to nicknames here; public summaries contain no player IDs.
        for lobby in result['lobbies']:
            members = []
            for value in lobby.pop('memberXuids', []):
                xuid = int(value, 16)
                name = lsp.TAGS.get(xuid)
                if not name:
                    row = lsp.DB.one("SELECT gamertag FROM players WHERE xuid = ?", (xuid,))
                    name = row['gamertag'] if row else ''
                members.append(name or 'Connecting player')
            lobby['members'] = sorted(members, key=str.casefold)
        return 200, result
    if what == "overview" and method == "GET":
        return 200, overview(lsp)
    if what == "online" and method == "GET":
        return 200, {"players": online(lsp)}
    if what == "challenges" and method == "GET":
        return 200, challenges(lsp, _int(q, "top", 3, 0, 25))
    if what == "leaderboards" and method == "GET":
        return 200, leaderboards(lsp, _int(q, "limit", 25, 1, 100))
    if what == "players":
        if not rest and method == "GET":
            return 200, players(lsp, (q.get("q") or [""])[0], _int(q, "limit", 50, 1, 200), _int(q, "offset", 0, 0, 10 ** 9))
        if len(rest) == 2 and method == "POST":
            return 200, player_action(lsp, rest[0], rest[1], _json(body))
    if what in ("notifications", "notification-types"):
        return _notifications(lsp, method, what, rest, body)
    if what == "rivals" and rest == ["rotate"] and method == "POST":
        events = lsp.RIV.rotate()
        lsp.announce_rotation()
        lsp.log(f"  ops: rivals re-rolled -> {len(events)} events")
        return 200, challenges(lsp, 0)
    if what == "routes" and method == "GET":
        if not rest:
            return 200, routes(lsp)
        if len(rest) == 1:
            return 200, route_boards(lsp, rest[0], _int(q, "limit", 50, 1, 100))
    if what == "recent" and method == "GET":
        return 200, recent(lsp, _int(q, "limit", 20, 1, 100))
    if what == "config":
        if method == "GET":
            return 200, get_config(lsp)
        if method == "PUT":
            return 200, set_config(lsp, _json(body))
    if what == "invites" and not rest and method == "POST":
        return 201, make_invite(lsp, _json(body))
    if what == "auth-events" and method == "GET":
        return 200, auth_events(lsp, _int(q, "limit", 50, 1, 500))
    if method == "GET":
        found = site_stats.route(lsp, what, rest, q, _int)
        if found is not None:
            return found
    raise OpsError(404, "not_found", "unknown endpoint")


# ---------------------------------------------------------------- reads

def overview(lsp):
    st = lsp.AUTH.stats()
    month, reroll = lsp.RIV.serial()
    return {
        "time": int(time.time()), "uptime": int(time.time() - STARTED), "env": lsp.AUTH.env,
        "auth": lsp.AUTH.mode, "registration": lsp.AUTH.registration, "maintenance": lsp.LAUNCHER.config.get()["maintenance"],
        "online": lsp.online_count(), "accounts": st["accounts"], "sessions": st["sessions"], "registered24h": st["registered_24h"],
        "players": lsp.DB.one("SELECT COUNT(*) FROM players WHERE gamertag <> ''")[0],
        "scores": lsp.SB.count(), "notifications": lsp.NOTIF.count(), "userContent": lsp.UGC.count(),
        "clubs": lsp.DB.one("SELECT COUNT(*) FROM clubs")[0], "lobbies": lsp.xbl.count(),
        "rotation": {"month": month, "reroll": reroll, "events": len(lsp.RIV.events())},
    }


def online(lsp):
    now = time.time()
    out = [{"xuid": x, "gamertag": lsp.TAGS.get(x, ""), "lastSeen": t} for x, t in list(social.LAST_POLL.items())
           if now - t < social.ONLINE_SECONDS]
    return sorted(out, key=lambda p: p["gamertag"].lower())


def _gamertag(lsp, r):
    return r["tag"] or lsp.TAGS.get(r["xuid"], "") or f"Player {r['xuid'] & 0xFFFF:04X}"


def _board_row(lsp, r, car_names):
    cls = r["cls"]
    return {**{k: r[k] for k in ("submissionId", "reports", "suspicious") if k in r}, "position": r["position"], "xuid": r["xuid"], "gamertag": _gamertag(lsp, r), "score": r["score"],
            "car": car_names.get(r["car"], ""), "carId": r["car"], "class": rivals.CLASS_LETTERS[cls] if 0 <= cls < len(rivals.CLASS_LETTERS) else "",
            "clean": r["clean"], "at": int(r["t"])}


def _event(lsp, e, rows):
    return {"id": e["id"], "name": e["name"], "track": e["trackname"], "laps": e["laps"], "km": round(e["length"] / 1000.0, 2),
            "kind": e["kind"], "description": e["description"],
            "class": rivals.CLASS_LETTERS[e["cls"]] if e["cls"] is not None and 0 <= e["cls"] < len(rivals.CLASS_LETTERS) else "",
            "rows": rows}


def _year_month(month):
    """Rotation month index (months since rivals.EPOCH) -> (year, month 1-12)."""
    return rivals.EPOCH[0] + (rivals.EPOCH[1] - 1 + month) // 12, (rivals.EPOCH[1] - 1 + month) % 12 + 1


def month_label(month):
    year, mo = _year_month(month)
    return f"{calendar.month_name[mo]} {year}"


def _rotation(lsp):
    """(label such as 'October 2026', unix time the monthly rotation ends)."""
    month, reroll = lsp.RIV.serial()
    year, mo = _year_month(month)
    end = calendar.timegm((year + (mo == 12), mo % 12 + 1, 1, 0, 0, 0))
    return {"label": month_label(month), "month": month, "reroll": reroll, "endsAt": end}


def challenges(lsp, top):
    names = lsp.RIV.names()["car"]
    events = [_event(lsp, e, [_board_row(lsp, r, names) for r in lsp.SB.top(*MONTHLY_BOARD, -1, e["id"], top)] if top else [])
              for e in lsp.RIV.events()]
    return {"rotation": _rotation(lsp), "events": events}


def leaderboards(lsp, limit):
    names = lsp.RIV.names()["car"]
    xp = [{"position": r["position"], "xuid": r["xuid"], "gamertag": _gamertag(lsp, r), "xp": int(r["score"])}
          for r in lsp.SB.top(*scoreboard.XP_BOARD, -1, -1, limit)]
    events = [_event(lsp, e, [_board_row(lsp, r, names) for r in lsp.SB.top(*MONTHLY_BOARD, -1, e["id"], limit)])
              for e in lsp.RIV.events()]
    return {"rotation": _rotation(lsp), "xp": xp, "events": events}


def _route_kind(name, closed):
    low = name.lower()
    for word, kind in (("cross country", "Cross Country"), ("street race", "Street Race"), ("scramble", "Scramble")):
        if word in low:
            return kind
    return "Circuit" if closed else "Sprint"


def route_catalog(lsp):
    """Festival Routes grouped by destination, from the game catalog: [{"name", "routes": [{"id", "name", "kind", "km", "circuit",
    "tracks": [track ids]}]}]. The catalog has several track ids for one route (same name); they are one route here, and its id is the
    lowest. Circuits come first, then by length. Cached: the catalog never changes while the server runs."""
    cached = getattr(lsp, "_route_catalog", None)
    if cached is not None:
        return cached
    rows = lsp.CARS.db.query("SELECT t.id, v.name, v.length_m, v.closed FROM Tracks t JOIN v_tracks v ON v.id = t.id "
                             "WHERE t.MediaName = 'Anthem' AND t.Active = 1 AND v.length_m IS NOT NULL AND v.name IS NOT NULL ORDER BY t.id")
    by_name = {}
    for r in rows:
        name = r["name"].strip()
        dest = next((d for d in DESTINATIONS if name.startswith(d + " ")), None)
        if dest is None or any(w in name for w in NOT_ROUTES):
            continue
        route = by_name.setdefault(name, {"id": r["id"], "name": name[len(dest) + 1:], "kind": _route_kind(name, r["closed"]),
                                          "km": round(r["length_m"] / 1000.0, 2), "circuit": bool(r["closed"]), "dest": dest, "tracks": []})
        route["tracks"].append(r["id"])
    out = []
    for d in DESTINATIONS:
        mine = sorted((r for r in by_name.values() if r["dest"] == d), key=lambda r: (not r["circuit"], r["km"], r["name"]))
        if mine:
            out.append({"name": d, "routes": [{k: v for k, v in r.items() if k != "dest"} for r in mine]})
    lsp._route_catalog = out
    return out


def routes(lsp):
    entries = {}
    for r in lsp.DB.query("SELECT track, cls, COUNT(DISTINCT xuid) AS n FROM score_submissions WHERE sb = ? AND st = ? GROUP BY track, cls",
                          FESTIVAL_BOARD):
        entries.setdefault(r["track"], {})[r["cls"]] = r["n"]
    dests = []
    for d in route_catalog(lsp):
        rs = []
        for r in d["routes"]:
            per = {}
            for t in r["tracks"]:
                for cls, n in entries.get(t, {}).items():
                    per[cls] = per.get(cls, 0) + n          # a player on two track ids of one route is counted twice: only a hint for the tabs
            rs.append({**r, "entries": {rivals.CLASS_LETTERS[c]: n for c, n in sorted(per.items()) if 0 <= c < len(rivals.CLASS_LETTERS)},
                       "total": sum(per.values())})
        dests.append({"name": d["name"], "routes": rs})
    return {"classes": list(rivals.CLASS_LETTERS), "destinations": dests}


def route_boards(lsp, route_id, limit):
    try:
        route_id = int(route_id)
    except ValueError:
        raise OpsError(404, "not_found", "unknown route")
    found = next(((d["name"], r) for d in route_catalog(lsp) for r in d["routes"] if r["id"] == route_id), None)
    if found is None:
        raise OpsError(404, "not_found", "unknown route")
    dest, route = found
    names = lsp.RIV.names()["car"]
    classes = []
    for cls, letter in enumerate(rivals.CLASS_LETTERS):
        best = {}
        for t in route["tracks"]:                       # best time per player over all of the route's track ids
            for r in lsp.SB.top(*FESTIVAL_BOARD, t, cls, limit):
                if r["xuid"] not in best or r["score"] < best[r["xuid"]]["score"]:
                    best[r["xuid"]] = r
        ranked = sorted(best.values(), key=lambda r: (r["score"], r["t"]))[:limit]
        for i, r in enumerate(ranked):
            r["position"] = i + 1
        classes.append({"class": letter, "rows": [_board_row(lsp, r, names) for r in ranked]})
    return {"destination": dest, "route": {k: route[k] for k in ("id", "name", "kind", "km", "circuit")}, "classes": classes}


def recent(lsp, limit):
    """The newest Festival Route and Monthly Rivals runs by anyone, newest first (every submission, not only personal bests)."""
    track_names = {t: f"{d['name']} {r['name']}" for d in route_catalog(lsp) for r in d["routes"] for t in r["tracks"]}
    events = {e["id"]: e["name"] for e in lsp.RIV.events()}
    names = lsp.RIV.names()["car"]
    rows = lsp.DB.query(f"SELECT s.id, {score_reports.COUNTS} AS report_count, s.xuid, s.sb, s.gsid, s.track, s.car, s.cls, s.score, s.clean, s.submitted, COALESCE(p.gamertag, '') AS tag "
                        f"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.st = ? AND s.sb IN (?, ?) AND {score_reports.VISIBLE} "
                        "ORDER BY s.submitted DESC, s.id DESC LIMIT ?", (FESTIVAL_BOARD[1], FESTIVAL_BOARD[0], MONTHLY_BOARD[0], limit * 3))
    out = []
    for r in rows:
        if r["sb"] == MONTHLY_BOARD[0]:
            where, kind = events.get(r["gsid"]), "event"
        else:
            where, kind = track_names.get(r["track"]), "route"
            if where is None:
                continue                                  # a track that is not a Festival Route (DLC id we do not know)
        if where is None:
            continue                                      # a Monthly event of an earlier rotation: nothing to name it by
        cls = r["cls"]
        out.append({**score_reports.fields(r), "gamertag": _gamertag(lsp, r), "xuid": r["xuid"], "kind": kind, "where": where, "score": r["score"],
                    "car": names.get(r["car"], ""), "carId": r["car"],
                    "class": rivals.CLASS_LETTERS[cls] if 0 <= cls < len(rivals.CLASS_LETTERS) else "",
                    "clean": bool(r["clean"]), "at": int(r["submitted"])})
        if len(out) >= limit:
            break
    return {"runs": out}


def players(lsp, text, limit, offset):
    where, args = "", []
    text = text.strip()
    if text:
        like = "%" + re.sub(r"([\\%_])", r"\\\1", text.lower()) + "%"
        where, args = "WHERE lower(a.gamertag) LIKE ?", [like]
        try:
            xuid = int(text, 0)
        except ValueError:
            xuid = None
        if xuid is not None:
            where += " OR a.xuid = ?"
            args.append(xuid)
    total = lsp.DB.one(f"SELECT COUNT(*) FROM accounts a {where}", args)[0]
    rows = lsp.DB.query(
        "SELECT a.xuid, a.gamertag, a.created, a.last_login, a.banned_at, a.ban_reason, COALESCE(p.career_level, 0) AS level, "
        "COALESCE(p.last_seen, 0) AS last_seen, COALESCE(x.xp, 0) AS xp FROM accounts a LEFT JOIN players p ON p.xuid = a.xuid "
        f"LEFT JOIN player_xp x ON x.xuid = a.xuid {where} ORDER BY GREATEST(a.last_login, COALESCE(p.last_seen, 0)) DESC, a.xuid "
        "LIMIT ? OFFSET ?", args + [limit, offset])
    return {"total": total, "players": [
        {"xuid": r["xuid"], "gamertag": r["gamertag"], "created": int(r["created"]), "lastLogin": int(r["last_login"]),
         "lastSeen": int(r["last_seen"]), "level": db.display_level(r["level"]), "xp": int(r["xp"]), "online": social.online(r["xuid"]),
         "banned": r["banned_at"] is not None, "banReason": r["ban_reason"]} for r in rows]}


def auth_events(lsp, limit):
    rows = lsp.DB.query("SELECT at, kind, ip, xuid, detail FROM auth_events ORDER BY id DESC LIMIT ?", (limit,))
    return {"events": [{"at": int(r["at"]), "kind": r["kind"], "ip": r["ip"], "xuid": r["xuid"], "detail": r["detail"]} for r in rows]}


# ---------------------------------------------------------------- actions

def _xuid(text):
    try:
        return int(text, 0)
    except ValueError:
        raise OpsError(400, "bad_request", "xuid must be a number")


def player_action(lsp, xuid_text, action, d):
    xuid = _xuid(xuid_text)
    if not lsp.DB.one("SELECT 1 FROM accounts WHERE xuid=?", (xuid,)):
        raise OpsError(404, "no_such_player", "no account with that xuid")
    if action == "ban":
        reason = str(d.get("reason", "")).strip()[:200] or "banned by an operator"
        lsp.AUTH.set_banned(xuid, reason)
        lsp.log(f"  ops: banned {xuid:#x}: {reason}")
        return {"banned": True}
    if action == "unban":
        lsp.AUTH.set_banned(xuid, None)
        lsp.log(f"  ops: unbanned {xuid:#x}")
        return {"banned": False}
    if action == "kick":
        n = lsp.AUTH.revoke(xuid)
        lsp.log(f"  ops: dropped {n} session(s) of {xuid:#x}")
        return {"sessionsDropped": n}
    raise OpsError(404, "not_found", "unknown player action")


def _notifications(lsp, method, what, rest, body):
    if what == "notification-types":
        if method != "GET" or rest:
            raise OpsError(404, "not_found")
        path = "/api/types"
    else:
        path = "/api/notifications" + "".join("/" + p for p in rest)
    status, obj = lsp.api(method, path, body)
    if method in ("POST", "DELETE"):
        lsp.log(f"  ops: {method} {path} -> {status}")
    return status, obj


def get_config(lsp):
    c = lsp.LAUNCHER.config.get()
    return {"env": lsp.AUTH.env, "auth": lsp.AUTH.mode, "registration": lsp.AUTH.registration,
            "motd": c["motd"], "maintenance": c["maintenance"], "minLauncher": c["minLauncher"], "minShim": c["minShim"], "links": c["links"],
            "heroImage": c.get("heroImage", ""), "news": c.get("news", []),
            "blockedNames": list(saved_blocklist(lsp, "blockedNames")), "blockedWords": list(saved_blocklist(lsp, "blockedWords"))}


def saved_blocklist(lsp, key):
    row = lsp.DB.one("SELECT value FROM meta WHERE key='ops_config'")
    return (json.loads(row["value"]) if row else {}).get(key, [])


def _words(d, key):
    v = d[key]
    if not isinstance(v, list) or len(v) > BLOCKLIST_MAX or not all(isinstance(x, str) and 2 <= len(x.strip()) <= 40 for x in v):
        raise OpsError(400, "bad_request", f"{key} must be a list of at most {BLOCKLIST_MAX} words of 2 to 40 characters")
    return sorted({x.strip().lower() for x in v})


def _text(d, key, limit):
    v = d[key]
    if not isinstance(v, str) or len(v) > limit or any(ord(ch) < 32 and ch != "\n" for ch in v):
        raise OpsError(400, "bad_request", f"{key} must be text of at most {limit} characters")
    return v.strip()


def _url(d, key, limit, https_only=False):
    v = d[key]
    pat = r"https://[^\s]+" if https_only else r"https?://[^\s]+"
    if not isinstance(v, str) or len(v) > limit or (v and not re.fullmatch(pat, v)):
        raise OpsError(400, "bad_request", f"{key} must be empty or an {'https' if https_only else 'http(s)'} URL")
    return v


def validate_config(d):
    """-> the settings to keep (unknown keys are errors, so a typo cannot silently do nothing)."""
    out = {}
    unknown = set(d) - {"motd", "maintenance", "minLauncher", "minShim", "links", "registration", "heroImage", "news", "blockedNames", "blockedWords"}
    if unknown:
        raise OpsError(400, "bad_request", f"unknown setting(s): {', '.join(sorted(unknown))}")
    for key, limit in (("motd", 500), ("maintenance", 300)):
        if key in d:
            out[key] = _text(d, key, limit)
    for key in ("minLauncher", "minShim"):
        if key in d:
            if not isinstance(d[key], str) or not re.fullmatch(r"\d{1,4}\.\d{1,4}\.\d{1,4}(-rc\d{1,3})?", d[key]):
                raise OpsError(400, "bad_request", f"{key} must look like 1.2.3 or 1.2.3-rc4")
            out[key] = d[key]
    if "links" in d:
        if not isinstance(d["links"], dict) or set(d["links"]) - set(LINK_KEYS):
            raise OpsError(400, "bad_request", f"links may only have: {', '.join(LINK_KEYS)}")
        out["links"] = {}
        for k, v in d["links"].items():
            if not isinstance(v, str) or len(v) > 200 or (v and not re.fullmatch(r"https?://[^\s]+", v)):
                raise OpsError(400, "bad_request", f"links.{k} must be empty or an http(s) URL")
            out["links"][k] = v
    if "heroImage" in d:
        out["heroImage"] = _url(d, "heroImage", 300, https_only=True)
    if "news" in d:
        if not isinstance(d["news"], list) or len(d["news"]) > NEWS_MAX:
            raise OpsError(400, "bad_request", f"news must be a list of at most {NEWS_MAX} cards")
        out["news"] = []
        for i, card in enumerate(d["news"]):
            if not isinstance(card, dict) or set(card) - set(NEWS_FIELDS) or not card.get("title"):
                raise OpsError(400, "bad_request", f"news[{i}] needs a title and may only have: {', '.join(NEWS_FIELDS)}")
            card = {k: card.get(k, "") for k in NEWS_FIELDS}
            out["news"].append({k: _url(card, k, NEWS_FIELDS[k], https_only=k == "image") if k in ("url", "image") else _text(card, k, NEWS_FIELDS[k])
                                for k in NEWS_FIELDS})
    for key in ("blockedNames", "blockedWords"):
        if key in d:
            out[key] = _words(d, key)
    if "registration" in d:
        if d["registration"] not in REGISTRATION_MODES:
            raise OpsError(400, "bad_request", f"registration must be one of {', '.join(REGISTRATION_MODES)}")
        out["registration"] = d["registration"]
    return out


def apply_config(lsp, saved):
    overlay = {k: v for k, v in saved.items() if k not in ("registration", "blockedNames", "blockedWords")}
    lsp.LAUNCHER.config.overlay = overlay
    if "registration" in saved:
        lsp.AUTH.registration = saved["registration"]
    lsp.AUTH.blocked = lsp.AUTH.blocked_file + tuple(auth.fold_tag(x) for x in saved.get("blockedNames", ()))
    lsp.AUTH.blocked_words = tuple(auth.fold_tag(x) for x in saved.get("blockedWords", ()))


def set_config(lsp, d):
    new = validate_config(d)
    row = lsp.DB.one("SELECT value FROM meta WHERE key='ops_config'")
    saved = json.loads(row["value"]) if row else {}
    if "links" in new:
        new["links"] = {**saved.get("links", {}), **new["links"]}
    saved.update(new)
    with lsp.DB.tx() as c:
        c.execute("INSERT INTO meta (key, value) VALUES ('ops_config', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                  (json.dumps(saved),))
    apply_config(lsp, saved)
    lsp.log(f"  ops: settings changed: {', '.join(sorted(new))}")
    return get_config(lsp)


def load_saved(lsp):
    """At LSP start: re-apply what the operator last saved (the environment's registration mode is only the default)."""
    row = lsp.DB.one("SELECT value FROM meta WHERE key='ops_config'")
    if row:
        apply_config(lsp, json.loads(row["value"]))


def make_invite(lsp, d):
    uses = d.get("uses", 1)
    if isinstance(uses, bool) or not isinstance(uses, int) or not 1 <= uses <= 1000:
        raise OpsError(400, "bad_request", "uses must be a whole number from 1 to 1000")
    note = str(d.get("note", ""))[:100]
    code = str(d.get("code", "")).strip().upper()
    if code:
        if not re.fullmatch(r"[A-Z0-9-]{4,40}", code):
            raise OpsError(400, "bad_request", "a code is 4-40 letters, digits or dashes")
    else:
        code = "FH2-" + "".join(secrets.choice(INVITE_ALPHABET) for _ in range(4)) + "-" + "".join(secrets.choice(INVITE_ALPHABET) for _ in range(4))
    lsp.AUTH.add_invite(code, uses, note)
    lsp.log(f"  ops: invite created ({uses} use(s)) {note!r}")
    return {"code": code, "uses": uses, "note": note}
