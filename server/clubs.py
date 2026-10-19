"""Car Clubs: clubs, members and roles, invitations, join requests, the noticeboard and each player's club history.

Everything lives in PostgreSQL (db.py, migration 2). A player is in at most one club. The rules come from the game's own club texts
(media/stringtables Clan.str): a tag is 3 or 4 characters and unique, a club has one Leader and at most 3 Co-Leaders, a club has a maximum
size, only Leader / Co-Leaders invite, approve, kick and post notices, and joining another club leaves the current one (a Leader who
leaves disbands the club). Wire values that the exe does not spell out are marked [guess] and are the first thing to check in game.
"""
import json
import time

import ugc

# Roles are stored (and compared) by power: member 1 < co-leader 2 < leader 3. The wire numbers run the other way, confirmed in game:
# 1 = LEADER, 2 = CO-LEADER, 3 = MEMBER, 0 = [Unknown] / none. Convert with wire_role / power_role at the protocol edge.
ROLE_MEMBER, ROLE_CO_LEADER, ROLE_LEADER = 1, 2, 3


def wire_role(power):
    return 4 - power if power else 0


def power_role(wire):
    return 4 - wire if 1 <= wire <= 3 else 0

PRIVACY_OPEN, PRIVACY_INVITE_ONLY, PRIVACY_PRIVATE = 1, 2, 3   # Open = 1 confirmed in game (the create dialog's "Open" sent 1); search sends 0 for
                                                               # "any"; Invite Only = 2 and Private = 3 are [guess] until those options are created
MAX_MEMBERS = 100
MAX_GARAGE = 20                       # cars in one Club Garage ("The Club Garage is full") [guess]
MAX_CO_LEADERS = 3
HISTORY_ACTIONS = {"created": 0, "joined": 1, "quit": 2, "kicked": 3, "promoted": 4, "demoted": 5, "disbanded": 6}   # [guess]

# Keyword lists for the three categories of ForzaCarClubGetClubKeywords (Category 0 region, 1 attitude, 2 interest [guess]).
KEYWORDS = (
    (0, ("Global", "North America", "South America", "Europe", "Asia", "Oceania", "Africa")),
    (1, ("Casual", "Friendly", "Competitive", "Hardcore", "Relaxed", "Serious")),
    (2, ("Racing", "Drifting", "Off-Road", "Cruising", "Car Collecting", "Tuning", "Design", "Photography")),
)


class ClubError(Exception):
    """A rule violation; `code` is the Xls ErrorCode the client maps to one of its club messages (see CODES)."""

    def __init__(self, text, code):
        super().__init__(text)
        self.text, self.code = text, code


# ErrorCode (the low 16 bits of the HRESULT 0x8051xxxx) -> meaning. The client maps 0x06, 0x07, 0x0e..0x1c and 0x4701..0x4703 to texts
# of Clan.str (FUN_140b2d220); which code is which text is [guess] until each message has been seen in game.
CODES = {
    "not_found": 0x06, "tag_exists": 0x07, "no_invites": 0x0e, "already_invited": 0x0f, "in_club": 0x10, "club_full": 0x11,
    "co_leader_limit": 0x12, "no_such_invite": 0x13, "no_privilege": 0x14, "already_recommended": 0x15, "not_in_club": 0x16,
    "bad_tag": 0x17, "bad_word": 0x18, "keywords": 0x19, "unknown": 0x1a, "garage_full": 0x1b,
}


def keyword_list():
    out, kid = [], 1
    for cat, words in KEYWORDS:
        for w in words:
            out.append({"Keyword": w, "KeywordId": kid, "Category": cat})
            kid += 1
    return out


KEYWORD_BY_ID = {k["KeywordId"]: k for k in keyword_list()}


class ClubStore:
    def __init__(self, db):
        self.db = db

    # ---------------------------------------------------------------- lookups
    def club(self, club_id, c=None):
        q = c.execute if c else self.db.execute
        return q("SELECT * FROM clubs WHERE id=?", (club_id,)).fetchone()

    def by_tag(self, tag, c=None):
        q = c.execute if c else self.db.execute
        return q("SELECT * FROM clubs WHERE lower(tag)=lower(?)", (tag,)).fetchone()

    def membership(self, xuid, c=None):
        """(club row, role) of the player's club, or (None, None)."""
        q = c.execute if c else self.db.execute
        r = q("SELECT m.role, c.* FROM club_members m JOIN clubs c ON c.id = m.club_id WHERE m.xuid=?", (xuid,)).fetchone()
        return (r, r["role"]) if r else (None, None)

    def member_info(self, xuid):
        """(clubId, clubTag, role) for the UserInfo struct of a player; (0, '', 0) without a club."""
        club, role = self.membership(xuid)
        return (club["id"], club["tag"], role) if club else (0, "", 0)

    def member_count(self, club_id):
        return self.db.one("SELECT COUNT(*) FROM club_members WHERE club_id=?", (club_id,))[0]

    def leaders(self, club_id, limit=20):
        return [r["xuid"] for r in self.db.query(
            "SELECT xuid FROM club_members WHERE club_id=? AND role>=? ORDER BY role DESC, joined LIMIT ?", (club_id, ROLE_CO_LEADER, limit))]

    def leader(self, club_id):
        r = self.db.one("SELECT xuid FROM club_members WHERE club_id=? AND role=? ", (club_id, ROLE_LEADER))
        return r["xuid"] if r else 0

    def champion(self, club_id):
        """The member with the most XP."""
        r = self.db.one("SELECT m.xuid FROM club_members m LEFT JOIN player_xp x ON x.xuid = m.xuid WHERE m.club_id=? "
                        "ORDER BY GREATEST(0, COALESCE(x.xp, 0) - m.xp_base) DESC, m.joined LIMIT 1", (club_id,))
        return r["xuid"] if r else 0

    # Club XP is what the members earned since they joined; the client shows level = XP / 100 + 1 (578 for 57713 XP, seen in game).
    GAIN = "GREATEST(0, x.xp - m.xp_base)"

    def xp(self, club_id):
        r = self.db.one(f"SELECT COALESCE(SUM({self.GAIN}), 0) FROM club_members m JOIN player_xp x ON x.xuid = m.xuid WHERE m.club_id=?", (club_id,))
        return int(r[0])

    def xp_rank(self, club_id):
        r = self.db.one(f"SELECT COUNT(*) + 1 FROM (SELECT m.club_id, SUM({self.GAIN}) AS t FROM club_members m JOIN player_xp x ON x.xuid = m.xuid "
                        f"GROUP BY m.club_id) s WHERE s.t > (SELECT COALESCE(SUM({self.GAIN}), 0) FROM club_members m JOIN player_xp x "
                        "ON x.xuid = m.xuid WHERE m.club_id=?)", (club_id,))
        return int(r[0])

    def members(self, club_id, limit):
        return self.db.query("SELECT xuid, role, status_changed FROM club_members WHERE club_id=? ORDER BY role DESC, joined LIMIT ?",
                             (club_id, limit))

    def accepts_invites(self, xuid):
        r = self.db.one("SELECT accepts FROM club_prefs WHERE xuid=?", (xuid,))
        return bool(r["accepts"]) if r else True

    def set_accepts_invites(self, xuid, accepts):
        with self.db.tx() as c:
            c.execute("INSERT INTO club_prefs (xuid, accepts) VALUES (?,?) ON CONFLICT (xuid) DO UPDATE SET accepts=excluded.accepts",
                      (xuid, int(bool(accepts))))

    # ---------------------------------------------------------------- search
    def search(self, privacy=-1, region=0, attitude=0, interest=0, activity=0, name="", limit=20, exclude_xuid=None):
        where, args = ["1=1"], []
        if privacy is not None and privacy > 0:          # 0 = any
            where.append("privacy=?")
            args.append(privacy)
        for col, v in (("region", region), ("attitude", attitude), ("interest", interest)):
            if v and v > 0:
                where.append(f"{col}=?")
                args.append(v)
        if name:
            where.append("name ILIKE ?")
            args.append(f"%{name}%")
        rows = self.db.query(
            f"SELECT *, (SELECT COUNT(*) FROM club_members m WHERE m.club_id = clubs.id) AS members FROM clubs WHERE {' AND '.join(where)} "
            "ORDER BY members DESC, id LIMIT ?", args + [max(1, limit)])
        return rows

    def recommended(self, xuid, limit):
        """Open and invite-only clubs the player is not in, biggest first."""
        club, _ = self.membership(xuid)
        return self.db.query(
            "SELECT *, (SELECT COUNT(*) FROM club_members m WHERE m.club_id = clubs.id) AS members FROM clubs WHERE privacy <> ? AND id <> ? "
            "ORDER BY members DESC, id LIMIT ?", (PRIVACY_PRIVATE, club["id"] if club else 0, max(1, limit)))

    def total(self):
        return self.db.one("SELECT COUNT(*) FROM clubs")[0]

    # Ladder tiers: (XP threshold, credit reward); the client lists them under "TIER n REWARD". The values are mine [guess].
    TIERS = ((1000, 10000), (5000, 25000), (20000, 50000), (60000, 100000), (150000, 250000))

    def ladder(self):
        """Every club with its XP, best first: [(club id, xp)]."""
        return [(r["id"], float(r["xp"])) for r in self.db.query(
            f"SELECT c.id, COALESCE(SUM({self.GAIN}), 0) AS xp FROM clubs c LEFT JOIN club_members m ON m.club_id = c.id "
            "LEFT JOIN player_xp x ON x.xuid = m.xuid GROUP BY c.id ORDER BY xp DESC, c.id")]

    def member_ladder(self, club_id):
        """The club's members ranked by the XP they earned since joining: [(xuid, xp)]."""
        return [(r["xuid"], float(r["xp"])) for r in self.db.query(
            f"SELECT m.xuid, COALESCE({self.GAIN}, 0) AS xp FROM club_members m LEFT JOIN player_xp x ON x.xuid = m.xuid "
            "WHERE m.club_id=? ORDER BY xp DESC, m.joined", (club_id,))]

    # ---------------------------------------------------------------- history
    def _history(self, c, xuid, club, action, role=0):
        c.execute("INSERT INTO club_history (xuid, club_id, club_name, action, role, created) VALUES (?,?,?,?,?,?)",
                  (xuid, club["id"], club["name"], HISTORY_ACTIONS[action], role, time.time()))

    def history(self, xuid, limit):
        return self.db.query("SELECT * FROM club_history WHERE xuid=? ORDER BY id DESC LIMIT ?", (xuid, max(1, limit)))

    # ---------------------------------------------------------------- create / change / disband
    def create(self, xuid, tag, name, description, privacy, region, attitude, interest):
        tag, name = (tag or "").strip(), (name or "").strip()
        if not 3 <= len(tag) <= 4:
            raise ClubError("club tags must be 3 or 4 characters long", CODES["bad_tag"])
        if not name:
            raise ClubError("a club needs a name", CODES["unknown"])
        with self.db.tx() as c:
            if self.membership(xuid, c)[0]:
                raise ClubError("already in a club", CODES["in_club"])
            if self.by_tag(tag, c):
                raise ClubError("the club tag already exists", CODES["tag_exists"])
            cid = c.execute("INSERT INTO clubs (tag, name, description, privacy, region, attitude, interest, created) VALUES (?,?,?,?,?,?,?,?)",
                            (tag, name[:50], (description or "")[:400], privacy, region, attitude, interest, time.time())).lastrowid
            c.execute("INSERT INTO club_members (xuid, club_id, role, joined, status_changed, xp_base) VALUES "
                      "(?,?,?,?,?, COALESCE((SELECT xp FROM player_xp WHERE xuid=?), 0))", (xuid, cid, ROLE_LEADER, time.time(), time.time(), xuid))
            self._history(c, xuid, {"id": cid, "name": name}, "created", ROLE_LEADER)
        return cid

    def change_details(self, xuid, name, description, privacy, region, attitude, interest):
        with self.db.tx() as c:
            club, role = self.membership(xuid, c)
            if not club:
                raise ClubError("not in a club", CODES["not_in_club"])
            if role != ROLE_LEADER:
                raise ClubError("only the leader changes the club", CODES["no_privilege"])
            c.execute("UPDATE clubs SET name=?, description=?, privacy=?, region=?, attitude=?, interest=? WHERE id=?",
                      ((name or club["name"])[:50], (description or "")[:400], privacy, region, attitude, interest, club["id"]))
            return club["id"]

    def _remove(self, c, xuid, club, action):
        c.execute("DELETE FROM shared_cars WHERE owner=? AND club_id=?", (xuid, club["id"]))       # a member's shared cars leave with them
        c.execute("DELETE FROM club_members WHERE xuid=?", (xuid,))
        self._history(c, xuid, club, action)

    def disband(self, c, club):
        """Removes the club; returns the xuids of the members it had."""
        members = [r["xuid"] for r in c.execute("SELECT xuid FROM club_members WHERE club_id=?", (club["id"],)).fetchall()]
        for x in members:
            self._history(c, x, club, "disbanded")
        c.execute("DELETE FROM clubs WHERE id=?", (club["id"],))          # members, invites, requests, notices cascade
        return members

    # ---------------------------------------------------------------- joining
    def _join(self, c, xuid, club, role=ROLE_MEMBER):
        """Adds `xuid` to `club`, leaving a previous club first (a Leader's club is disbanded). Returns the xuids disbanded out of the old club."""
        old, old_role = self.membership(xuid, c)
        left = []
        if old and old["id"] != club["id"]:
            if old_role == ROLE_LEADER:
                left = [x for x in self.disband(c, old) if x != xuid]
            else:
                self._remove(c, xuid, old, "quit")
        if c.execute("SELECT COUNT(*) FROM club_members WHERE club_id=?", (club["id"],)).fetchone()[0] >= MAX_MEMBERS:
            raise ClubError("the club has reached its maximum size", CODES["club_full"])
        c.execute("INSERT INTO club_members (xuid, club_id, role, joined, status_changed, xp_base) VALUES "
                  "(?,?,?,?,?, COALESCE((SELECT xp FROM player_xp WHERE xuid=?), 0)) ON CONFLICT (xuid) DO NOTHING",
                  (xuid, club["id"], role, time.time(), time.time(), xuid))
        c.execute("DELETE FROM club_requests WHERE xuid=?", (xuid,))
        c.execute("DELETE FROM club_invites WHERE invitee=? AND club_id=?", (xuid, club["id"]))
        self._history(c, xuid, club, "joined", role)
        return left

    def request_join(self, xuid, club_id=None, tag=None):
        """An open club takes the player at once; others queue a request. Returns (club, approved)."""
        with self.db.tx() as c:
            club = self.club(club_id, c) if club_id is not None else self.by_tag(tag or "", c)
            if not club:
                raise ClubError("club not found", CODES["not_found"])
            cur, _ = self.membership(xuid, c)
            if cur and cur["id"] == club["id"]:
                raise ClubError("already in this club", CODES["in_club"])
            if club["privacy"] == PRIVACY_OPEN:
                self._join(c, xuid, club)
                return club, True
            if club["privacy"] == PRIVACY_PRIVATE:
                raise ClubError("the club is private", CODES["no_privilege"])
            c.execute("INSERT INTO club_requests (club_id, xuid, created) VALUES (?,?,?) ON CONFLICT DO NOTHING", (club["id"], xuid, time.time()))
            return club, False

    def approve(self, leader_xuid, user_xuid):
        with self.db.tx() as c:
            club, role = self.membership(leader_xuid, c)
            if not club or role < ROLE_CO_LEADER:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            if self.membership(user_xuid, c)[0] and self.membership(user_xuid, c)[0]["id"] == club["id"]:
                return club                                    # already in (joined meanwhile): accepting a stale request is a no-op
            if not c.execute("SELECT 1 FROM club_requests WHERE club_id=? AND xuid=?", (club["id"], user_xuid)).fetchone():
                raise ClubError("no such request", CODES["no_such_invite"])
            self._join(c, user_xuid, club)
            return club

    def invite(self, sponsor, invitee):
        with self.db.tx() as c:
            club, role = self.membership(sponsor, c)
            if not club:
                raise ClubError("not in a club", CODES["not_in_club"])
            if role < ROLE_CO_LEADER:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            if not self.accepts_invites(invitee):
                raise ClubError("the user does not accept club invites", CODES["no_invites"])
            if self.membership(invitee, c)[0]:
                raise ClubError("the player is already in a club", CODES["in_club"])
            if c.execute("SELECT 1 FROM club_invites WHERE club_id=? AND invitee=?", (club["id"], invitee)).fetchone():
                raise ClubError("already invited", CODES["already_invited"])
            iid = c.execute("INSERT INTO club_invites (club_id, sponsor, invitee, created) VALUES (?,?,?,?)",
                            (club["id"], sponsor, invitee, time.time())).lastrowid
            return club, iid

    def accept_invite(self, xuid, club_id, invitation_id):
        with self.db.tx() as c:
            # the client's invitation id is the id of the Message Center notification, which is not the row id here: match on club + invitee
            inv = c.execute("SELECT * FROM club_invites WHERE club_id=? AND invitee=?", (club_id, xuid)).fetchone()
            club = self.club(club_id, c)
            if not inv or not club:
                raise ClubError("the invite does not exist", CODES["no_such_invite"])
            self._join(c, xuid, club)
            return club

    # ---------------------------------------------------------------- members
    def change_role(self, actor, target, new_role):
        """Returns (club, old role). Promoting someone to Leader hands the leadership over (the actor becomes a Co-Leader)."""
        with self.db.tx() as c:
            club, role = self.membership(actor, c)
            tclub, trole = self.membership(target, c)
            if not club:
                raise ClubError("not in a club", CODES["not_in_club"])
            if not tclub or tclub["id"] != club["id"]:
                raise ClubError("not in your club", CODES["not_in_club"])
            if actor != target and role != ROLE_LEADER:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            if actor == target and new_role > role:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            if new_role == ROLE_CO_LEADER and trole != ROLE_CO_LEADER and c.execute(
                    "SELECT COUNT(*) FROM club_members WHERE club_id=? AND role=?", (club["id"], ROLE_CO_LEADER)).fetchone()[0] >= MAX_CO_LEADERS:
                raise ClubError("only 3 Co-Leaders are allowed", CODES["co_leader_limit"])
            if new_role == ROLE_LEADER and actor != target:
                c.execute("UPDATE club_members SET role=?, status_changed=? WHERE xuid=?", (ROLE_CO_LEADER, time.time(), actor))
            c.execute("UPDATE club_members SET role=?, status_changed=? WHERE xuid=?", (new_role, time.time(), target))
            self._history(c, target, club, "promoted" if new_role > trole else "demoted", new_role)
            return club, trole

    def remove(self, actor, target):
        """Kick (or quit when actor == target). Returns (club, kind, disbanded members): kind 'quit' | 'kicked' | 'disbanded'."""
        with self.db.tx() as c:
            club, role = self.membership(actor, c)
            if not club:
                raise ClubError("not in a club", CODES["not_in_club"])
            tclub, trole = self.membership(target, c)
            if not tclub or tclub["id"] != club["id"]:
                raise ClubError("not in your club", CODES["not_in_club"])
            if actor == target:
                if role == ROLE_LEADER:
                    return club, "disbanded", [x for x in self.disband(c, club) if x != actor]
                self._remove(c, target, club, "quit")
                return club, "quit", []
            if role < ROLE_CO_LEADER or trole >= role:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            self._remove(c, target, club, "kicked")
            return club, "kicked", []

    def recommend_removal(self, voter, target):
        with self.db.tx() as c:
            club, _ = self.membership(voter, c)
            tclub, _ = self.membership(target, c)
            if not club or not tclub or club["id"] != tclub["id"]:
                raise ClubError("not in your club", CODES["not_in_club"])
            if c.execute("SELECT 1 FROM club_removal_votes WHERE club_id=? AND target=? AND voter=?", (club["id"], target, voter)).fetchone():
                raise ClubError("already recommended", CODES["already_recommended"])
            c.execute("INSERT INTO club_removal_votes (club_id, target, voter, created) VALUES (?,?,?,?)", (club["id"], target, voter, time.time()))
            return club

    # ---------------------------------------------------------------- noticeboard
    def post_notice(self, xuid, message, notification_ids):
        """Records a notice; `notification_ids` are the per-member Message Center rows created for it. Returns the notice id."""
        with self.db.tx() as c:
            club, role = self.membership(xuid, c)
            if not club or role < ROLE_CO_LEADER:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            nid = c.execute("INSERT INTO club_notices (club_id, sender, role, message, created, notification_ids) VALUES (?,?,?,?,?,?)",
                            (club["id"], xuid, role, message, time.time(), json.dumps(notification_ids))).lastrowid
            return nid

    def notice(self, notice_id):
        return self.db.one("SELECT * FROM club_notices WHERE id=?", (notice_id,))

    def delete_notice(self, xuid, notice_id):
        """Returns the Message Center ids to delete."""
        with self.db.tx() as c:
            club, role = self.membership(xuid, c)
            n = c.execute("SELECT * FROM club_notices WHERE id=?", (notice_id,)).fetchone()
            if not club or role < ROLE_CO_LEADER or not n or n["club_id"] != club["id"]:
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            c.execute("DELETE FROM club_notices WHERE id=?", (notice_id,))
            return json.loads(n["notification_ids"])

    # ---------------------------------------------------------------- invitation / join candidates
    def users_to_invite(self, xuid, limit):
        """Players who could be invited: have logged in, not in a club. (No friends list yet: every known player.)"""
        return self.db.query(
            "SELECT p.xuid, p.gamertag, COALESCE(cp.accepts, 1) AS accepts FROM players p LEFT JOIN club_prefs cp ON cp.xuid = p.xuid "
            "WHERE p.xuid <> ? AND p.career_level > 0 AND p.gamertag <> '' AND NOT EXISTS (SELECT 1 FROM club_members m WHERE m.xuid = p.xuid) "
            "ORDER BY p.last_seen DESC LIMIT ?", (xuid, max(1, limit)))

    def users_to_join(self, xuid, limit):
        """Players who are in a club, with that club's size and privacy (so one can ask to join their club)."""
        return self.db.query(
            "SELECT m.xuid, c.name, c.privacy, (SELECT COUNT(*) FROM club_members mm WHERE mm.club_id = c.id) AS members "
            "FROM club_members m JOIN clubs c ON c.id = m.club_id WHERE m.xuid <> ? ORDER BY m.joined DESC LIMIT ?", (xuid, max(1, limit)))

    # ---------------------------------------------------------------- Club Garage (shared cars)
    def textures_create(self, owner, livery, thumbnail):
        with self.db.tx() as c:
            return c.execute("INSERT INTO shared_textures (owner, livery, thumbnail, created) VALUES (?,?,?,?)",
                             (owner, ugc.pack(bytes(livery)), ugc.pack(bytes(thumbnail)), time.time())).lastrowid

    def textures(self, textures_id):
        r = self.db.one("SELECT * FROM shared_textures WHERE id=?", (textures_id,))
        return (ugc.unpack(r["livery"]), ugc.unpack(r["thumbnail"])) if r else (b"", b"")

    def share_car(self, xuid, game_car_id, profile_id, model_id, car, textures_id, livery, tuning, overwrite):
        """Puts a car in the player's club garage. Sharing the same garage car again replaces the shared one (version + 1) when
        `overwrite`, otherwise leaves it as it is. Returns (car id, version, club)."""
        with self.db.tx() as c:
            club, _ = self.membership(xuid, c)
            if not club:
                raise ClubError("not in a club", CODES["not_in_club"])
            old = c.execute("SELECT id, version FROM shared_cars WHERE owner=? AND game_car_id=?", (xuid, bytes(game_car_id))).fetchone()
            now = time.time()
            if old:
                if overwrite:
                    c.execute("UPDATE shared_cars SET club_id=?, car=?, model_id=?, textures_id=?, livery=?, tuning=?, version=version+1, "
                              "shared=? WHERE id=?", (club["id"], json.dumps(car), model_id, textures_id, ugc.pack(bytes(livery)),
                                                      ugc.pack(bytes(tuning)), now, old["id"]))
                    return old["id"], old["version"] + 1, club
                return old["id"], old["version"], club
            if c.execute("SELECT COUNT(*) FROM shared_cars WHERE club_id=?", (club["id"],)).fetchone()[0] >= MAX_GARAGE:
                raise ClubError("the club garage is full", CODES["garage_full"])
            cid = c.execute("INSERT INTO shared_cars (club_id, owner, game_car_id, profile_id, model_id, car, textures_id, livery, tuning, "
                            "shared, last_used) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                            (club["id"], xuid, bytes(game_car_id), bytes(profile_id), model_id, json.dumps(car), textures_id,
                             ugc.pack(bytes(livery)), ugc.pack(bytes(tuning)), now, now)).lastrowid
            return cid, 1, club

    def club_cars(self, xuid, limit):
        club, _ = self.membership(xuid)
        if not club:
            return []
        return self.db.query("SELECT id, club_id, owner, model_id, car, version, shared, last_used, race_count, textures_id FROM shared_cars "
                             "WHERE club_id=? ORDER BY shared DESC LIMIT ?", (club["id"], max(1, limit)))

    def shared_car(self, car_id):
        r = self.db.one("SELECT * FROM shared_cars WHERE id=?", (car_id,))
        if not r:
            return None
        d = dict(r)
        d["livery"], d["tuning"] = ugc.unpack(d["livery"]), ugc.unpack(d["tuning"])
        return d

    def remove_car(self, xuid, car_id):
        with self.db.tx() as c:
            car = c.execute("SELECT owner, club_id FROM shared_cars WHERE id=?", (car_id,)).fetchone()
            if not car:
                return False
            club, role = self.membership(xuid, c)
            if car["owner"] != xuid and not (club and club["id"] == car["club_id"] and role >= ROLE_CO_LEADER):
                raise ClubError("insufficient privileges", CODES["no_privilege"])
            c.execute("DELETE FROM shared_cars WHERE id=?", (car_id,))
            return True

    def count_race(self, car_id):
        with self.db.tx() as c:
            c.execute("UPDATE shared_cars SET race_count = race_count + 1, last_used=? WHERE id=?", (time.time(), car_id))
