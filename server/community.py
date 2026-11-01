"""Authenticated launcher friends, presence, profiles, conversations and lobby leases."""
import time
from urllib.parse import parse_qs

import auth
import clubs
import db
import launcher_lobbies
import social
import xbl

LAUNCHER_SEEN = {}                  # independent of the game's presence poll
MESSAGE_KEEP = 7 * 86400            # chat messages older than a week are deleted
HUB_CACHE_SECONDS = 2.5
_PRUNE = {"at": 0.0}
INVITE_ERRORS = {                   # ClubStore rule violations, in the launcher's words
    "not_in_club": "You are not in a club.", "no_privilege": "Only the club leader and co-leaders can invite players.",
    "no_invites": "That player does not accept club invites.", "in_club": "That player is already in a club.",
    "already_invited": "That player is already invited.", "club_full": "The club is full.",
}


class Community:
    def __init__(self, ctx):
        self.ctx, self.db, self.social = ctx, ctx.auth.db, ctx.social

    def resolve(self, value):
        value = str(value).strip()
        try:
            xuid = int(value, 16 if value.lower().startswith("0x") else 10)
        except ValueError:
            xuid = None
        if xuid is not None and 0 < xuid < 2**63:
            row = self.db.one("SELECT xuid FROM accounts WHERE xuid=? AND (banned_at IS NULL OR ban_until <= ?)", (xuid, time.time()))
        else:
            row = self.db.one("SELECT xuid FROM accounts WHERE gamertag_key=? AND (banned_at IS NULL OR ban_until <= ?)", (auth.fold_tag(value), time.time()))
        if not row:
            raise auth.AuthError(404, "player_not_found", "No player has that gamertag or ID.")
        return row[0]

    def profile(self, xuid, lobbies=()):
        return self.profiles([xuid], lobbies)[xuid]

    def profiles(self, xuids, lobbies=()):
        """The cards of many players in three queries; the friends list asked nine per friend, every 5 s per open launcher."""
        xuids = list(dict.fromkeys(xuids))
        if not xuids:
            return {}
        marks = ",".join("?" * len(xuids))
        rows = {r[0]: r for r in self.db.query("SELECT a.xuid, a.gamertag, p.career_level, COALESCE(g.gamerpic_at,0) AS gamerpic_version "
                                               "FROM accounts a LEFT JOIN players p ON p.xuid=a.xuid LEFT JOIN player_profiles g ON g.xuid=a.xuid "
                                               f"WHERE a.xuid IN ({marks})", xuids)}
        clubs = {r["xuid"]: r for r in self.db.query("SELECT m.xuid, c.id, c.tag, c.name, m.role, (SELECT COUNT(*) FROM club_members n WHERE n.club_id=c.id) AS members "
                                                     f"FROM club_members m JOIN clubs c ON c.id=m.club_id WHERE m.xuid IN ({marks})", xuids)}
        return {x: self._card(x, rows.get(x), clubs.get(x), lobbies) for x in xuids}

    def _card(self, xuid, row, club, lobbies):
        if not row:
            # Dev/optional-auth worlds may contain a legitimate game player
            # without a launcher account. They are visible but cannot be added
            # until they register (resolve() still requires an account).
            row = {"gamertag": self.social.profile(xuid)["Gamertag"], "career_level": 0, "gamerpic_version":0}
        lobby = next((l for l in lobbies if f"{xuid:x}" in l.get("memberXuids", [])), None)
        playing = lobby is not None or social.online(xuid)
        return {"xuid": xuid, "gamertag": row["gamertag"], "level": db.display_level(row["career_level"] or 0),
                "gamerpicVersion": row['gamerpic_version'],
                "state": "playing" if playing else "online" if time.time() - LAUNCHER_SEEN.get(xuid, 0) < 40 else "offline",
                "club": {k: club[k] for k in ("id", "tag", "name", "role", "members")} if club else None,
                "lobby": {k: v for k, v in lobby.items() if k != "memberXuids"} if lobby else None}

    def snapshot(self, me):
        result = self.hub_lobbies()
        lobbies = (result or {}).get("lobbies", [])
        people = self.social.friends(me)
        requests = self.db.query("SELECT sender, recipient FROM friend_requests WHERE sender=? OR recipient=? ORDER BY created", (me, me))
        cards = self.profiles([me, *people, *(r[0] for r in requests), *(r[1] for r in requests)], lobbies)
        return {"friends": [cards[f] for f in people], "incoming": [cards[r[0]] for r in requests if r[1] == me],
                "outgoing": [cards[r[1]] for r in requests if r[0] == me], "me": cards[me],
                "reservation": public_hold(launcher_lobbies.current(me)), "chats": self.channels(me),
                "lobbyCounts": {kind: sum(l['kind'] == kind for l in lobbies) for kind in ('freeroam', 'roadtrip')} if result is not None else None}

    def hub_lobbies(self):
        """The hub's answer, shared for a moment: a launcher open means one UDP query every 5 s, up to 0.3 s each."""
        now = time.monotonic()
        cached = self.__dict__.get("_hub")
        if cached and now - cached[0] < HUB_CACHE_SECONDS:
            return cached[1]
        result = xbl.hub_lobbies()
        self._hub = (now, result)
        return result

    def request(self, me, value):
        target = self.resolve(value)
        if target == me:
            raise auth.AuthError(400, "self_request", "That is your own account.")
        with self.db.tx() as c:
            if c.execute("SELECT 1 FROM friendships WHERE a=? AND b=?", (min(me, target), max(me, target))).fetchone():
                raise auth.AuthError(409, "already_friends", "You are already friends.")
            if c.execute("SELECT COUNT(*) FROM friend_requests WHERE sender=?", (me,)).fetchone()[0] >= 100:
                raise auth.AuthError(409, "request_limit", "Cancel an outgoing request before sending another.")
            c.execute("INSERT INTO friend_requests(sender,recipient,created) VALUES (?,?,?) ON CONFLICT DO NOTHING", (me, target, time.time()))
        return {"ok": True}

    def respond(self, me, target, accept=False):
        with self.db.tx() as c:
            if accept:
                if not c.execute("SELECT 1 FROM friend_requests WHERE sender=? AND recipient=?", (target, me)).fetchone():
                    raise auth.AuthError(404, "request_gone", "This friend request is no longer available.")
                c.execute("INSERT INTO friendships(a,b,created) VALUES (?,?,?) ON CONFLICT DO NOTHING", (min(me, target), max(me, target), time.time()))
            c.execute("DELETE FROM friend_requests WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?)", (target, me, me, target))
        return {"ok": True}

    def remove(self, me, target):
        with self.db.tx() as c:
            c.execute("DELETE FROM friendships WHERE a=? AND b=?", (min(me, target), max(me, target)))
            c.execute("DELETE FROM friend_requests WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?)", (target, me, me, target))
        return {"ok": True}

    def channel(self, me, value, c=None):
        query = (lambda sql, args: c.execute(sql, args).fetchone()) if c else self.db.one
        if value == "club":
            row = query("SELECT club_id FROM club_members WHERE xuid=?", (me,))
            if row:
                return f"club:{row[0]}"
        elif value.isdigit() and 0 < int(value) < 2**63:
            other = int(value)
            if query("SELECT 1 FROM friendships WHERE a=? AND b=?", (min(me, other), max(me, other))):
                return f"dm:{min(me, other)}:{max(me, other)}"
        raise auth.AuthError(403, "chat_unavailable", "Direct messages require friendship; club chat requires current membership.")

    def channels(self, me):
        friends = self.social.friends(me)
        cards = self.profiles(friends)
        refs = [(str(f), cards[f]["gamertag"]) for f in friends]
        club = self.db.one("SELECT c.tag, c.name FROM clubs c JOIN club_members m ON m.club_id=c.id WHERE m.xuid=?", (me,))
        if club:
            refs.insert(0, ("club", f"[{club['tag']}] {club['name']}"))
        if not refs:
            return []
        named = {ref: self.channel(me, ref) for ref, _ in refs}
        channels = list(named.values())
        marks = ",".join("?" * len(channels))
        last_ids = dict(tuple(r) for r in self.db.query(f"SELECT channel, MAX(id) FROM launcher_messages WHERE channel IN ({marks}) AND created>? GROUP BY channel",
                                                        (*channels, time.time() - MESSAGE_KEEP)))
        bodies = {}
        if last_ids:
            ids = list(last_ids.values())
            bodies = dict(tuple(r) for r in self.db.query(f"SELECT id, body FROM launcher_messages WHERE id IN ({','.join('?' * len(ids))})", ids))
        read = dict(tuple(r) for r in self.db.query(f"SELECT channel, last_id FROM launcher_chat_reads WHERE xuid=? AND channel IN ({marks})", (me, *channels)))
        incoming, unread = {}, {}
        for channel, message in self.db.query(f"SELECT channel, id FROM launcher_messages WHERE channel IN ({marks}) AND sender<>?", (*channels, me)):
            incoming[channel] = max(incoming.get(channel, 0), message)
            if message > read.get(channel, 0):
                unread[channel] = unread.get(channel, 0) + 1
        return [{"id": ref, "title": title, "last": bodies.get(last_ids.get(named[ref]), ""), "unread": unread.get(named[ref], 0),
                 "lastIncomingId": incoming.get(named[ref], 0)} for ref, title in refs]

    def messages(self, me, ref, after=0):
        channel = self.channel(me, ref)
        # On first open show the most recent page; then incrementally fetch forwards.
        rows = self.db.query("SELECT m.id,m.sender,a.gamertag,m.body,m.created FROM launcher_messages m JOIN accounts a ON a.xuid=m.sender "
                             "WHERE m.channel=? AND m.id>? AND m.created>? ORDER BY m.id " + ("ASC" if after else "DESC") + " LIMIT 100",
                             (channel, after, time.time() - MESSAGE_KEEP))
        return {"messages": sorted([dict(r) for r in rows], key=lambda r: r["id"])}

    def send(self, me, ref, body):
        if not isinstance(body, str) or not body.strip() or len(body.strip()) > 2000 or any(ord(ch) < 32 and ch not in '\n\t' for ch in body):
            raise auth.AuthError(400, "bad_message", "Messages must contain 1–2000 characters.")
        with self.db.tx() as c:
            channel = self.channel(me, ref, c)
            c.execute("INSERT INTO launcher_messages(sender,channel,body,created) VALUES (?,?,?,?)", (me, channel, body.strip(), time.time()))
        return {"ok": True}

    def read(self, me, ref, last):
        with self.db.tx() as c:
            channel = self.channel(me, ref, c)
            row = c.execute("SELECT MAX(id) FROM launcher_messages WHERE channel=?", (channel,)).fetchone()
            last = max(0, min(last, row[0] or 0))
            c.execute("INSERT INTO launcher_chat_reads(xuid,channel,last_id) VALUES (?,?,?) ON CONFLICT (xuid,channel) "
                      "DO UPDATE SET last_id=GREATEST(launcher_chat_reads.last_id,excluded.last_id)", (me, channel, last))
        return {"ok": True}

    def prune_messages(self, now=None):
        """Deletes chat messages past MESSAGE_KEEP (at most once an hour per process)."""
        now = time.time() if now is None else now
        if now - _PRUNE["at"] < 3600:
            return 0
        _PRUNE["at"] = now
        with self.db.tx() as c:
            return c.execute("DELETE FROM launcher_messages WHERE created<?", (now - MESSAGE_KEEP,)).rowcount

    def club_suggestions(self, me, limit=5):
        """The friends added most recently who are not in a club yet: the shortcut list of the club invite screen."""
        rows = self.db.query("SELECT CASE WHEN a=? THEN b ELSE a END AS xuid FROM friendships WHERE a=? OR b=? ORDER BY created DESC LIMIT 50", (me, me, me))
        out = []
        for r in rows:
            person = self.profile(r[0])
            if person["club"] is None:
                out.append(person)
            if len(out) >= limit:
                break
        return {"players": out}

    def club_invite(self, me, value):
        target = self.resolve(value)
        if target == me:
            raise auth.AuthError(400, "self_invite", "That is your own account.")
        invite = getattr(self.ctx, "club_invite", None)
        if invite is None:
            raise auth.AuthError(503, "unavailable", "Club invites are not available on this server.")
        try:
            invite(me, target)
        except clubs.ClubError as e:
            code = next((k for k, v in clubs.CODES.items() if v == e.code), "unknown")
            raise auth.AuthError(409, code, INVITE_ERRORS.get(code, e.text)) from None
        return {"ok": True, "gamertag": self.profile(target)["gamertag"]}

    def lobbies(self, me):
        result = xbl.hub_lobbies()
        if result is None:
            raise auth.AuthError(503, "unavailable", "The game server did not answer. Try again shortly.")
        pops = xbl.hub_populations()
        if pops is None:
            raise auth.AuthError(503, "unavailable")
        flags = xbl.flagged_set(pops)
        out = []
        with xbl._LOBBY_LOCK:
            launcher_lobbies.prune()
            sessions = {key[2]: rec for key, rec in list(xbl._SESSIONS.items()) if key[1] == xbl.TARGET_TEMPLATE.lower()}
            for row in result["lobbies"]:
                rec = sessions.get(row['id'].lower())
                occupied = xbl.lobby_occupancy(row['id'], rec, pops) if rec is not None else None
                if not xbl.pool_fits(row['id'], pops, flags, me in flags):
                    continue
                row = dict(row)
                ids = row.pop("memberXuids", [])
                row["_ids"] = ids
                present = {str(int(x, 16)) for x in pops.get(row['id'], []) if x}
                row["held"] = len(launcher_lobbies.seats(row["id"]) - present)
                row["occupied"] = occupied if occupied is not None else row['players'] + row['held']
                out.append(row)
            h = launcher_lobbies.HOLDS.get(str(me))
            if h and h['key'] is None:
                out.append({"id": h['id'], "kind": h['kind'], "players": 0, "held": 1, "occupied": 1,
                            "capacity": xbl.LOBBY_CAPACITY, "status": "Waiting for your matchmaking request", "members": []})
        # Database lookups do not hold the matchmaking mutex.
        for row in out:
            if '_ids' in row:
                row['members'] = [self.profile(int(x, 16)) for x in row.pop('_ids') if int(x, 16)]
        return {"lobbies": out, "reservation": public_hold(launcher_lobbies.current(me)), "time": int(time.time())}


def public_hold(h):
    return {k: v for k, v in h.items() if k != "key"} if h else None


def handle(ctx, method, p, u, body, acct):
    me, store = acct.xuid, Community(ctx)
    LAUNCHER_SEEN[me] = time.time()
    try:
        store.prune_messages()
    except Exception:  # noqa: BLE001 - housekeeping must never fail a request
        pass
    # Expired presence entries carry no durable meaning.
    for x, seen in list(LAUNCHER_SEEN.items()):
        if time.time() - seen > 120:
            LAUNCHER_SEEN.pop(x, None)
    if p == "/v1/community" and method == "GET":
        return store.snapshot(me)
    if p == "/v1/community/friends" and method == "POST":
        return store.request(me, body.get("player", ""))
    parts = p.split("/")
    if len(parts) in (5, 6) and parts[:4] == ["", "v1", "community", "friends"] and parts[4].isdigit():
        target = int(parts[4])
        if not 0 < target < 2**63:
            raise auth.AuthError(400, "bad_request")
        if len(parts) == 6 and parts[5] == "accept" and method == "POST":
            return store.respond(me, target, True)
        if len(parts) == 6 and parts[5] == "request" and method == "DELETE":
            return store.respond(me, target)
        if len(parts) == 5 and method == "DELETE":
            return store.remove(me, target)
    if p == "/v1/community/lobbies" and method == "GET":
        return store.lobbies(me)
    if p == "/v1/community/reservation":
        if method == "POST":
            return {"reservation": public_hold(launcher_lobbies.reserve(me, body.get("lobby"), body.get("kind")))}
        if method == "DELETE":
            launcher_lobbies.cancel(me)
            return {"ok": True}
    if p == "/v1/community/club/suggestions" and method == "GET":
        return store.club_suggestions(me)
    if p == "/v1/community/club/invite" and method == "POST":
        return store.club_invite(me, body.get("player", ""))
    if len(parts) == 5 and parts[:4] == ["", "v1", "community", "chat"]:
        ref = parts[4]
        if method == "GET":
            try:
                after = max(0, min(2**63-1, int((parse_qs(u.query).get("after") or [0])[0])))
            except ValueError:
                raise auth.AuthError(400, "bad_request") from None
            return store.messages(me, ref, after)
        if method == "POST":
            return store.send(me, ref, body.get("text"))
        if method == "PATCH":
            last = body.get("lastId", 0)
            if not isinstance(last, int) or isinstance(last, bool):
                raise auth.AuthError(400, "bad_request")
            return store.read(me, ref, last)
    raise auth.AuthError(404, "not_found")
