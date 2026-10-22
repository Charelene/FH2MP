#!/usr/bin/env python3
"""FH2 Turn 10 LSP stand-in (Xls web services over HTTP, Content-Type bin/xtsw).

The shim's IXMLHTTPRequest2 (shim/src/xhr2.inc) redirects every request the game makes through
XMLHTTP60 to this server (default 127.0.0.1:5080, plain HTTP). The original host arrives in
X-FH2MP-Host; only forzamotorsport.net hosts are Turn 10 Xls calls, the rest (xboxlive.com ...) are
answered with an empty 200.

Request : headers X-ClassName / X-Method, body = "<Call>_Parameters" struct, big endian.
Response: 12-byte header (Result u32, ErrorCode u32, DataLength u32; all big endian) followed by the
          "<Call>_Results" struct. Result 0 = OK; 2 = error (ErrorCode | 0x80510000), DataLength bytes
          of message text follow the header. Schemas come from the user's own exe (server/xls_schema.py).

Usage: python3 -I server/fh2mp_lsp.py [port]
       State and game catalog (cars, tracks, strings, schemas, shipped Drivatars) live in PostgreSQL: FH2MP_PG_DSN (libpq conninfo,
       default `dbname=fh2mp`); the catalog is loaded once with tools/export_gamedata.py + tools/load_gamedata_pg.py from an install:
       the server itself never reads game files. Also: FH2MP_LSP_BIND (listen address, default 127.0.0.1), FH2MP_PUBLIC_HOST /
       FH2MP_GAME_PORT (the game server address handed to clients).
"""
import json
import os
import re
import struct
import sys
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import threading  # noqa: E402
import xls  # noqa: E402
import drivatar  # noqa: E402
import scoreboard  # noqa: E402
import rivals  # noqa: E402
import ugc  # noqa: E402
import pngtool  # noqa: E402
import xls_schema as S  # noqa: E402
import cardb  # noqa: E402
import db  # noqa: E402
import notifstore  # noqa: E402
import clubs  # noqa: E402
import social  # noqa: E402
import xbl  # noqa: E402
import auth  # noqa: E402
import launcher_api  # noqa: E402
import ops_api  # noqa: E402

SEQ = 0
SC = None
STATE = {}
CTX = threading.local()   # per-request: xuid (from the session token; X-UserId only for legacy clients), authed, tag
DRV = None                # drivatar.DrivatarStore
CARS = None               # cardb.CarDb (the user's own game database)
SB = None                 # scoreboard.ScoreStore
DB = None                 # db.Database (PostgreSQL)
UGC = None                # ugc.UgcStore (paint jobs, tunes, layer groups)
NOTIF = None              # notifstore.NotificationStore
CLUBS = None              # clubs.ClubStore
SOCIAL = None             # social.SocialStore
AUTH = None               # auth.AuthService (accountless identity, server/auth.py)
LAUNCHER = None           # launcher_api.Ctx
OPS = ops_api.OpsConfig() # operator API access (token + allowed addresses), see ops_api.py; replaced from the environment in setup_auth
LEVELS = {}               # xuid -> currentCareerLevel as sent at Logon
TAGS = {}                 # xuid -> gamertag, learned at Logon


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def candidates(cls, method, suffix):
    """Possible descriptor-entry names for a call, e.g. ForzaUserLogon_Results."""
    cls = cls.split(",")[0].strip()          # "Forza.WebServices.User"
    parts = cls.split(".")
    names = [
        "".join(parts) + method,                                 # ForzaWebServicesUserLogon
        "".join(p for p in parts if p != "WebServices") + method,  # ForzaUserLogon
        "".join(parts[1:]) + method,                              # WebServicesUserLogon
        "".join(p for p in parts[1:] if p != "WebServices") + method,
    ]
    return [n + suffix for n in names]


def find_struct(cls, method, suffix):
    for n in candidates(cls, method, suffix):
        try:
            return n, SC.call_struct(n)
        except KeyError:
            continue
    return None, None


def frame(payload=b"", result=0, error=0, text=b""):
    return struct.pack(">III", result, error, len(text)) + text + payload


# ---------------------------------------------------------------- handlers

def logon(req):
    lg = req["loginData"]
    if getattr(CTX, "authed", False):
        lg["xuid"], lg["gamerTag"] = CTX.xuid, CTX.tag          # the account decides who this is, not what the client claims
    TAGS[lg["xuid"]] = lg["gamerTag"]
    LEVELS[lg["xuid"]] = lg["currentCareerLevel"]
    with DB.tx() as c:
        db.touch_player(c, lg["xuid"], lg["gamerTag"], level=lg["currentCareerLevel"])
    log(f"  Logon xuid={lg['xuid']:#x} tag={lg['gamerTag']!r} lcid={lg['LCID']}")
    r = xls.default_struct(SC, SC.sub(SC.call_struct("ForzaUserLogon_Results")[0]))
    r.update({
        "isProfileValid": True,
        "notificationNonce": 1,
        "notificationPollIntervalSeconds": 5,
        "defaultFriendsPollIntervalSeconds": 60,
        "maxFriendsPollIntervalSeconds": 300,
        "defaultTimeoutMs": 30000,
        "defaultContentTimeoutMs": 40000,
        "defaultInGameTimeoutMs": 30000,
        "userData": {**_owner(r["userData"], lg["xuid"]), "wzGamerTag": lg["gamerTag"],
                     "acceptsClubInvites": CLUBS.accepts_invites(lg["xuid"])},
        # The client's permission table comes from here (it never calls GetPermissionsTable in practice); an empty
        # table makes every feature gate read "locked" (rival races, gift cars: "cannot connect to Xbox Live").
        # Capacity of the array is 256 x 16 bytes.
        "permissions": all_permissions(255),
        # The client keeps at most 7 categories; it shows the ones with a tab (1, 3, 5, 6, 7) in the Message Center.
        "notificationCategories": [
            {"type": t, "name": name, "description": desc, "backgroundImageFileId": 0}
            for t, (name, desc) in sorted(CATEGORIES.items())
        ],
    })
    return {"responseData": r}


# ---------------------------------------------------------------- notifications (Message Center)

POLL_SECONDS = 5
MSG_TYPE = 0x200    # notificationType carrying MessageNotificationParameters (to be confirmed in game)
filetime = notifstore.filetime


# notificationType -> parameter struct, from the client's dispatcher FUN_140f15ec0 (struct descriptors
# found via the exe's own names). Types not listed here are not dispatched by the client.
NOTIF_STRUCTS = {
    0x000: "PermissionsDirtyNotificationParameters",
    0x100: "NewDlcNotificationParameters",
    0x200: "MessageNotificationParameters", 0x201: "MessageNotificationParameters",
    0x203: "MessageNotificationParameters",
    0x600: "BannedNotificationParameters", 0x601: "UnbannedNotificationParameters",
    0x602: "MessageNotificationParameters",
    0x8000: "UserBadgeGrantedNotificationParameters", 0x8001: "SetCreditsNotificationParameters",
    0x8301: "SharedCarNotificationParameters", 0x8302: "CurationPayoutNotificationParameters",
    0x8303: "DrivatarPayoutNotificationParameters", 0x8401: "MessageNotificationParameters",
    0x8402: "TimeTrialRecommendNotificationParameters", 0x8403: "TimeTrialTimeBeatenNotificationParameters",
    0x8500: "GiftReceiveMoneyNotificationParameters", 0x8501: "GiftReceiveItemNotificationParameters",
    0x8502: "GiftReceiveCarNotificationParameters", 0x8503: "StorefrontOutOfStockNotificationParameters",
    0x8504: "StorefrontMoneyWaitingNotificationParameters",
    # Car Club notifications (Message Center category 7): the structs have no registered name, so they
    # are given by descriptor VA (build 1.0.0.64): 0x700 join request {requestorXuid, requestorGamertag,
    # clubId}, 0x701 invite {sponsorXuid, clubId, clubName, clubTag, sponsorGamertag}, 0x702 removal vote,
    # 0x703 kicked, 0x704/0x705 {clubName, clubTag}, 0x706 member left, 0x707 new member, 0x708 promotion,
    # 0x709 noticeboard message {senderXuid, senderClubRole, senderGamertag, noticeBoardMessage}.
    **{0x700 + i: 0x1436ead20 + 0x10 * i for i in range(10)},
}
STRUCT_FALLBACK = "MessageNotificationParameters"

# Message Center tabs, confirmed in game 2026-10-16. Category 2 is the Community News page (not a tab), 4 never
# showed anything.
CATEGORIES = {
    1: ("New Content", "New downloadable content"),
    2: ("Announcements", "Community news and announcements"),
    3: ("Rewards", "Payouts and rewards"),
    4: ("Time Trials", "Time trial notices"),
    5: ("Gifts", "Gifts from other players"),
    6: ("Messages", "Messages and account notices"),
    7: ("Car Club", "Car club requests and notices"),
}

# kind -> (notificationType, where it shows up). Any other dispatched type can be sent by its number.
KINDS = {
    "announcement": (0x200, "Community News page"), "dlc": (0x100, "category 1"),
    "reward_curation": (0x8302, "category 3 (shown as Layer Group Reward)"),
    "reward_drivatar": (0x8303, "category 3 (shown as Drivatar Reward)"),
    "gift_money": (0x8500, "category 5"), "gift_item": (0x8501, "category 5 (GiftFileType 0 = tuning)"),
    "gift_car": (0x8502, "category 5"),
    "message": (0x602, "category 6"), "ban": (0x600, "category 6 (applies a feature ban, undo with unban)"),
    "unban": (0x601, "category 6"),
    "club_join_request": (0x700, "category 7"), "club_invite": (0x701, "category 7"),
    "club_removal_vote": (0x702, "category 7"), "club_kicked": (0x703, "category 7"),
    "club_notice_a": (0x704, "category 7"), "club_notice_b": (0x705, "category 7"),
    "club_member_left": (0x706, "category 7"), "club_new_member": (0x707, "category 7"),
    "club_promotion": (0x708, "category 7"), "club_noticeboard": (0x709, "category 7"),
}


def notif_fields(ntype):
    s = NOTIF_STRUCTS.get(ntype, STRUCT_FALLBACK)
    return SC.fields(s) if isinstance(s, int) else SC.call_struct(s)


def add_notification(message, sra="", ntype=None, params=None, xuid=0, car_id=None, gift_id=False):
    n = NOTIF.add(message, sra, MSG_TYPE if ntype is None else ntype, params, xuid, car_id, gift_id)
    log(f"  + notification #{n['id']} type={n['type']:#x}: {message!r}")
    return n


def _coerce(kind, v):
    if kind in (9, 0x10):
        return str(v)
    if kind == 0x11:
        return v if isinstance(v, bool) else str(v).lower() in ("1", "true", "yes")
    if kind == 7:
        return float(v)
    return v if isinstance(v, int) else int(str(v), 0)


def notif_params(ntype, extra):
    """Admin query extras (Field=value) -> typed values for the type's parameter struct."""
    if not extra:
        return {}
    fields = notif_fields(ntype)
    kinds = {f.name: f.type for f in fields}
    out = {}
    for k, v in extra.items():
        if k not in kinds:
            raise ValueError(f"no field {k!r} in {NOTIF_STRUCTS.get(ntype, STRUCT_FALLBACK)}; have {sorted(kinds)}")
        out[k] = _coerce(kinds[k], v)
    return out


def _visible():
    """Notifications for the requesting player: broadcasts (no xuid) plus ones addressed to them."""
    return NOTIF.visible(getattr(CTX, "xuid", 0))


def notif_retrieve(req):
    results, block = [], bytearray()
    vis = _visible()
    for n in vis:
        fields = notif_fields(n["type"])
        val = xls.default_struct(SC, fields)
        if "Message" in val:
            val["Message"] = n["message"]
        if "SRAUri" in val:
            val["SRAUri"] = n["sra"]
        if "OfferId" in val:
            val["OfferId"] = n["sra"]
        val.update(n.get("params", {}))
        pb = xls.encode(SC, fields, val)
        results.append({"id": n["id"], "xuidRecipient": n.get("xuid", 0), "notificationType": n["type"],
                        "hasThumbnail": False, "isRead": n["read"], "deleteAction": 0,
                        "expirationDate": filetime(time.time() + 86400 * 365), "sentDate": n["sent"],
                        "cbParameters": len(pb)})
        block += pb
    return {"results": results, "parameterBlock": bytes(block), "nextRetrieveSeconds": POLL_SECONDS,
            "reconcileActiveNotifications": True, "activeNotificationIds": [n["id"] for n in vis]}


def notif_mark_read(req):
    NOTIF.mark_read(getattr(CTX, "xuid", 0), req["notificationId"])
    return None


def notif_clear(req):
    NOTIF.clear(getattr(CTX, "xuid", 0), req["notificationId"])      # clears it for this player only
    return None


# ---------------------------------------------------------------- drivatars

def _me():
    xuid = getattr(CTX, "xuid", 0)
    return xuid, TAGS.get(xuid, "")


def drv_save(req):
    xuid, tag = _me()
    DRV.save_upload(xuid, tag, req["info"], req["curveTypes"], req["segmentInformation"])
    log(f"  stored Drivatar upload from {xuid:#x} ({len(req['segmentInformation'])} segments, "
        f"{len(req['curveTypes'])} curves)")
    return None


def drv_for_cache(req):
    xuid, tag = _me()
    DRV.for_player(xuid, tag)          # makes sure the requesting player has one (profile-based if needed)
    n = req["numberOfDrivatars"]
    # The client replaces its built-in set with whatever we return here, so send the whole shipped
    # set plus the players' own Drivatars, never a short or empty list.
    nplayers = DRV.count()
    recs = DRV.pool(max(n, len(DRV.templates) + nplayers))
    log(f"  ForCache numberOfDrivatars={n}: serving {len(recs)} ({nplayers} player + shipped)")
    return {"drivatarInfos": recs}


def drv_for_race(req):
    xuid, tag = _me()
    me = DRV.for_player(xuid, tag)
    me["wristbandLevel"] = DRV.bucket_wristband(req["bucketId"])   # the car the player is driving now
    DRV.set_wristband(xuid, me["wristbandLevel"])
    cached = set(req["cachedDrivatars"]) | set(req["cachedDrivatarsNewCars"])
    skill = req["skillSelection"] if 0 <= req["skillSelection"] <= 4 else None
    others = DRV.pool(max(0, req["numberOfDrivatars"]), skill=skill, near_wristband=me["wristbandLevel"],
                      exclude_xuid=xuid, exclude_xuids=cached)
    log(f"  ForRace bucket={req['bucketId']} n={req['numberOfDrivatars']} skill={req['skillSelection']} "
        f"cached={len(req['cachedDrivatars'])}+{len(req['cachedDrivatarsNewCars'])}: me + {len(others)} others")
    return {"playerData": me, "drivatarInfos": others,
            "currentTime": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}


# ---------------------------------------------------------------- scoreboards (Rivals / leaderboards)

def _score_row(sc_row, rowdict):
    """Fill a ScoreboardRow (wire dict) from a scoreboard.py row."""
    user = rowdict["user"]
    lvl = LEVELS.get(sc_row["xuid"], 100000 + scoreboard.hash_int(sc_row["tag"], "lvl") % 900000)
    user.update({"qwXuid": sc_row["xuid"], "wzGamerTag": (sc_row["tag"] or TAGS.get(sc_row["xuid"], ""))[:15],
                 "currentCareerLevel": lvl})
    rowdict.update({
        "position": sc_row["position"], "rank": sc_row["position"], "score": scoreboard.dbl_to_u64(sc_row["score"]),
        "submittedTime": scoreboard.iso(sc_row["t"]), "carId": sc_row["car"], "carClassId": min(sc_row["cls"], TOP_CLASS),
        "carPerformanceIndex": sc_row["cpi"], "usedSTM": sc_row["stm"], "usedTCS": sc_row["tcs"],
        "usedABS": sc_row["abs"], "isClean": sc_row["clean"], "ghostFileId": sc_row.get("ghost", 0), "bounty": 0})
    return rowdict


def sb_submit_score(req):
    xuid, tag = _me()
    d = req["data"]
    s = SB.submit(xuid, tag, req["type"], d)
    if s is None:
        why = "class X/R not allowed on a Rivals board" if (d["scoreboardType"] in scoreboard.EVENT_BOARDS and
                                                             d["carClassId"] in scoreboard.MAXED_CLASSES) else "score is not a finite number"
        log(f"  SubmitScore board=({d['scoreboardType']},{req['type']}) track={d['trackId']}: ignored, {why}")
        return None
    log(f"  SubmitScore board=({d['scoreboardType']},{req['type']}) track={d['trackId']} car={d['carId']} "
        f"score={s['score']:.3f} clean={d['isClean']} -> ghost upload for score {s['score_id'].hex()[:8]}")
    try:
        notify_beaten(xuid, tag, req["type"], d, s)
    except Exception as ex:          # a notification problem must never fail the score submission
        log(f"  notify_beaten failed: {ex!r}")
    return {"scoreId": s["score_id"], "uploadFiles": True}


def notify_beaten(xuid, tag, score_type, d, s):
    """A new personal best on a Monthly event tells every other real player whose best on that event is slower: "<tag> beat you in
    the <event> challenge" (Message Center type 0x8403, shown under Rivals > My Rivals [C]). One notification per (beaten player,
    beater, event): a newer one replaces the old, so repeat laps do not pile up."""
    e = event_info(d["gameScoreboardId"]) if d["scoreboardType"] == 3 else None
    if e is None:
        return
    best = SB._best_per_player(3, score_type, -1, pivot=e["id"])
    mine = best.get(xuid)
    if mine is None or abs(mine["score"] - s["score"]) > 1e-9:
        return                                   # not the submitter's best on this event
    for other_xuid, row in best.items():
        if other_xuid == xuid or not SB.better(score_type, s["score"], row["score"]):
            continue
        send_beaten(other_xuid, xuid, tag, e, d["carId"], d["carClassId"], score_type, s["score"])


def send_beaten(beaten_xuid, beater_xuid, beater_tag, e, car_id, car_class, score_type, score):
    """Message Center 0x8403 to `beaten_xuid`; replaces an older one from the same beater for the same event."""
    for n in NOTIF.visible(beaten_xuid):
        p = n.get("params") or {}
        if n["type"] == 0x8403 and p.get("timeTrialId") == e["id"] and p.get("senderXuid") == beater_xuid:
            NOTIF.delete(n["id"])
    params = {"senderXuid": beater_xuid, "senderGamertag": (beater_tag or "Rival")[:31], "senderClubTag": "", "senderClubRole": 0,
              "senderFlags": 0, "timeTrialId": e["id"], "carId": car_id, "carClassId": min(car_class, TOP_CLASS),
              "localizedTimeTrialName": e["name"][:127], "trackName": e["trackname"][:63], "trackId": e["track"],
              "score": scoreboard.dbl_to_u64(score), "scoreType": score_type, "category": MONTHLY_CATEGORY}
    add_notification("", "", 0x8403, params, xuid=beaten_xuid)
    log(f"  {beater_tag or beater_xuid} beat player {beaten_xuid:#x} on event {e['id']}: told them (0x8403)")


def sb_notify_rival(req):
    """The client says it beat `rivalXuid` in event `rivalEventId` with score `myScoreId` (Scoreboard.NotifyRival, no result): that
    rival gets the "beat you" message, as in retail [inferred: the call is made when a real rival is beaten]. Made-up rivals have
    xuid 0 and are ignored."""
    xuid, tag = _me()
    sub = SB.by_score_id(req["myScoreId"])
    e = event_info(req["rivalEventId"])
    log(f"  NotifyRival event={req['rivalEventId']} rival={req['rivalXuid']:#x} score={'%.3f' % sub['score'] if sub else '?'}")
    if sub is None or e is None or not req["rivalXuid"] or req["rivalXuid"] == xuid:
        return None
    send_beaten(req["rivalXuid"], xuid, tag or sub["tag"], e, sub["car"], sub["cls"], sub["st"], sub["score"])
    return None


def announce_rotation():
    """Once per rotation, a Rival Recommendation (0x8402) to everyone for one of the new events (Message Center / My Rivals)."""
    if RIV is None:
        return
    month, n = RIV.serial()
    key = f"{month}:{n}"
    r = DB.one("SELECT value FROM meta WHERE key='rival_announced'")
    if r and r["value"] == key:
        return
    evs = RIV.events()
    if not evs:
        return
    e = evs[(month * 7 + n * 3) % len(evs)]
    params = {"TimeTrialId": e["id"], "localizedTimeTrialName": e["name"][:127], "trackName": e["trackname"][:63],
              "description": e["description"][:511], "category": MONTHLY_CATEGORY}
    add_notification("", "", 0x8402, params, xuid=0)
    with DB.tx() as c:
        c.execute("INSERT INTO meta (key, value) VALUES ('rival_announced', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key,))
    log(f"  announced rotation {key}: recommended event {e['id']} ({e['name']})")


def sb_upload_file(req):
    sid = SB.attach_ghost(req["scoreId"], req["data"])
    log(f"  UploadFile scoreId={bytes(req['scoreId']).hex()[:8]} {len(req['data'])} bytes -> ghost #{sid}")
    return None


def sb_download_file(req):
    data = SB.ghost(req["id"]) if req["id"] > 0 else None
    log(f"  DownloadFile id={req['id']} maxSize={req['maxSize']}: {len(data) if data else 'no ghost'}")
    if data is None or len(data) > req["maxSize"]:
        return {"data": b""}
    return {"data": data}


def sb_submit_xp(req):
    xuid, _ = _me()
    total, verdict, reason = SB.add_xp(xuid, req["xp"])
    if verdict == "ok":
        log(f"  SubmitXpScore +{req['xp']:g} -> total {total:g} user={xuid:#x}")
    else:
        log(f"  SubmitXpScore +{req['xp']:g} REJECTED ({reason}), total stays {total:g} user={xuid:#x}")
    return None


def sb_best(req):
    xuid, _ = _me()
    b = SB.best_of(xuid, req["scoreboardType"], req["scoreType"], req["trackId"], req["gameScoreboardId"])
    log(f"  GetBestScore board=({req['scoreboardType']},{req['scoreType']}) gsid={req['gameScoreboardId']} "
        f"track={req['trackId']} car={req['carId']}: "
        f"{'%.3f' % b['score'] if b else 'none'}")
    if not b:
        return {"bestScore": {"score": 0, "isClean": False}, "scoreFound": False}
    return {"bestScore": {"score": scoreboard.dbl_to_u64(b["score"]), "isClean": b["clean"]}, "scoreFound": True}


def sb_rival(req):
    xuid, _ = _me()
    r = SB.rival_for(xuid, req["scoreboardType"], req["scoreType"], req["trackId"], req["pivotId"],
                    req["rivalXuid"])
    res = xls.default_struct(SC, SC.call_struct("ForzaWebServicesScoreboardGetRival_Results"))
    if r is None:
        log("  GetRival: no rival")
        return res
    sd = res["rival"]["scoreData"]
    _score_row(r, sd)
    # your own ghost (you are #1, or picked yourself) pays nothing: otherwise racing it would farm credits
    sd["bounty"] = res["rival"]["bounty"] = 0 if r["xuid"] == xuid else SB.bounty_for(_bounty_track(req), r["position"])
    log(f"  GetRival board=({req['scoreboardType']},{req['scoreType']}) event={req['rivalEventId']} "
        f"track={req['trackId']} pivot={req['pivotId']} rivalXuid={req['rivalXuid']:#x}: {r['tag']} {r['score']:.3f}")
    return res


def _club_board_row(rank, club_id, xp, tmpl):
    """A GetRows row of a club board: the club's leader as the user, the club's XP as the score, the club in clubData."""
    r = _clone(tmpl)
    club = CLUBS.club(club_id)
    leader = CLUBS.leader(club_id)
    r["user"] = _owner(tmpl["user"], leader) if leader else r["user"]
    r.update({"position": rank, "rank": rank, "score": scoreboard.dbl_to_u64(xp), "submittedTime": scoreboard.iso(time.time())})
    r["clubData"].update({"clubId": club_id, "clubTag": club["tag"][:9], "clubName": club["name"][:49],
                          "memberCount": CLUBS.member_count(club_id), "clubPrivacy": club["privacy"]})
    return r


def club_board_rows(req, results):
    """The club leaderboards ("CLUB LEADERBOARDS / CLUB LEVEL"): GetRows with isClubBoard, one row per club by XP. view 0 = all clubs,
    1 = my club, 2 = friends' clubs (none) [view numbers as seen on the player boards]."""
    xuid, _ = _me()
    mine_id = CLUBS.member_info(xuid)[0]
    ladder = [(i + 1, cid, xp) for i, (cid, xp) in enumerate(CLUBS.ladder())]
    res = xls.default_struct(SC, SC.call_struct(results))
    tmpl = res["userRow"]
    if req["view"] == 1:
        page = [x for x in ladder if x[1] == mine_id]
    elif req["view"] == 2:
        page = []
    else:
        page = ladder
    start = max(1, req["startAt"])
    page = page[start - 1:start - 1 + max(1, min(200, req["maxResults"]))]
    res["rows"] = [_club_board_row(*x, tmpl) for x in page]
    mine = next((x for x in ladder if x[1] == mine_id), None)
    if mine:
        res["userRow"] = _club_board_row(*mine, tmpl)
    res["available"] = len(page) if req["view"] != 0 else len(ladder)
    log(f"  GetRows[club board] board=({req['scoreboardType']},{req['scoreType']}) view={req['view']}: {len(res['rows'])} clubs, "
        f"mine={'#%d' % mine[0] if mine else '-'}")
    return res


def sb_rows(req, results="ForzaWebServicesScoreboardGetRowsAnthem_Results"):
    if req.get("isClubBoard") and "clubData" in _elem_template(results, "rows"):
        return club_board_rows(req, results)
    xuid, _ = _me()
    page, mine, nreal, nfake = SB.rows(xuid, req["scoreboardType"], req["scoreType"], req["trackId"],
                                      max(1, req["startAt"]), min(200, max(0, req["maxResults"])), req["pivotId"])
    FRIENDS_VIEW = 2    # the leaderboard's "FILTER: FRIENDS" (GetRows view=2); we have no friends, so it is just you
    if req["view"] == FRIENDS_VIEW:
        page = [dict(mine, position=1)] if mine is not None else []
        mine = dict(mine, position=1) if mine is not None else None
    res = xls.default_struct(SC, SC.call_struct(results))
    tmpl = res["userRow"]
    res["rows"] = [_score_row(r, json.loads(json.dumps(tmpl))) for r in page]
    if mine is not None:
        res["userRow"] = _score_row(mine, res["userRow"])
    # `available` (GetRows) / `totalRowCount` (GetRowsAnthem) = size of the whole board. Left at 0 the
    # leaderboard page shows "RESULTS UNAVAILABLE" even with valid rows [inferred from the schema; verify in game].
    total = 1 if req["view"] == FRIENDS_VIEW and mine is not None else (0 if req["view"] == FRIENDS_VIEW else nreal + nfake)
    res["available"] = total
    if "totalRowCount" in res:
        res["totalRowCount"] = total
    log(f"  GetRows[{results[len('ForzaWebServicesScoreboard'):-8]}] board=({req['scoreboardType']},{req['scoreType']}) view={req['view']} "
        f"track={req['trackId']} pivot={req['pivotId']}: {len(page)} rows ({nreal} real, {nfake} fake), "
        f"you={'#%d' % mine['position'] if mine else '-'}")
    return res


MONTHLY_CATEGORY = 4          # the Monthly Rivals page lists GetTimeTrials entries of category 4 [C]; other categories are unmapped
RIV = None                    # rivals.RivalCatalog: seeded monthly rotation of circuit events with restriction themes
CLASS_NAMES = ("D", "C", "B", "A", "S1", "S2")
TOP_CLASS = 5          # classes 6 (X) and 7 (R) exist in the game DB but the client reports them for maxed cars; relayed as S2 [display choice]


def event_info(event_id):
    return RIV.by_id(event_id) if RIV is not None else None


def event_cars(event_id):
    """Cars a rival event's restriction allows (the fake rivals drive these, so a D Class Dash rival is in a D class car)."""
    e = event_info(event_id)
    return RIV.cars_for(e["attrs"]) if e else None


def _bounty_track(req):
    e = event_info(req["rivalEventId"])
    return e["track"] if e else req["trackId"]


def game_options_xml(e):
    """`<GameOptions>` for a rival event in the game's own format (GameTunableSettings.zip: CareerRaceModes.xml "Laps Race" and
    GameOptionPresets.xml). UsePFXCheckpoints / anti-griefing are what the career Laps Race mode uses; the confirmed effect so far is
    the lap count and the class limit. Other restriction attributes (BodyFamilyId, MakeId, ModelId, YearMin/Max, DriveTypeId, CountryId)
    are named after the exe's option table [inferred, being tested]. The track is not part of it (GetTimeTrials.trackId)."""
    attrs = "".join(f' {k}="{v}"' for k, v in e["attrs"].items())
    buckets = f"<Buckets><Bucket><CarRestrictions{attrs}/></Bucket></Buckets>" if e["attrs"] else ""
    tod = f' TimeOfDay="{rivals.tod_seconds(e["tod"])}"' if e.get("tod") is not None else ""   # seconds since midnight [C]
    weather = ""
    if e.get("weather") is not None:
        # DirectWeatherControlSettings: attribute names from the exe's strings (ControlWeather, ControlAtmosphericPreset,
        # AtmosphericPresetIndex, Control/Desired AtmosphericWetness | EnvironmentalWetness | PrecipitationLevel) [inferred, under test]
        weather = ('<DirectWeatherControlSettings ControlWeather="true" ControlAtmosphericPreset="true" '
                   f'AtmosphericPresetIndex="{e["weather"]}" ControlEnvironmentalWetness="true" '
                   f'DesiredEnvironmentalWetness="{e["wetness"]}" ControlAtmosphericWetness="true" '
                   f'DesiredAtmosphericWetness="{e["wetness"]}" ControlPrecipitationLevel="true" '
                   f'DesiredPrecipitationLevel="{e["precip"]}"/>')
    return ('<GameOptions UiRacificationMode="Race" DrivatarTrainingDisabled="false" UseOnlineAntiGriefing="true" '
            f'UsePFXCheckpoints="true" PositionCPFlaresFromCentre="false" HasTraffic="false"{tod}>'
            f'{buckets}<StartingGridOptions GridOrderingType="Random" Ascending="1"/>'
            f'<EndConditionOptions Type="NumLaps" NumLaps="{e["laps"]}"/>'
            '<ScoringOptions Type="TimeBased" TargetScoreEnabled="1" BiggerIsBetter="0" FinishingPlaceRelevant="true"/>'
            f'{weather}</GameOptions>')


def hopper_game_options(req):
    e = event_info(req["hopperId"])
    if e is None:
        log(f"  GetGameOptions hopper={req['hopperId']} round={req['roundNumber']}: not a rival event, empty")
        return {"gameOptionsXml": b""}
    xml = game_options_xml(e).encode("utf-8")
    log(f"  GetGameOptions rival event {e['id']} ({e['kind']} {e['attrs']}): {e['laps']} lap(s) ({len(xml)} bytes)")
    return {"gameOptionsXml": xml[:max(0, req["optionXmlMaxLength"])]}


def sb_time_trials(req):
    """The rival events listed under Rivals > Monthly (see rivals.py). The race uses the event's trackId; laps and the car
    restriction come from Hopper.GetGameOptions, keyed on hopperId = event id."""
    announce_rotation()
    now = time.time()
    tts = []
    for k, e in enumerate(RIV.events()):
        tts.append({"id": e["id"], "category": MONTHLY_CATEGORY, "scoreType": 1, "name": e["name"],
                    "description": e["description"], "notRegisteredMessage": "", "trackId": e["track"],
                    "trackName": e["trackname"], "hopperId": e["id"], "gameOptionsIndex": 0,
                    "startDate": scoreboard.iso(now - 86400), "endDate": scoreboard.iso(now + 86400 * 30),
                    "isUserRegistered": True, "isNew": k < 3, "isFeatured": k == 0, "isActive": True})
    n = req["maxTimeTrials"]
    log(f"  GetTimeTrials max={n}: serving {min(n, len(tts))} rival events")
    return {"timeTrials": tts[:max(n, 0) or len(tts)]}


# ---------------------------------------------------------------- permissions

def all_permissions(n=255):
    return [{"methodId": i, "tier": 0, "isEnabled": True, "access": 0} for i in range(1, n + 1)]


def permissions_table(req):
    """Every feature enabled. The client looks features up by methodId (FUN_14126c2b0 -> FUN_140f09930):
    a feature missing from the table reads as 4 ("locked", e.g. the "server not available" popup when
    downloading a gifted car), an entry with access 0 reads as allowed."""
    n = min(req["maxPermissions"] if req else 0, 255) or 255
    log(f"  GetPermissionsTable max={req['maxPermissions'] if req else None}: enabling features 1..{n}")
    return {"permissions": all_permissions(n),
            "featureAreasBanMask": 0, "userFlags": 0}


def economy_settings(req):
    """Credit/XP rates for ForzaEconomyModule. Race credits are route metres * 0.001 * creditPerKm (FUN_14054a110,
    called from FUN_140614250/140614760 [confirmed]), XP is max(xpPerKm * km, minimumFinishLineXp) (FUN_14054a140), so the
    all-zero default paid 0 for everything. creditPerKm 1000 matches CreditsPerKm in the race templates
    (EntityModel/Templates/templates_races.xml) [inferred]; the other values were Turn 10's and are guesses [guess]."""
    r = xls.default_struct(SC, SC.call_struct("ForzaUserGetEconomySettings_Results"))
    r["settings"].update({
        "creditDestinationBonus": 2000, "creditDestinationFalloff": 0, "xpPerKmOffline": 100, "xpPerKmOnline": 100,
        "creditPerKm": 1000, "minimumFinishLineXp": 100, "tombolaPrizeScale": 1.0, "promoBonusScale": 1.0,
        "wagerRaceCost": 1000, "enableTokens": False, "enableXboxMusic": False})
    log("  GetEconomySettings -> non-zero guess values")
    return r


# ---------------------------------------------------------------- gifting (Message Center category 5)

class XlsError(Exception):
    def __init__(self, text, code=1):
        super().__init__(text)
        self.text, self.code = text, code


def _gift(giftid, types):
    """The unclaimed gift notification with this GiftId, for the requesting player."""
    for n in _visible():
        if n["type"] in types and n.get("params", {}).get("GiftId") == giftid:
            if n.get("claimed"):
                raise XlsError("gift already retrieved")
            return n
    raise XlsError("no such gift")


def _claim(n):
    """Atomic exactly-once claim for the requesting player; the check in _gift() is only the fast path."""
    if not NOTIF.claim(getattr(CTX, "xuid", 0), n["id"]):
        raise XlsError("gift already retrieved")


def gift_credits(req):
    n = _gift(req["giftId"], (0x8500,))
    _claim(n)
    p = n["params"]
    log(f"  RetrieveCreditsGift #{req['giftId']}: {p.get('Amount', 0)} CR from {p.get('SenderGamerTag', '')!r}")
    return {"sendingXuid": p.get("SenderXuid", 0), "creditAmount": p.get("Amount", 0)}


def gift_car(req):
    n = _gift(req["giftId"], (0x8502,))
    p = n["params"]
    car_id = n.get("carId") or p.get("ModelId", 0)
    car = CARS.car_struct(car_id)
    if car is None:
        raise XlsError(f"unknown carId {car_id}")
    _claim(n)
    res = xls.default_struct(SC, SC.call_struct("ForzaGiftingRetrieveCarGift_Results"))
    res["car"].update(car)
    res["giftListing"].update({"giftId": req["giftId"], "senderXuid": p.get("SenderXuid", 0),
                               "reason": p.get("Reason", 0)})
    res["carHistory"]["base"]["originalOwner"] = p.get("SenderGamerTag", "")[:31]
    log(f"  RetrieveCarGift #{req['giftId']}: carId {car_id} (make {car['make']}, {car['year']}) "
        f"from {p.get('SenderGamerTag', '')!r}; blob sizes asked {req['liveryDetailsBufferSize']}/"
        f"{req['partsInstalledBufferSize']}/{req['partsInTrunkBufferSize']}")
    return res



# ---------------------------------------------------------------- user content: paint jobs, tunes (server/ugc.py)

_TEMPLATES = {}


def _elem_template(result_struct, field):
    """A default element of an array field of a result struct (every field present, ready to fill)."""
    key = (result_struct, field)
    if key not in _TEMPLATES:
        f = next(x for x in SC.call_struct(result_struct) if x.name == field)
        el = SC.field(f.elem)
        _TEMPLATES[key] = xls.default_struct(SC, SC.fields(el.nested))
    return _clone(_TEMPLATES[key])


def _clone(v):
    if isinstance(v, dict):
        return {k: _clone(x) for k, x in v.items()}
    if isinstance(v, list):
        return [_clone(x) for x in v]
    return v


def _owner(tmpl, xuid):
    """The UserInfo struct of a player (creator of a design, club member, ...), with the club tag shown in front of the gamertag."""
    o = _clone(tmpl)
    club_id, club_tag, role = CLUBS.member_info(xuid) if CLUBS else (0, "", 0)
    o.update({"qwXuid": xuid, "wzGamerTag": (TAGS.get(xuid) or _tag_from_db(xuid))[:31],
              "currentCareerLevel": LEVELS.get(xuid, 1), "clubId": club_id, "clubTag": club_tag[:9], "roleInClub": clubs.wire_role(role)})
    return o


def _tag_from_db(xuid):
    r = DB.one("SELECT gamertag FROM players WHERE xuid=?", (xuid,))
    return r["gamertag"] if r else ""


def _stamp(t):
    return scoreboard.iso(t)


KEYWORDS = ("Racing", "Rally", "Drift", "Retro", "Classic", "Anime", "Sponsored", "Clean", "Camo", "Flames",
            "Stripes", "Cartoon", "Movie", "Game", "Sports", "Original", "Tribute", "Police", "Tuner", "Art")
TUNE_KEYWORDS = ("Road Racing", "Street", "Drift", "Drag", "Rally", "Off-Road", "Grip", "Top Speed", "Balanced", "Stable",
                 "Loose", "Wet Weather", "Beginner", "Expert", "Stock Plus", "Clean Racing")
KEYWORDS_BY_FILE_TYPE = {0: TUNE_KEYWORDS}      # the client asks per file type (0..4): 0 = tune and 2 = livery [both confirmed from Create calls];
                                                # 1, 3 and 4 (other content kinds, vinyl groups among them) keep the generic list for now
FILE_KIND = {}          # the client's file type number -> kind, learned from Create calls


def ugc_keywords(req):
    words = KEYWORDS_BY_FILE_TYPE.get(req["fileType"], KEYWORDS)
    return {"keywords": [{"Keyword": k, "KeywordId": i + 1, "Bucket": 0 if i < 10 else 1} for i, k in enumerate(words)][:req["maxKeywordResults"]]}


def _ordinal(f):
    """The `Ordinal` of a storefront item is the car id: the client's MakeName / ModelName / Year bindings read it (u16 at +0x64298
    of the item, FUN_140c48f70 / FUN_140c49150) and show "DLC Car" when it is 0 or unknown."""
    return (f.get("car_id") or 0) & 0xFFFF


def _light(f, tmpl):
    r = _clone(tmpl)
    r.update({"Id": f["id"], "PopularityRating": min(f["likes"], 5), "Creator": _owner(tmpl["Creator"], f["owner"]),
              "IsCreatorFollowed": False, "SubmissionTime": _stamp(f["created"]), "Ordinal": _ordinal(f), "MethodObtained": 0})
    return r


def _heavy(f, tmpl):
    r = _clone(tmpl)
    r.update({"Id": f["id"], "Type": f["type"], "Title": f["title"], "Description": f["description"],
              "SubmissionTime": _stamp(f["created"]), "Owner": _owner(tmpl["Owner"], f["owner"]),
              "PayloadSize": f["payload_size"], "PreviewPayload": bytes(f["preview"] or b"")[:409600], "Ordinal": _ordinal(f),
              "IsUnderReview": False, "VersionedContentId": f["versioned_id"] or 0,
              "PopularityRating": min(f["likes"], 5), "IsCreatorFollowed": False})
    return r


def content_upload(req):
    xuid, _ = _me()
    data = bytes(req["uploadData"])
    try:
        got = UGC.blob_write(req["contentId"], req["payloadOffset"], data, owner=xuid)
    except ugc.LimitError as e:
        raise XlsError(str(e))
    log(f"  Content.UploadContent id={req['contentId']} @{req['payloadOffset']} +{len(data)} of {req['payloadTotalSize']}: "
        f"{'have %d' % got if got is not None else 'unknown id (or not yours)'}")
    if got is None:
        raise XlsError(f"no such content {req['contentId']}")
    return {"payloadUploadedSize": got}


def content_download(req):
    r = UGC.blob_read(req["contentId"], req["payloadOffset"], req["requestedSize"])
    if r is None:
        raise XlsError(f"no such content {req['contentId']}")
    chunk, last, total = r
    log(f"  Content.DownloadContent id={req['contentId']} @{req['payloadOffset']} +{len(chunk)}/{total} last={last}")
    return {"isLastBuffer": last, "downloadData": chunk, "totalSize": total}


def large_file_create(req):
    xuid, _ = _me()
    try:
        bid = UGC.blob_create(xuid, req["userFileName"], req["payloadSize"])
    except ugc.LimitError as e:
        raise XlsError(str(e))
    log(f"  LargeFileUpload.Create name={req['userFileName']!r} size={req['payloadSize']} -> content id {bid}")
    return {"contentFileId": bid}


def profile_update_livery(body):
    """OnlineProfile.UpdateLivery: the real upload of a design (layout in ugc.parse_livery_upload). It is kept under (owner, name) until
    CreateVersionedLivery names it."""
    xuid, _ = _me()
    up = ugc.parse_livery_upload(body)
    if up is None:
        log(f"  UpdateLivery: unparsable body ({len(body)}B)")
        return b""
    try:
        bid = UGC.blob_create(xuid, up["name"], len(body))
        UGC.blob_write(bid, 0, body, owner=xuid)
    except ugc.LimitError as e:
        log(f"  UpdateLivery refused: {e}")
        return b""
    log(f"  UpdateLivery {up['name']!r} car={up['car_id']} layers={up['layers']} container={len(up['container'])}B "
        f"({len(body)}B on the wire) -> kept as blob #{bid}")
    return b""


BIG_THUMB, SMALL_THUMB = (816, 459), (192, 108)       # design thumbnails as the game writes them (RGBA PNG)


_PLACEHOLDER = {}


def _placeholder_thumbs():
    """(big, small) plain mid-grey RGBA PNGs at the sizes the game writes, built once."""
    if not _PLACEHOLDER:
        for key, (w, h) in (("big", BIG_THUMB), ("small", SMALL_THUMB)):
            row = bytes((128, 128, 128, 255)) * w
            _PLACEHOLDER[key] = pngtool.encode(w, h, 4, [row] * h)
    return _PLACEHOLDER["big"], _PLACEHOLDER["small"]


def livery_thumbs(preview):
    """(big, small) design thumbnails from the Storefront preview (an 816x459 PNG): the client saves both with a downloaded design and
    crashes in LiveryAsyncSaveTask without them / with the wrong size (2026-10-18)."""
    try:
        if preview[:4] != b"\x89PNG":
            return b"", b""
        w, h = pngtool.decode(preview)[:2]                  # dimensions are enforced inside the decoder
        if (w, h) == BIG_THUMB:
            big = preview
        else:
            big = pngtool.downscale(preview, *BIG_THUMB)
        return big, pngtool.downscale(preview, *SMALL_THUMB)
    except (ValueError, struct.error, Exception) as e:      # noqa: BLE001 - never fail a share over a thumbnail
        log(f"  thumbnail generation failed: {e!r}")
        return b"", b""


def cv_create_livery(req):
    xuid, _ = _me()
    vid = UGC.versioned_create("livery", xuid, req["name"], req["carId"], bytes(req["LiveryThumbnail"]))
    blob = UGC.blob_by_name(xuid, req["name"])
    note = "no matching UpdateLivery upload"
    if blob:
        up = ugc.parse_livery_upload(blob[1])
        if up and up["container"]:
            UGC.versioned_set_data(vid, up["payload"])
            with DB.tx() as c:
                c.execute("UPDATE ugc_versioned SET layers=?, car_id=COALESCE(NULLIF(car_id, 0), ?) WHERE id=?", (up["layers"], up["car_id"], vid))
            note = f"data {len(up['payload'])}B (inflates to {len(up['container'])}B), {up['layers']} layers"
    log(f"  CreateVersionedLivery {req['name']!r} car={req['carId']} thumb={len(req['LiveryThumbnail'])}B -> versioned {vid} ({note})")
    return {"versionedLiveryId": vid}


def cv_create_layergroup(req):
    xuid, _ = _me()
    vid = UGC.versioned_create("layergroup", xuid, req["name"], None, bytes(req["thumbnail"]))
    log(f"  CreateVersionedLayerGroup {req['name']!r} thumb={len(req['thumbnail'])}B -> versioned {vid}")
    return {"versionedLayerGroupId": vid}


def cv_create_tune(req):
    xuid, _ = _me()
    vid = UGC.versioned_create("tune", xuid, req["name"], req["carId"])
    log(f"  CreateVersionedTune {req['name']!r} car={req['carId']} -> versioned {vid}")
    return {"versionedTuneId": vid}


def cv_create_tune_structs(req):
    xuid, _ = _me()
    vid = UGC.versioned_create("tune", xuid, "", req["carId"], parts=req["installedParts"], tuning=req["tuning"])
    log(f"  CreateVersionedTuneFromStructs car={req['carId']} -> versioned {vid}")
    return {"versionedTuneId": vid}


def _layer_count(n):
    """numberOfLayers is an unsigned 16-bit field on the wire; a design stored with -1 (count unknown) crashed the encoder, so the
    client got a server error instead of the design (199 times on 2026-10-20)."""
    return max(0, min(0xFFFF, int(n or 0)))


def _livery_layers(v):
    """The layer count of a stored design. Shares overwrote it with the client's -1 ("unknown"); the real count is in the UpdateLivery
    upload the design came from (same owner and name), so it is read from there. 0 only if that upload is gone."""
    if v["layers"] is not None and v["layers"] >= 0:
        return _layer_count(v["layers"])
    blob = UGC.blob_by_name(v["creator"], v["name"]) if v["name"] else None
    up = ugc.parse_livery_upload(blob[1]) if blob else None
    return _layer_count(up["layers"] if up else 0)


SHIM_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?(-debug)?$")
PLACEMENT_FIX_RC = 3            # first release candidate whose shim turns the unsupported livery texture into an ordinary one (patch_placement_fallback)
LIVERY_GATE_BYTES = 1100        # a design's inflated container: about 1034 bytes for an empty design, 1.5 KB and up with layers


def shim_has_placement_fix(version):
    """True when the client's shim (X-FH2MP-Shim) carries patch_placement_fallback: from 1.0.0-rc3 on, any later release, and our own debug
    builds. Without it the game stops ("Azimuth stopped: CreatePlacementTexture2D ...") when a car wearing a layered livery appears."""
    m = SHIM_RE.match((version or "").strip())
    if not m:
        return False
    major, minor, patch, rc, debug = m.groups()
    if debug:
        return True
    if rc is None:
        return (int(major), int(minor), int(patch)) >= (1, 0, 0)
    return (int(major), int(minor), int(patch)) > (1, 0, 0) or int(rc) >= PLACEMENT_FIX_RC


def design_has_layers(stored_layers, data):
    """A design with layers, judged by the stored count and, because the count is unreliable (shares stored -1), by the size of the data
    (`data` as UGC.versioned_get returns it: already unpacked)."""
    if stored_layers and stored_layers > 0:
        return True
    if not data:
        return False
    try:
        container = ugc.livery_chunk(ugc.strip_livery_prefix(bytes(data)))
    except Exception:           # noqa: BLE001 - anything unreadable counts as "has layers": hiding it is the safe side
        return True
    return container is None or len(container) > LIVERY_GATE_BYTES


def cv_get_livery(req):
    v = UGC.versioned_get(req["versionedLiveryId"])
    if v is None or v["kind"] != "livery":
        raise XlsError(f"no such livery {req['versionedLiveryId']}")
    xuid, _ = _me()
    if (not os.environ.get("FH2MP_LIVERY_GATE_OFF") and v["creator"] != xuid and not shim_has_placement_fix(getattr(CTX, "shim", ""))
            and design_has_layers(v["layers"], v["data"])):
        # A car wearing a layered design makes the game build a 1024x1024 11-mip texture that Azimuth cannot place; clients without the shim
        # fallback stop (prod 2026-10-20: every wipe lines up with all the other clients fetching a layered design, e.g. 581 with 26 layers).
        # They get the answer designs with layers=-1 always got (an error): the car shows without the design, nothing crashes.
        log(f"  RetrieveVersionedLivery {v['id']}: withheld from a client without the texture fix (shim {getattr(CTX, 'shim', '') or 'unknown'})")
        raise XlsError(f"livery {v['id']} is not available to this client")
    res = xls.default_struct(SC, SC.call_struct("ForzaContentVersioningRetrieveVersionedLiveryById_Results"))
    d = res["versionedLiveryData"]
    data = ugc.strip_livery_prefix(bytes(v["data"] or b""))        # the client's own compression-stream blob, served as uploaded
    d.update({"profileId": 0, "carId": v["car_id"] or 0, "numberOfLayers": _livery_layers(v), "creatorXuid": v["creator"],
              "data": data, "liveryThumbnail": bytes(v["thumb"] or b""),
              "liveryThumbnailSmall": bytes(v["thumb_small"] or b"")})
    if not d["liveryThumbnail"]:        # a design shared before thumbnails were generated: derive them from its storefront preview now
        f = DB.one("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", (v["id"],))
        pv = (UGC.file_get(f["id"]) or {}).get("preview", b"") if f else b""
        big, small = livery_thumbs(pv) if pv else (b"", b"")
        if big:
            UGC.versioned_set_thumbs(v["id"], big, small)
            d["liveryThumbnail"], d["liveryThumbnailSmall"] = big, small
    if not d["liveryThumbnail"] and not os.environ.get("FH2MP_NO_THUMB_PLACEHOLDER"):
        # A design that was saved but never shared has no thumbnail. Other players' games fetch it when its owner joins and the game
        # expects the thumbnails to be there (a design with layers and no thumbnail went with a lobby-wide client crash on prod,
        # 2026-10-20: the owner removed the livery and the crashes stopped). Serve a plain thumbnail of the right size instead of nothing.
        d["liveryThumbnail"], d["liveryThumbnailSmall"] = _placeholder_thumbs()
    log(f"  RetrieveVersionedLivery {v['id']}: {len(d['data'])}B data, {d['numberOfLayers']} layers, thumb {len(d['liveryThumbnail'])}B")
    return res


def cv_get_layergroup(req):
    v = UGC.versioned_get(req["versionedLayerGroupId"])
    if v is None or v["kind"] != "layergroup":
        raise XlsError(f"no such layer group {req['versionedLayerGroupId']}")
    res = xls.default_struct(SC, SC.call_struct("ForzaContentVersioningRetrieveVersionedLayerGroupById_Results"))
    res["versionedLayerGroupData"].update({"name": v["name"], "numberOfLayers": _layer_count(v["layers"]), "creatorXuid": v["creator"],
                                           "data": bytes(v["data"] or b""), "thumbnailData": bytes(v["thumb"] or b"")})
    return res


def cv_get_tune(req):
    v = UGC.versioned_get(req["versionedTuneId"])
    if v is None or v["kind"] != "tune":
        raise XlsError(f"no such tune {req['versionedTuneId']}")
    res = xls.default_struct(SC, SC.call_struct("ForzaContentVersioningRetrieveVersionedTuneById_Results"))
    d = res["versionedTuneData"]
    d.update({"CarId": v["car_id"] or 0, "CreatorName": TAGS.get(v["creator"]) or _tag_from_db(v["creator"]), "CreatorXuid": v["creator"]})
    if v["parts"]:
        d["InstalledParts"] = json.loads(v["parts"])
    if v["tuning"]:
        d["Tuning"] = json.loads(v["tuning"])
    return res


def _cv_ref(field, delta):
    def h(req):
        UGC.versioned_ref(req[field], delta)
        return None
    return h


def _create_file(kind, header, versioned_id, car_id, file_name, preview, meta=None):
    xuid, _ = _me()
    blob = UGC.blob_by_name(xuid, file_name) if file_name else None
    v = UGC.versioned_get(versioned_id) if versioned_id else None
    if v is not None and v["creator"] != xuid:
        log(f"  {kind} share refused: versioned item {versioned_id} belongs to {v['creator']}, not to {xuid}")
        raise XlsError(f"versioned item {versioned_id} is not yours")
    car_id = car_id or (v["car_id"] if v else None)         # a tune's listing carries make/model/class only; its car is on the versioned item
    if kind == "livery" and v is not None:
        if blob and not v["data"]:
            UGC.versioned_set_data(versioned_id, blob[1], creator=xuid)
        if header["Quantity"] and header["Quantity"] > 0:      # the client sends -1 when it does not know; that must not replace a real count
            UGC.versioned_set_layers(versioned_id, header["Quantity"], xuid)
    m = {}
    if car_id:
        r = DB.one("SELECT make_id, model_id, class_id FROM game.v_cars WHERE id=?", (car_id,))
        if r:
            m = {"make_id": r["make_id"], "model_id": r["model_id"], "class_id": r["class_id"]}
    m.update(meta or {})
    FILE_KIND[header["Type"]] = kind
    if kind == "livery" and v is not None and bytes(preview)[:4] == b"\x89PNG":
        big, small = livery_thumbs(bytes(preview))
        if big:
            UGC.versioned_set_thumbs(versioned_id, big, small, creator=xuid)
    fid = UGC.file_create(kind, xuid, header["Title"], header["Description"], header["Type"], versioned_id, header["PayloadSize"],
                          bytes(preview), blob[0] if blob else None, car_id, kw1=header["KeywordOne"], kw2=header["KeywordTwo"], **m)
    log(f"  {kind} shared: file #{fid} {header['Title']!r} type={header['Type']} versioned={versioned_id} "
        f"payload={header['PayloadSize']} blob={'#%d' % blob[0] if blob else None} preview={len(preview)}B")
    return fid


def store_livery_create(req):
    d = req["liveryDescription"]
    fid = _create_file("livery", req["fileHeader"], d["base"]["VersionedLiveryId"], d["carId"], req["fileName"], req["previewData"])
    return {"contentFileId": fid}


def store_tuning_create(req):
    d = req["tuningDescription"]["base"]
    meta = {"make_id": d["MakeId"], "model_id": d["ModelId"], "class_id": d["ClassId"], "env": d["EnvironmentId"],
            "track": d["TrackId"], "pi": d["PerformanceIndex"]}
    fid = _create_file("tune", req["fileHeader"], d["VersionedTuneId"], None, req["fileName"], req["previewData"], meta)
    return {"contentFileId": fid}


def _search_files(kind, results_struct, label, req):
    p = req["searchParameters"]
    files = UGC.search(kind, p["MakeId"], p["ModelId"], p["ClassId"], p["KeywordOneId"], p["KeywordTwoId"], p["EnvironmentId"],
                       p["TrackId"], p["NumberOfLayers"], p["GamerTag"], p["ManualSearchKeywords"], min(max(req["maxFiles"], 0), 100) or 50)
    tmpl = _elem_template(results_struct, "searchResults")
    log(f"  {label} make={p['MakeId']} model={p['ModelId']} class={p['ClassId']} kw={p['KeywordOneId']}/{p['KeywordTwoId']} "
        f"env={p['EnvironmentId']} track={p['TrackId']} tag={p['GamerTag']!r} text={p['ManualSearchKeywords']!r}: {len(files)} found")
    return {"searchResults": [_light(f, tmpl) for f in files]}


def store_search_liveries(req):
    return _search_files("livery", "ForzaStorefrontSearchLiveryFiles_Results", "SearchLiveryFiles", req)


def store_search_tunes(req):
    return _search_files("tune", "ForzaStorefrontSearchTuningFiles_Results", "SearchTuningFiles", req)


def store_heavy(req):
    ids = req["fileIds"][:max(0, req["fileCount"])] if req["fileCount"] else req["fileIds"]
    files = UGC.files(ids)
    tmpl = _elem_template("ForzaStorefrontGetFileHeavyWeightData_Results", "fileData")
    for f in files:
        UGC.bump(f["id"], "downloads")
    log(f"  GetFileHeavyWeightData {list(ids)}: {len(files)} found")
    return {"fileData": [_heavy(f, tmpl) for f in files]}


def store_my_files(req):
    xuid, _ = _me()
    files = UGC.mine(xuid, req["fileType"], min(max(req["maxFiles"], 0), 500) or 100)
    tmpl = _elem_template("ForzaStorefrontGetMyFileList_Results", "files")
    out = []
    for f in files:
        r = _clone(tmpl)
        r.update({"Id": f["id"], "Title": f["title"], "Description": f["description"], "SubmissionTime": _stamp(f["created"]),
                  "IsUnderReview": False, "VersionedContentId": f["versioned_id"] or 0, "PopularityRating": min(f["likes"], 5),
                  "Ordinal": _ordinal(f),
                  "DownloadCount": f["downloads"], "LikeCount": f["likes"], "UseCount": f["uses"]})
        out.append(r)
    log(f"  GetMyFileList type={req['fileType']}: {len(out)} files")
    return {"files": out}


def store_if_shared(req):
    r = DB.one("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", (req["versionedItemId"],))
    res = xls.default_struct(SC, SC.call_struct("ForzaStorefrontGetFileDataIfShared_Results"))
    if r:
        f = UGC.file_get(r["id"])
        tmpl = xls.default_struct(SC, SC.call_struct("ForzaStorefrontGetFileDataIfShared_Results"))["storefrontData"]
        res["found"] = True
        res["storefrontData"] = _light(f, tmpl)
    return res


def _bump_handler(column, field="fileId"):
    def h(req):
        UGC.bump(req[field], column)
        return None
    return h


def store_batch_views(req):
    for i in req["fileIds"][:max(0, req["fileCount"])]:
        UGC.bump(i, "views")
    return None


def store_remove(req):
    xuid, _ = _me()
    log(f"  RemoveFile #{req['fileId']}: {'removed' if UGC.remove(req['fileId'], xuid) else 'not yours / unknown'}")
    return None


def store_retitle(req):
    xuid, _ = _me()
    UGC.retitle(req["fileId"], xuid, title=req["newTitle"])
    return None


def store_redescribe(req):
    xuid, _ = _me()
    UGC.retitle(req["fileId"], xuid, description=req["newDescription"])
    return None


def store_featured_liveries(req):
    files, seen = [], set()
    for f in UGC.search("livery", limit=200):
        if f["car_id"] not in seen:
            seen.add(f["car_id"])
            files.append(f)
        if len(files) >= max(1, req["numCars"]):
            break
    tmpl = _elem_template("ForzaStorefrontGetFeaturedLiveriesForRandomCars_Results", "liveryData")
    return {"liveryData": [_heavy(UGC.file_get(f["id"]), tmpl) for f in files]}      # file_get unpacks the preview


def store_drivatar_liveries(req):
    rows = DB.query("SELECT id, versioned_id FROM ugc_files WHERE kind='livery' AND car_id=? AND removed=0 AND versioned_id IS NOT NULL "
                    "ORDER BY likes DESC, id DESC LIMIT ?", (req["carId"], max(1, req["maxContentResults"])))
    return {"drivatarLiveries": [{"Id": r["id"], "VersionedContentId": r["versioned_id"]} for r in rows]}


def store_curated(req):
    """Storefront.GetCuratedContent (the "Recommended Designs" page): community files of the asked type for the player's car."""
    p = req["curationParameters"]
    files = UGC.curated(p["ContentType"], p["CarId"], min(max(req["maxContentResults"], 1), 100))
    tmpl = _elem_template("ForzaStorefrontGetCuratedContent_Results", "curatedContent")
    log(f"  GetCuratedContent type={p['ContentType']} car={p['CarId']} track={p['TrackId']} pi={p['PiTargetValue']}: {len(files)} found")
    return {"curatedContent": [_light(f, tmpl) for f in files]}


def store_curated_data(req):
    f = UGC.file_get(req["contentId"])
    if f is None:
        raise XlsError(f"no such content {req['contentId']}")
    v = UGC.versioned_get(f["versioned_id"]) if f["versioned_id"] else None
    res = xls.default_struct(SC, SC.call_struct("ForzaStorefrontGetCuratedContentData_Results"))
    res["contentData"].update({"Id": f["id"], "PreviewPayload": bytes(f["preview"] or b"")[:409600],
                               "Payload": bytes((v or {}).get("data") or b"")[:1048576]})
    log(f"  GetCuratedContentData #{f['id']}: payload {len(res['contentData']['Payload'])}B, preview {len(res['contentData']['PreviewPayload'])}B")
    return res


def store_noop(req):
    return None


# ---------------------------------------------------------------- Car Clubs (server/clubs.py)

def _club_errors(fn):
    """A ClubError becomes the Xls error result the client maps to one of its club messages."""
    def wrapped(req):
        try:
            return fn(req)
        except clubs.ClubError as e:
            log(f"  club: {e.text} (error code {e.code:#x})")
            raise XlsError(e.text, e.code)
    wrapped.__name__ = fn.__name__
    return wrapped


def _keyword(tmpl, kid):
    k = clubs.KEYWORD_BY_ID.get(kid)
    t = _clone(tmpl)
    if k:
        t.update(k)
    return t


def _club_struct(tmpl, club):
    """A club description in any of the result shapes (details, full info, search rows); fields a shape lacks are skipped."""
    cid = club["id"]
    out = _clone(tmpl)
    user = lambda x: _owner(tmpl["clubLeader"], x)
    values = {
        "clubId": cid, "clubTag": club["tag"][:9], "clubName": club["name"][:49], "clubDescription": club["description"][:400],
        "isOpenForMembership": club["privacy"] == clubs.PRIVACY_OPEN, "creationDate": _stamp(club["created"]),
        "carCount": 0, "memberCount": CLUBS.member_count(cid), "clubLeader": user(CLUBS.leader(cid)) if CLUBS.leader(cid) else tmpl["clubLeader"],
        "clubChampion": user(CLUBS.champion(cid)) if CLUBS.champion(cid) else tmpl["clubChampion"],
        "clubPrivacy": club["privacy"], "clubXP": CLUBS.xp(cid), "clubXPRank": CLUBS.xp_rank(cid),
    }
    for k, v in values.items():
        if k in out:
            out[k] = v
    for field, col in (("regionKeyword", "region"), ("attitudeKeyword", "attitude"), ("interestKeyword", "interest")):
        if field in out:
            out[field] = _keyword(tmpl[field], club[col])
    return out


def _club_list(result_struct, list_field, rows, total_field="totalClubs"):
    el = _elem_template(result_struct, list_field)
    out = {list_field: [_club_struct(el, r) for r in rows]}
    if any(f.name == total_field for f in SC.call_struct(result_struct)):
        out[total_field] = CLUBS.total()
    return out


@_club_errors
def club_details(req):
    club = CLUBS.club(req["clubId"])
    if not club:
        raise clubs.ClubError("club not found", clubs.CODES["not_found"])
    res = xls.default_struct(SC, SC.call_struct("ForzaCarClubGetClubDetails_Results"))
    res["clubDetails"] = _club_struct(res["clubDetails"], club)
    return res


@_club_errors
def club_full_info(req):
    club = CLUBS.club(req["clubId"])
    if not club:
        raise clubs.ClubError("club not found", clubs.CODES["not_found"])
    res = xls.default_struct(SC, SC.call_struct("ForzaCarClubGetClubFullInfo_Results"))
    res["clubDetails"] = _club_struct(res["clubDetails"], club)
    leader_tmpl = _elem_template("ForzaCarClubGetClubFullInfo_Results", "leaders")
    res["leaders"] = [_owner(leader_tmpl, x) for x in CLUBS.leaders(club["id"], max(1, req["maxLeaders"]))]
    return res


def club_keywords(req):
    return {"keywords": clubs.keyword_list()[:max(1, req["maxKeywordResults"])]}


def club_recommended(req):
    rows = CLUBS.recommended(getattr(CTX, "xuid", 0), req["maxResults"])
    log(f"  GetRecommendedClubs: {len(rows)}")
    return _club_list("ForzaCarClubGetRecommendedClubs_Results", "clubDetailsList", rows)


def club_friends(req):
    return _club_list("ForzaCarClubGetFriendsClubs_Results", "friendsClubDetailsList", [])      # no friends list yet


def club_search(req):
    p = req["clubSearchParams"]
    rows = CLUBS.search(p["Privacy"], _kw(p["RegionKeywordId"]), _kw(p["AttitudeKeywordId"]), _kw(p["InterestKeywordId"]), p["Activity"],
                        limit=req["maxResults"])
    log(f"  SearchForClubs privacy={p['Privacy']} region={p['RegionKeywordId']} attitude={p['AttitudeKeywordId']} "
        f"interest={p['InterestKeywordId']} activity={p['Activity']} friends={p['FriendsOnly']}: {len(rows)}")
    return _club_list("ForzaCarClubSearchForClubs_Results", "clubDetailsList", rows)


def club_search_name(req):
    rows = CLUBS.search(name=req["clubName"], limit=req["maxResults"])
    log(f"  SearchForClubsByName {req['clubName']!r}: {len(rows)}")
    return _club_list("ForzaCarClubSearchForClubsByName_Results", "clubDetailsList", rows)


@_club_errors
def club_search_tag(req):
    club = CLUBS.by_tag(req["clubTag"])
    if not club:
        raise clubs.ClubError("club not found", clubs.CODES["not_found"])
    res = xls.default_struct(SC, SC.call_struct("ForzaCarClubSearchForClubByTag_Results"))
    res["clubDetails"] = _club_struct(res["clubDetails"], club)
    return res


def club_status(req):
    club_id, _, _ = CLUBS.member_info(req["xuid"])
    return {"clubStatus": {"acceptsClubInvites": CLUBS.accepts_invites(req["xuid"]), "clubId": club_id}}


def club_members(req):
    club = CLUBS.club(req["clubId"])
    el = _elem_template("ClubGetClubMembers_Results", "clubMembers")
    rows = CLUBS.members(req["clubId"], max(1, req["maxMembers"])) if club else []
    out = []
    for r in rows:
        m = _clone(el)
        m["user"] = _owner(el["user"], r["xuid"])
        m["recentClubStatusChange"] = time.time() - r["status_changed"] < 86400
        out.append(m)
    return {"clubMembers": out, "totalMembers": CLUBS.member_count(req["clubId"]) if club else 0}


def _tag(xuid):
    return (TAGS.get(xuid) or _tag_from_db(xuid))[:31]


def _club_notify(xuid, ntype, params):
    return add_notification("", "", ntype, params, xuid)


def _notify_leaders(club, ntype, params, exclude=()):
    for x in CLUBS.leaders(club["id"], 50):
        if x not in exclude:
            _club_notify(x, ntype, params)


def _kw(v):
    """The client sends 255 for "no keyword"."""
    return 0 if v >= 255 else v


@_club_errors
def club_create(req):
    xuid, _ = _me()
    cid = CLUBS.create(xuid, req["clubTag"], req["clubName"], req["clubDescription"], req["privacy"], _kw(req["regionKeywordId"]),
                       _kw(req["attitudeKeywordId"]), _kw(req["interestKeywordId"]))
    log(f"  club created: #{cid} [{req['clubTag']}] {req['clubName']!r} by {xuid:#x} privacy={req['privacy']} "
        f"keywords={req['regionKeywordId']}/{req['attitudeKeywordId']}/{req['interestKeywordId']}")
    return {"clubId": cid}


@_club_errors
def club_change_details(req):
    xuid, _ = _me()
    CLUBS.change_details(xuid, req["newClubName"], req["newClubDescription"], req["privacyLevel"], _kw(req["regionKeyword"]),
                         _kw(req["attitudeKeyword"]), _kw(req["interestKeyword"]))
    return None


def _clear_requests(club, xuid):
    """The player is in: their "club request" notices (0x700) in the leaders' Message Center are used up."""
    for leader in CLUBS.leaders(club["id"], 50):
        for n in NOTIF.visible(leader):
            if n["type"] == 0x700 and n["params"].get("requestorXuid") == xuid:
                NOTIF.delete(n["id"])


def _joined(club, xuid, left=()):
    """Notices for a join: the leaders hear about the new member; members of a disbanded old club are told they were removed."""
    _clear_requests(club, xuid)
    _notify_leaders(club, 0x707, {"newMemberXuid": xuid, "newMemberGamertag": _tag(xuid), "clubName": club["name"][:49],
                                  "clubTag": club["tag"][:9]}, exclude=(xuid,))


@_club_errors
def club_request_join(req):
    xuid, _ = _me()
    club, approved = CLUBS.request_join(xuid, club_id=req["clubId"])
    return _after_request(club, xuid, approved)


@_club_errors
def club_request_join_name(req):
    xuid, _ = _me()
    club, approved = CLUBS.request_join(xuid, tag=req["clubTag"])
    return _after_request(club, xuid, approved)


def _after_request(club, xuid, approved):
    if approved:
        _joined(club, xuid)
    else:
        _notify_leaders(club, 0x700, {"requestorXuid": xuid, "requestorGamertag": _tag(xuid), "clubId": club["id"]})
    log(f"  {xuid:#x} asked to join [{club['tag']}]: {'joined' if approved else 'request queued'}")
    return {"requestApproved": approved}


@_club_errors
def club_approve(req):
    xuid, _ = _me()
    club = CLUBS.approve(xuid, req["approvedUser"])
    _club_notify(req["approvedUser"], 0x705, {"clubName": club["name"][:49], "clubTag": club["tag"][:9]})
    _joined(club, req["approvedUser"])
    return None


def _invite(club, iid, sponsor, invitee):
    n = _club_notify(invitee, 0x701, {"sponsorXuid": sponsor, "clubId": club["id"], "clubName": club["name"][:49],
                                       "clubTag": club["tag"][:9], "sponsorGamertag": _tag(sponsor)})
    log(f"  club invite #{iid} [{club['tag']}] {sponsor:#x} -> {invitee:#x} (notification {n['id']})")


@_club_errors
def club_invite(req):
    xuid, _ = _me()
    club, iid = CLUBS.invite(xuid, req["userToInvite"])
    _invite(club, iid, xuid, req["userToInvite"])
    return None


@_club_errors
def club_invite_name(req):
    xuid, _ = _me()
    r = DB.one("SELECT xuid FROM players WHERE lower(gamertag)=lower(?)", (req["usernameToInvite"],))
    if not r:
        raise clubs.ClubError("no such player", clubs.CODES["not_found"])
    club, iid = CLUBS.invite(xuid, r["xuid"])
    _invite(club, iid, xuid, r["xuid"])
    return None


@_club_errors
def club_accept_invite(req):
    xuid, _ = _me()
    club = CLUBS.accept_invite(xuid, req["clubId"], req["invitationId"])
    for n in NOTIF.visible(xuid):                      # the invitation notices for this club are used up
        if n["type"] == 0x701 and n["params"].get("clubId") == club["id"]:
            NOTIF.delete(n["id"])
    _joined(club, xuid)
    return None


@_club_errors
def club_change_role(req):
    xuid, _ = _me()
    target, new_role = req["userToChange"], clubs.power_role(req["newRole"])
    if not new_role:
        raise XlsError("unknown role", clubs.CODES["unknown"])
    club, old = CLUBS.change_role(xuid, target, new_role)
    base = {"clubName": club["name"][:49], "clubTag": club["tag"][:9]}
    if new_role > old:
        _club_notify(target, 0x708, {"newRole": clubs.wire_role(new_role), "newPromotedXuid": target,
                                     "newPromotedGamertag": _tag(target), **base})
    elif new_role < old:
        _club_notify(target, 0x704, base)
    log(f"  club role: {target:#x} {old} -> {new_role} by {xuid:#x}")
    return None


def _club_removed(xuid, target, club, kind, others):
    """Tell the people concerned about a kick, a quit or a disbanded club, and log it."""
    base = {"clubName": club["name"][:49], "clubTag": club["tag"][:9]}
    if kind == "kicked":
        _club_notify(target, 0x703, {"kickedXuid": target, "kickedGamertag": _tag(target), **base})
    elif kind == "quit":
        _notify_leaders(club, 0x706, {"exMemberXuid": target, "exMemberGamertag": _tag(target), **base})
    else:
        for x in others:
            _club_notify(x, 0x703, {"kickedXuid": x, "kickedGamertag": _tag(x), **base})      # the club was disbanded
    log(f"  club remove: {target:#x} {kind} by {xuid:#x} from [{club['tag']}]")


@_club_errors
def club_remove(req):
    xuid, _ = _me()
    target = req["userToRemove"]
    club, kind, others = CLUBS.remove(xuid, target)
    _club_removed(xuid, target, club, kind, others)
    return None


@_club_errors
def club_leave(req):
    """Xls.WebServices.Club.LeaveClub (no parameters): the player quits their own club; a Leader who leaves disbands it. The game's
    "Leave club" button calls this, not RemoveFromClub, and it was unanswered ("no schema") until 2026-10-20."""
    xuid, _ = _me()
    club, kind, others = CLUBS.remove(xuid, xuid)
    _club_removed(xuid, xuid, club, kind, others)
    return None


@_club_errors
def club_recommend_removal(req):
    xuid, _ = _me()
    club = CLUBS.recommend_removal(xuid, req["userToRemove"])
    _notify_leaders(club, 0x702, {"recommendingRemovalXuid": xuid, "recommendingRemovalGamertag": _tag(xuid),
                                  "recommendedForRemovalXuid": req["userToRemove"],
                                  "recommendedForRemovalGamertag": _tag(req["userToRemove"]), "clubId": club["id"]})
    return None


@_club_errors
def club_post_notice(req):
    xuid, _ = _me()
    club, role = CLUBS.membership(xuid)
    if not club or role < clubs.ROLE_CO_LEADER:
        raise clubs.ClubError("insufficient privileges", clubs.CODES["no_privilege"])
    params = {"senderXuid": xuid, "senderClubRole": clubs.wire_role(role), "senderGamertag": _tag(xuid), "noticeBoardMessage": req["noticeBoardMessage"][:561]}
    ids = [_club_notify(r["xuid"], 0x709, params)["id"] for r in CLUBS.members(club["id"], 1000)]
    nid = CLUBS.post_notice(xuid, req["noticeBoardMessage"], ids)
    log(f"  club notice #{nid} by {xuid:#x} to {len(ids)} members")
    return {"notificationId": nid}


@_club_errors
def club_delete_notice(req):
    xuid, _ = _me()
    for nid in CLUBS.delete_notice(xuid, req["notificationId"]):
        NOTIF.delete(nid)
    return None


def club_history(req):
    el = _elem_template("ClubGetClubHistory_Results", "userClubHistory")
    out = []
    for r in CLUBS.history(req["userXuid"], req["maxEntries"]):
        h = _clone(el)
        h.update({"clubName": r["club_name"][:49], "date": _stamp(r["created"]), "user": _owner(el["user"], req["userXuid"]),
                  "role": clubs.wire_role(r["role"]), "action": r["action"]})
        out.append(h)
    return {"userClubHistory": out}


def club_users_to_invite(req):
    el = _elem_template("ClubGetUsersToInvite_Results", "candidatesForInvite")
    out = []
    for r in CLUBS.users_to_invite(getattr(CTX, "xuid", 0), req["maxResults"]):
        c = _clone(el)
        c.update({"user": _owner(el["user"], r["xuid"]), "acceptsClubInvites": bool(r["accepts"]), "hasEverLoggedIn": True, "clubName": ""})
        out.append(c)
    return {"candidatesForInvite": out}


def club_users_to_join(req):
    el = _elem_template("ClubGetUsersToJoin_Results", "usersInClubs")
    out = []
    for r in CLUBS.users_to_join(getattr(CTX, "xuid", 0), req["maxResults"]):
        c = _clone(el)
        c.update({"user": _owner(el["user"], r["xuid"]), "clubMemberCount": r["members"], "clubName": r["name"][:49],
                  "clubIsOpenForMembership": r["privacy"] == clubs.PRIVACY_OPEN})
        out.append(c)
    return {"usersInClubs": out}


def club_accept_invites_pref(req):
    xuid, _ = _me()
    CLUBS.set_accepts_invites(xuid, req["acceptingClubInvites"])
    return None


def club_ladder(req):
    """Scoreboard.GetLadderRows (Club Overview > View Club Ladder): the club's MEMBERS ranked by the XP they earned in it, the caller's
    own row and the reward tiers. (The club-vs-club board is GetRows with isClubBoard.)"""
    res = xls.default_struct(SC, SC.call_struct("ForzaWebServicesScoreboardGetLadderRows_Results"))
    row_t = _elem_template("ForzaWebServicesScoreboardGetLadderRows_Results", "rows")
    tier_t = _elem_template("ForzaWebServicesScoreboardGetLadderRows_Results", "ladderTiers")
    xuid, _ = _me()
    ladder = CLUBS.member_ladder(req["clubId"])
    club_xp = sum(xp for _, xp in ladder)

    def row(rank, member, xp):
        r = _clone(row_t)
        r.update({"rank": rank, "user": _owner(row_t["user"], member), "score": scoreboard.dbl_to_u64(xp),
                  "topTierCount": sum(1 for th, _ in clubs.ClubStore.TIERS if club_xp >= th)})
        return r
    start = max(1, req["startAt"])
    res["rows"] = [row(i + 1, m, xp) for i, (m, xp) in enumerate(ladder)][start - 1:start - 1 + max(1, req["maxResults"])]
    res["ladderTiers"] = [dict(_clone(tier_t), xpThreshold=float(th), creditRewardAmount=cr, tierNumber=i + 1)
                          for i, (th, cr) in enumerate(clubs.ClubStore.TIERS)][:max(0, req["maxTiers"])]
    mine = next(((i + 1, m, xp) for i, (m, xp) in enumerate(ladder) if m == xuid), None)
    if mine:
        res["userRow"] = row(*mine)
    res["available"] = len(ladder)
    log(f"  GetLadderRows club={req['clubId']} start={req['startAt']} max={req['maxResults']}: {len(res['rows'])} of {len(ladder)} members, "
        f"club XP {club_xp:.0f}, you={'#%d' % mine[0] if mine else '-'}")
    return res


def user_get_user_data(req):
    """Xls.WebServices.User.GetUserData: the client calls it after every club change and takes its own club tag / role from the answer
    (an empty default made the tag vanish right after creating a club)."""
    xuid, tag = _me()
    res = xls.default_struct(SC, SC.call_struct("UserGetUserData_Results"))
    u = _owner(res["userData"], xuid)
    u.update({"wzGamerTag": tag[:31], "acceptsClubInvites": CLUBS.accepts_invites(xuid)})
    res["userData"] = u
    return res


def _car_info(car_id):
    """Catalog facts of a car for the shared-car structs (names, make / body / region ids, DLC content id)."""
    return DB.one("SELECT c.makeid, c.familymodelid, c.familybodyid, c.cartypeid, c.regionid, c.countryid, c.familyspecialid, "
                  "c.engineplacementid, c.contentid, c.year, v.make, v.name FROM game.data_car c JOIN game.v_cars v ON v.id = c.id WHERE c.id=?",
                  (car_id,))


def _shared_notify(club, action, car, xuid, targets):
    info = _car_info(car["model_id"])
    params = {"sharedCarAction": action, "playerXuid": xuid, "playerGamertag": _tag(xuid), "MakeName": (info["make"] if info else "")[:61],
              "ModelName": (info["name"] if info else "")[:161], "perfIndex": json.loads(car["car"]).get("perfIndex", 0.0),
              "Year": info["year"] if info else 0}
    for t in targets:
        if t != xuid:
            _club_notify(t, 0x8301, params)


@_club_errors
def shared_upload_textures(req):
    xuid, _ = _me()
    tid = CLUBS.textures_create(xuid, req["liveryTexture"], req["LiveryThumbnail"])
    log(f"  SharedCar.UploadCarTextures -> #{tid} (livery {len(req['liveryTexture'])}B, thumbnail {len(req['LiveryThumbnail'])}B)")
    return {"texturesId": tid}


@_club_errors
def shared_create(req):
    xuid, _ = _me()
    d = dict(req["completeCarData"])
    guid, profile, model_id = d.pop("gameCarId"), d.pop("ProfileId"), d["modelId"]
    cid, version, club = CLUBS.share_car(xuid, guid, profile, model_id, d, req["texturesId"], req["liveryDetails"], req["tuning"],
                                         req["overWriteExisting"])
    car = CLUBS.shared_car(cid)
    _shared_notify(club, 0, car, xuid, [r["xuid"] for r in CLUBS.members(club["id"], 1000)] if version == 1 else [])
    log(f"  SharedCar.Create car {model_id} by {xuid:#x} -> shared car #{cid} v{version} in [{club['tag']}] "
        f"(overwrite={req['overWriteExisting']}, livery {len(req['liveryDetails'])}B, tuning {len(req['tuning'])}B)")
    return {"carId": cid}


def shared_club_cars(req):
    xuid, _ = _me()
    el = _elem_template("ForzaSharedCarGetClubCars_Results", "carData")
    out = []
    for r in CLUBS.club_cars(xuid, req["maxResults"]):
        car = json.loads(r["car"])
        info = _car_info(r["model_id"])
        c = _clone(el)
        c.update({"owner": _owner(el["owner"], r["owner"]), "sharedDate": _stamp(r["shared"]), "lastUsedDate": _stamp(r["last_used"]),
                  "sharedCarId": r["id"], "modelId": r["model_id"], "raceCount": r["race_count"], "version": r["version"],
                  "performanceIndex": car.get("perfIndex", 0.0), "simulationPeakPower": car.get("simPeakPower", 0.0),
                  "simulationPeakTorque": car.get("simPeakTorque", 0.0), "curbWeight": car.get("curbWeight", 0.0),
                  "weightDistribution": car.get("weightDistribution", 0.0), "isStock": car.get("isStock", False)})
        for k in ("classId", "driveTypeId", "engineConfigId", "cylinderId", "aspirationTypeId", "displacementId", "speedRating",
                  "accelerationRating", "launchRating", "brakingRating", "handlingRating"):
            c[k] = car.get(k, c[k])
        if info:
            c.update({"year": info["year"], "makeId": info["makeid"], "modelFamilyId": info["familymodelid"], "bodyFamilyId": info["familybodyid"],
                      "carTypeId": info["cartypeid"], "regionId": info["regionid"], "countryId": info["countryid"],
                      "familySpecialId": info["familyspecialid"], "enginePlacementId": info["engineplacementid"],
                      "contentId": info["contentid"], "makeName": info["make"][:61], "modelName": info["name"][:161]})
        out.append(c)
    log(f"  SharedCar.GetClubCars by {xuid:#x}: {len(out)} cars")
    return {"carData": out}


def shared_details(req):
    el = _elem_template("ForzaSharedCarGetCarDetails_Results", "carData")
    out = []
    for cid in req["carIds"][:max(0, req["carCount"])]:
        r = CLUBS.shared_car(cid)
        if not r:
            continue
        car = json.loads(r["car"])
        c = _clone(el)
        c.update({"sharedCarId": cid, "thumbnail": CLUBS.textures(r["textures_id"])[1][:230400],
                  "manufacturerColorIndex": car.get("manufacturerColorIndex", 0), "carColorRGB": car.get("carColorRGB", 0),
                  "isMetallic": car.get("isMetallic", False), "specialColorIndex": car.get("specialColorIndex", 0)})
        out.append(c)
    return {"carData": out}


def shared_download(req):
    r = CLUBS.shared_car(req["carId"])
    if not r:
        raise XlsError(f"no such shared car {req['carId']}")
    log(f"  SharedCar.Download #{req['carId']} v{r['version']} (asked v{req['version']}): livery {len(r['livery'])}B, tuning {len(r['tuning'])}B")
    return {"liveryDetails": r["livery"][:req["maxLiveryLength"] or None], "tuningDetails": r["tuning"][:req["maxTuningLength"] or None]}


@_club_errors
def shared_remove(req):
    xuid, _ = _me()
    log(f"  SharedCar.Remove #{req['carId']} by {xuid:#x}: {'removed' if CLUBS.remove_car(xuid, req['carId']) else 'unknown car'}")
    return None


def shared_race(req):
    xuid, _ = _me()
    r = CLUBS.shared_car(req["carId"])
    if r:
        CLUBS.count_race(req["carId"])
        if r["owner"] != xuid:
            club, _ = CLUBS.membership(r["owner"])
            if club:
                _shared_notify(club, 1, r, xuid, [r["owner"]])           # someone raced the owner's shared car [action 1 = used: guess]
    return None


def _also_xls_club():
    """The `Club*` calls arrive with class Xls.WebServices.Club (seen in game 2026-10-19); keep the Forza name too."""
    for (cls, method), v in list(HANDLERS.items()):
        if cls == "Forza.WebServices.Club":
            HANDLERS[("Xls.WebServices.Club", method)] = v


# ---------------------------------------------------------------- friends, profiles, gamerpics (server/social.py)

def _pic_url(xuid, version):
    host = os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")
    return f"http://{host}:{os.environ.get('FH2MP_LSP_PUBLIC_PORT', '5080')}/gamerpic/{xuid}.png?v={version}"


def _xuid_of(segment, headers):
    """`xuid(123)` or `me` in an XSAPI path -> int."""
    m = re.fullmatch(r"xuid\((\d+)\)", segment)
    if m:
        return int(m.group(1))
    return int(xbl_xuid(headers) or 0)


def xbl_social(host, command, path, body, headers):
    """XSAPI profile / social / presence / peoplehub calls -> (status, JSON object) or None when the path is not one of ours."""
    from urllib.parse import parse_qs, urlparse
    u = urlparse(path)
    q = parse_qs(u.query)
    segs = [x for x in u.path.split("/") if x]
    settings = [x for v in q.get("settings", []) for x in v.split(",") if x]

    def users(xuids, wanted):
        return [social.profile_user(SOCIAL, x, wanted, _pic_url) for x in xuids]

    if host == "profile.xboxlive.com":
        if command == "POST" and segs == ["users", "batch", "profile", "settings"]:
            j = json.loads(body or b"{}")
            ids = [int(x) for x in j.get("userIds", [])]
            log(f"  profile settings for {len(ids)} user(s): {','.join(str(i)[-5:] for i in ids)}")
            return 200, {"profileUsers": users(ids, j.get("settings", []))}
        if command == "GET" and len(segs) == 4 and segs[0] == "users" and segs[2:] == ["profile", "settings"]:
            return 200, {"profileUsers": users([_xuid_of(segs[1], headers)], settings)}
        if command == "GET" and len(segs) == 6 and segs[0] == "users" and segs[2:4] == ["profile", "settings"] and segs[4] == "people":
            me = _xuid_of(segs[1], headers)
            log(f"  profile settings of the social group {segs[5]!r} of {me:#x}: {len(SOCIAL.friends(me))} friends")
            return 200, {"profileUsers": users(SOCIAL.friends(me), settings)}
    elif host == "social.xboxlive.com" and command == "GET" and len(segs) == 3 and segs[0] == "users":
        me = _xuid_of(segs[1], headers)
        if segs[2] == "people":
            log(f"  social people of {me:#x}: {len(SOCIAL.friends(me))}")
            return 200, social.people(SOCIAL, me)
        if segs[2] == "summary":
            return 200, social.summary(SOCIAL, me)
    elif host == "peoplehub.xboxlive.com" and command == "GET" and segs[:1] == ["users"] and segs[2:3] == ["people"]:
        me = _xuid_of(segs[1], headers)
        out = []
        for f in SOCIAL.friends(me):
            prof = SOCIAL.profile(f)
            out.append({"xuid": str(f), "isFavorite": False, "isFollowingCaller": True, "isFollowedByCaller": True, "displayName": prof["GameDisplayName"],
                        "realName": "", "displayPicRaw": _pic_url(f, prof["_pic_version"]), "gamertag": prof["Gamertag"],
                        "gamerScore": prof["Gamerscore"], "presenceState": "Online" if social.online(f) else "Offline"})
        log(f"  peoplehub people of {me:#x}: {len(out)}")
        return 200, {"people": out}
    elif host == "userpresence.xboxlive.com":
        if command == "POST" and segs == ["users", "batch"]:
            ids = [int(x) for x in json.loads(body or b"{}").get("users", [])]
            log(f"  presence of {len(ids)} user(s)")
            return 200, [social.presence(x) for x in ids]
        if command == "GET" and len(segs) == 3 and segs[0] == "users" and segs[2] == "presence":
            return 200, social.presence(_xuid_of(segs[1], headers))
    return None


def _player_ref(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def social_api(method, path, body):
    """Admin / launcher API for friends and gamerpics (loopback only for now; a launcher needs per-player authentication first).

    GET    /api/players                       known players with their friend count
    GET    /api/friends/<xuid>                a player's friends
    POST   /api/friends                       {"a": xuid, "b": xuid}  make two players friends
    DELETE /api/friends                       {"a": xuid, "b": xuid}
    PUT    /api/players/<xuid>/gamerpic       body: a PNG (<= 1 MB); DELETE removes it (the identicon is used again)
    GET    /api/players/<xuid>/gamerpic       the picture
    POST   /api/players/<xuid>                {"displayName": "...", "gamerscore": n, "tier": "Gold"}
    """
    parts = [p for p in path.split("?")[0].split("/") if p]
    try:
        if parts[:2] == ["api", "players"] and len(parts) == 2 and method == "GET":
            return 200, {"players": [{"xuid": r["xuid"], "gamertag": r["gamertag"], "level": r["career_level"], "friends": r["friends"],
                                      "hasGamerpic": bool(r["has_gamerpic"])} for r in SOCIAL.players()]}, None
        if parts[:2] == ["api", "friends"]:
            if len(parts) == 3 and method == "GET":
                return 200, {"friends": [{"xuid": f, "gamertag": _tag_from_db(f)} for f in SOCIAL.friends(_player_ref(parts[2]))]}, None
            if len(parts) == 2 and method in ("POST", "DELETE"):
                d = json.loads(body or b"{}")
                a, b = _player_ref(d["a"]), _player_ref(d["b"])
                if method == "POST":
                    SOCIAL.add_friend(a, b)
                    return 201, {"friends": [a, b]}, None
                return 200, {"removed": SOCIAL.remove_friend(a, b)}, None
        if parts[:2] == ["api", "players"] and len(parts) >= 3:
            xuid = _player_ref(parts[2])
            if len(parts) == 4 and parts[3] == "gamerpic":
                if method == "PUT":
                    SOCIAL.set_gamerpic(xuid, body)
                    return 200, {"updated": xuid, "bytes": len(body)}, None
                if method == "DELETE":
                    SOCIAL.clear_gamerpic(xuid)
                    return 200, {"cleared": xuid}, None
                if method == "GET":
                    return 200, None, SOCIAL.gamerpic(xuid)
            if len(parts) == 3 and method == "POST":
                d = json.loads(body or b"{}")
                SOCIAL.set_profile(xuid, d.get("displayName"), d.get("gamerscore"), d.get("tier"))
                return 200, {"updated": xuid}, None
    except (ValueError, KeyError, TypeError) as e:
        return 400, {"error": str(e)}, None
    return None


RAW_HANDLERS = {}        # (class, method) -> fn(body) for calls the exe has no descriptor for; returns the response payload bytes
RAW_HANDLERS[("Forza.WebServices.OnlineProfile", "UpdateLivery")] = lambda body: profile_update_livery(body)


HANDLERS = {
    ("Xls.WebServices.Content", "UploadContent"): ("", content_upload),
    ("Xls.WebServices.Content", "DownloadContent"): ("", content_download),
    ("Forza.WebServices.LargeFileUpload", "Create"): ("", large_file_create),
    ("Forza.WebServices.ContentVersioning", "CreateVersionedLivery"): ("", cv_create_livery),
    ("Forza.WebServices.ContentVersioning", "CreateVersionedLayerGroup"): ("", cv_create_layergroup),
    ("Forza.WebServices.ContentVersioning", "CreateVersionedTune"): ("", cv_create_tune),
    ("Forza.WebServices.ContentVersioning", "CreateVersionedTuneFromStructs"): ("", cv_create_tune_structs),
    ("Forza.WebServices.ContentVersioning", "RetrieveVersionedLiveryById"): ("", cv_get_livery),
    ("Forza.WebServices.ContentVersioning", "RetrieveVersionedLayerGroupById"): ("", cv_get_layergroup),
    ("Forza.WebServices.ContentVersioning", "RetrieveVersionedTuneById"): ("", cv_get_tune),
    ("Forza.WebServices.ContentVersioning", "AddReferenceToVersionedLivery"): ("", _cv_ref("versionedLiveryId", 1)),
    ("Forza.WebServices.ContentVersioning", "RemoveReferenceToVersionedLivery"): ("", _cv_ref("versionedLiveryId", -1)),
    ("Forza.WebServices.ContentVersioning", "AddReferenceToVersionedLayerGroup"): ("", _cv_ref("versionedLayerGroupId", 1)),
    ("Forza.WebServices.ContentVersioning", "RemoveReferenceToVersionedLayerGroup"): ("", _cv_ref("versionedLayerGroupId", -1)),
    ("Forza.WebServices.ContentVersioning", "AddReferenceToVersionedTune"): ("", _cv_ref("versionedTuneId", 1)),
    ("Forza.WebServices.ContentVersioning", "RemoveReferenceToVersionedTune"): ("", _cv_ref("versionedTuneId", -1)),
    ("Forza.WebServices.StorefrontLivery", "Create"): ("", store_livery_create),
    ("Forza.WebServices.StorefrontTuning", "Create"): ("", store_tuning_create),
    ("Forza.WebServices.Storefront", "SearchLiveryFiles"): ("", store_search_liveries),
    ("Forza.WebServices.Storefront", "SearchTuningFiles"): ("", store_search_tunes),
    ("Forza.WebServices.Storefront", "GetFileHeavyWeightData"): ("", store_heavy),
    ("Forza.WebServices.Storefront", "GetMyFileList"): ("", store_my_files),
    ("Forza.WebServices.Storefront", "GetFileDataIfShared"): ("", store_if_shared),
    ("Forza.WebServices.Storefront", "SynchronizeSharedFiles"): ("", store_noop),
    ("Forza.WebServices.Storefront", "IncrementLikes"): ("", _bump_handler("likes")),
    ("Forza.WebServices.Storefront", "IncrementUsage"): ("", _bump_handler("uses")),
    ("Forza.WebServices.Storefront", "IncrementViews"): ("", _bump_handler("views")),
    ("Forza.WebServices.Storefront", "BatchIncrementViews"): ("", store_batch_views),
    ("Forza.WebServices.Storefront", "RemoveFile"): ("", store_remove),
    ("Forza.WebServices.Storefront", "ChangeFileTitle"): ("", store_retitle),
    ("Forza.WebServices.Storefront", "ChangeFileDescription"): ("", store_redescribe),
    ("Forza.WebServices.Storefront", "GetFileKeywords"): ("", ugc_keywords),
    ("Forza.WebServices.Storefront", "GetFeaturedLiveriesForRandomCars"): ("", store_featured_liveries),
    ("Forza.WebServices.Storefront", "GetLiveriesForDrivatar"): ("", store_drivatar_liveries),
    ("Forza.WebServices.Storefront", "GetCuratedContent"): ("", store_curated),
    ("Forza.WebServices.Storefront", "GetCuratedContentData"): ("", store_curated_data),
    ("Forza.WebServices.Storefront", "ReportContent"): ("", store_noop),
    ("Forza.WebServices.Storefront", "ToggleFollowedCreator"): ("", store_noop),
    ("Forza.WebServices.CarClub", "GetClubDetails"): ("", club_details),
    ("Forza.WebServices.CarClub", "GetClubFullInfo"): ("", club_full_info),
    ("Forza.WebServices.CarClub", "GetClubKeywords"): ("", club_keywords),
    ("Forza.WebServices.CarClub", "GetFriendsClubs"): ("", club_friends),
    ("Forza.WebServices.CarClub", "GetRecommendedClubs"): ("", club_recommended),
    ("Forza.WebServices.CarClub", "SearchForClubByTag"): ("", club_search_tag),
    ("Forza.WebServices.CarClub", "SearchForClubs"): ("", club_search),
    ("Forza.WebServices.CarClub", "SearchForClubsByName"): ("", club_search_name),
    ("Forza.WebServices.Club", "CreateClub"): ("", club_create),
    ("Forza.WebServices.Club", "ChangeClubDetails"): ("", club_change_details),
    ("Forza.WebServices.Club", "GetClubMembers"): ("", club_members),
    ("Forza.WebServices.Club", "GetClubStatus"): ("", club_status),
    ("Forza.WebServices.Club", "ChangeMemberRole"): ("", club_change_role),
    ("Forza.WebServices.Club", "InviteToClub"): ("", club_invite),
    ("Forza.WebServices.Club", "InviteToClubByName"): ("", club_invite_name),
    ("Forza.WebServices.Club", "AcceptClubInvite"): ("", club_accept_invite),
    ("Forza.WebServices.Club", "RequestJoiningClub"): ("", club_request_join),
    ("Forza.WebServices.Club", "RequestJoiningClubByName"): ("", club_request_join_name),
    ("Forza.WebServices.Club", "ApproveRequestToJoin"): ("", club_approve),
    ("Forza.WebServices.Club", "RemoveFromClub"): ("", club_remove),
    ("Forza.WebServices.Club", "LeaveClub"): ("", club_leave),
    ("Forza.WebServices.Club", "RecommendMemberForRemoval"): ("", club_recommend_removal),
    ("Forza.WebServices.Club", "CreateNoticeBoardNotification"): ("", club_post_notice),
    ("Forza.WebServices.Club", "DeleteNoticeBoardNotification"): ("", club_delete_notice),
    ("Forza.WebServices.Club", "GetClubHistory"): ("", club_history),
    ("Forza.WebServices.Club", "GetUsersToInvite"): ("", club_users_to_invite),
    ("Forza.WebServices.Club", "GetUsersToJoin"): ("", club_users_to_join),
    ("Forza.WebServices.User", "AcceptClubInvites"): ("", club_accept_invites_pref),
    ("Xls.WebServices.User", "AcceptClubInvites"): ("", club_accept_invites_pref),
    ("Forza.WebServices.SharedCar", "GetClubCars"): ("", shared_club_cars),
    ("Forza.WebServices.SharedCar", "UploadCarTextures"): ("", shared_upload_textures),
    ("Forza.WebServices.SharedCar", "Create"): ("", shared_create),
    ("Forza.WebServices.SharedCar", "GetCarDetails"): ("", shared_details),
    ("Forza.WebServices.SharedCar", "DownloadSharedCar"): ("", shared_download),
    ("Forza.WebServices.SharedCar", "RemoveSharedCar"): ("", shared_remove),
    ("Forza.WebServices.SharedCar", "IncrementRaceCount"): ("", shared_race),
    ("Forza.WebServices.Scoreboard", "GetLadderRows"): ("", club_ladder),
    ("Xls.WebServices.User", "GetUserData"): ("", user_get_user_data),
    ("Forza.WebServices.User", "GetUserData"): ("", user_get_user_data),
    ("Forza.WebServices.User", "GetPermissionsTable"): ("", permissions_table),
    ("Forza.WebServices.User", "GetEconomySettings"): ("", economy_settings),
    ("Forza.WebServices.Gifting", "RetrieveCreditsGift"): ("", gift_credits),
    ("Forza.WebServices.Gifting", "RetrieveCarGift"): ("", gift_car),
    ("Forza.WebServices.Scoreboard", "GetRows"): ("", lambda req: sb_rows(req, "ForzaWebServicesScoreboardGetRows_Results")),
    ("Forza.WebServices.Scoreboard", "GetTimeTrials"): ("", sb_time_trials),
    ("Forza.WebServices.Hopper", "GetGameOptions"): ("", hopper_game_options),
    ("Forza.WebServices.Scoreboard", "SubmitScore"): ("", sb_submit_score),
    ("Forza.WebServices.Scoreboard", "UploadFile"): ("", sb_upload_file),
    ("Forza.WebServices.Scoreboard", "NotifyRival"): ("", sb_notify_rival),
    ("Forza.WebServices.Scoreboard", "DownloadFile"): ("", sb_download_file),
    ("Forza.WebServices.Scoreboard", "SubmitXpScore"): ("", sb_submit_xp),
    ("Forza.WebServices.Scoreboard", "GetBestScore"): ("", sb_best),
    ("Forza.WebServices.Scoreboard", "GetRival"): ("", sb_rival),
    ("Forza.WebServices.Scoreboard", "GetRowsAnthem"): ("", sb_rows),
    ("Forza.WebServices.Drivatar", "SaveDrivatarInformation"): ("", drv_save),
    ("Forza.WebServices.Drivatar", "GetDrivatarsForCache"): ("", drv_for_cache),
    ("Forza.WebServices.Drivatar", "GetDrivatarInformationForRace"): ("", drv_for_race),
    ("Forza.WebServices.User", "Logon"): ("ForzaUserLogon", logon),
    ("Xls.WebServices.Notifications", "RetrieveForUser"): ("", notif_retrieve),
    ("Xls.WebServices.Notifications", "MarkRead"): ("", notif_mark_read),
    ("Xls.WebServices.Notifications", "Clear"): ("", notif_clear),
}


# Reads whose "empty success" would be cached by the client and replace its built-in data (the
# Drivatar download cache was overwritten with an empty one by exactly this). Until we serve real
# data they fail, so the client keeps using what it ships with.
FAIL_UNTIL_IMPLEMENTED = {
    ("Forza.WebServices.Drivatar", "GetDrivatarInformationForTrack"),
}


_also_xls_club()


def handle_xls(cls, method, body):
    key = (cls.split(",")[0].strip(), method)
    if key in RAW_HANDLERS:
        return frame(RAW_HANDLERS[key](body) or b"")
    if key in FAIL_UNTIL_IMPLEMENTED and key not in HANDLERS:
        log("  not implemented -> error result (client keeps its local data)")
        return frame(result=2, error=0x0001, text=b"fh2mp: not implemented")
    pname, pfields = find_struct(cls, method, "_Parameters")
    req = None
    if pfields is not None and body:
        try:
            req, rem = xls.decode(SC, pfields, body)
            if rem:
                log(f"  warning: {rem} unread request bytes")
        except Exception as e:  # noqa: BLE001
            log(f"  request decode failed: {e}")
    elif pfields is not None:
        log(f"  request schema {pname} (empty body)")
    rname, rfields = find_struct(cls, method, "_Results")
    if rfields is None and key not in HANDLERS:
        log(f"  no schema for {cls} {method}; answering OK with empty body")
        return frame()
    try:
        result = HANDLERS[key][1](req) if key in HANDLERS else None
    except XlsError as e:
        log(f"  -> error result: {e.text}")
        return frame(result=2, error=e.code, text=e.text.encode())
    if result is None:
        if rfields is None:
            return frame()
        result = xls.default_struct(SC, rfields)
        if key not in HANDLERS:
            log(f"  default {rname}")
    return frame(xls.encode(SC, rfields, result))


class ApiError(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status, self.msg = status, msg


def _field_info(ntype):
    return [{"name": f.name, "type": S_TYPES.get(f.type, hex(f.type)), "maxLength": f.size or None}
            for f in notif_fields(ntype)]


S_TYPES = {0: "u8", 1: "u16", 2: "i16", 3: "i32", 4: "u32", 5: "i64", 6: "u64", 7: "float", 9: "string",
           0x10: "datetime", 0x11: "bool"}


def _notif_json(n):
    return {"id": n["id"], "type": n["type"], "kind": next((k for k, v in KINDS.items() if v[0] == n["type"]), None),
            "text": n["message"], "sra": n["sra"], "fields": n.get("params", {}), "xuid": n.get("xuid", 0),
            "read": n["read"], "readBy": n.get("readBy", []), "sent": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(n["sent"] / 1e7 - 11644473600))}


def api(method, path, body):
    """JSON admin API for notifications (no auth: the server only listens on loopback by default).

    GET    /api/types                 notification kinds, categories and each type's fields
    GET    /api/notifications         everything queued
    POST   /api/notifications         {"kind"|"type", "text", "sra", "xuid", "fields": {...}} -> created item
    GET    /api/notifications/<id>
    DELETE /api/notifications/<id>    (the client drops it at its next poll)
    DELETE /api/notifications         remove all
    """
    parts = [p for p in path.split("?")[0].split("/") if p]          # ["api", "notifications", id]
    if parts[:2] == ["api", "types"] and method == "GET":
        kinds = [{"kind": k, "type": t, "shows_in": where, "fields": _field_info(t)} for k, (t, where) in KINDS.items()]
        known = sorted(set(NOTIF_STRUCTS) - {v[0] for v in KINDS.values()})
        return 200, {"kinds": kinds, "otherTypes": [{"type": t, "fields": _field_info(t)} for t in known],
                     "categories": [{"id": c, "name": n, "description": d} for c, (n, d) in sorted(CATEGORIES.items())]}
    if parts[:2] != ["api", "notifications"]:
        raise ApiError(404, "unknown endpoint")
    if len(parts) == 2:
        if method == "GET":
            return 200, {"notifications": [_notif_json(n) for n in NOTIF.all()]}
        if method == "DELETE":
            return 200, {"removed": NOTIF.delete_all()}
        if method == "POST":
            try:
                d = json.loads(body or b"{}")
            except ValueError as e:
                raise ApiError(400, f"invalid JSON: {e}")
            if "kind" in d:
                if d["kind"] not in KINDS:
                    raise ApiError(400, f"unknown kind {d['kind']!r}; have {sorted(KINDS)}")
                ntype = KINDS[d["kind"]][0]
            elif "type" in d:
                try:
                    ntype = int(d["type"], 0) if isinstance(d["type"], str) else int(d["type"])
                except ValueError:
                    raise ApiError(400, "type must be a number")
                if ntype not in NOTIF_STRUCTS:
                    raise ApiError(400, f"type {ntype:#x} is not dispatched by the client")
            else:
                raise ApiError(400, "give \"kind\" or \"type\"")
            fields = dict(d.get("fields") or {})
            car_id = None
            if ntype == 0x8502 and "carId" in fields:   # convenience: look make/model up in the game database
                car_id = int(fields.pop("carId"))
                mm = CARS.make_model(car_id)
                if mm is None:
                    raise ApiError(400, f"unknown carId {car_id} (not in the game database)")
                fields.setdefault("MakeId", mm[0])
                fields.setdefault("ModelId", mm[1])
            is_gift = ntype in (0x8500, 0x8501, 0x8502) and not fields.get("GiftId")    # GiftId = the new id, set on insert
            try:
                params = notif_params(ntype, fields)
                xuid = d.get("xuid", 0)
                xuid = int(xuid, 0) if isinstance(xuid, str) else int(xuid)
            except (ValueError, TypeError) as e:
                raise ApiError(400, str(e))
            if "text" in d and "Message" not in {f.name for f in notif_fields(ntype)}:
                raise ApiError(400, f"type {ntype:#x} has no message text; set \"fields\" instead")
            n = add_notification(str(d.get("text", "")), str(d.get("sra", "")), ntype, params, xuid, car_id, is_gift)
            return 201, _notif_json(n)
        raise ApiError(405, "method not allowed")
    try:
        nid = int(parts[2])
    except ValueError:
        raise ApiError(404, "bad notification id")
    n = NOTIF.get(nid)
    if n is None:
        raise ApiError(404, "no such notification")
    if method == "GET":
        return 200, _notif_json(n)
    if method == "DELETE":
        NOTIF.delete(nid)
        return 200, {"removed": nid}
    raise ApiError(405, "method not allowed")


GSI_VERSION = "6CC152FD"   # build version string in the exe (PTR_s_6CC152FD); same value as the NetV2 handshake


def gsds_variants():
    """Answer for POST gameserverds.xboxlive.com/titles/<tid>/variants (Xbox GSDS, GetVariants in the exe).
    Shape from Microsoft's open-source XSAPI (branch 1807_xdk_qfe_preview, Source/Services/GameServerPlatform):
    every field is a JSON *string* (isPublisher "True"/"False", rank and player counts numeric strings).
    The game matches the GSI set names against a regex, by default `Release_<version>\\..*` (FUN_141838d70), and
    takes the "initialize failed" exit of the matchmaking-init state when none matches (docs/matchmaking.md), so the
    sets are named Release_<version>.<mode>."""
    schema = "00000000-0000-0000-0000-000000000002"
    modes = ("FreeRoam", "CarMeet", "RoadTrip")
    return json.dumps({
        "variants": [{"gamevariantId": "00000000-0000-0000-0000-00000000000%d" % (i + 1), "name": "Release_%s.%s" % (GSI_VERSION, m),
                      "isPublisher": "True", "rank": "0", "gameVariantSchemaId": schema} for i, m in enumerate(modes)],
        "variantSchemas": [{"variantSchemaId": schema, "schemaContent": "{}", "name": "FreeRoam"}],
        "gsiSets": [{"minRequiredPlayers": "1", "maxAllowedPlayers": str(MAX_LOBBY_PLAYERS),
                     "gsiSetId": "00000000-0000-0000-0000-00000000010%d" % i, "gsiSetName": "Release_%s.%s" % (GSI_VERSION, m),
                     "selectionOrder": str(i), "variantSchemaId": schema} for i, m in enumerate(modes)],
    }).encode()


MAX_LOBBY_PLAYERS = 12   # engine ceiling seen so far (traffic server 12 slots); docs/matchmaking.md


def gsds_qos_servers():
    """GET gameserverds.xboxlive.com/titles/<tid>/qosservers; {"qosServers":[{serverFqdn, serverSecureDeviceAddress,
    targetLocation}]}, all strings (XSAPI source). The list must not be empty: XSAPI then returns a null vector from its
    QoS async and the game dereferences it (crash at game+0x41fd28, docs/matchmaking.md). The host comes from the
    FH2MP_PUBLIC_HOST environment variable (default 127.0.0.1); the secure device address is an opaque base64 blob."""
    import base64
    host = os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")
    sda = base64.b64encode(b"FH2MP-SDA-0000000").decode()
    return json.dumps({"qosServers": [{"serverFqdn": host, "serverSecureDeviceAddress": sda,
                                       "targetLocation": "North Central US"}]}).encode()


def xbl_xuid(headers):
    """The caller's Xbox user id: the shim's XSAPI token is "XBL3.0 x=fh2mp;<decimal xuid>" (shim/src/winmm_proxy.c)."""
    if getattr(CTX, "authed", False):
        return str(CTX.xuid)                                    # from the session token, never from a header the client wrote
    if AUTH is not None and AUTH.mode == "required":
        return None
    m = re.search(r"x=fh2mp;(\d+)", headers.get("Authorization", ""))
    return m.group(1) if m else None


def gsms_cluster():
    """POST gameserverms.xboxlive.com/titles/<tid>/clusters (AllocateClusterAsync) -> ClusterResult JSON (key names read from
    Microsoft.Xbox.Services.dll: fulfillmentState fulfilled|queued|aborted, pollIntervalMilliseconds, hostName, region,
    portMappings[{Key, Value{external, internal}}]). Every allocation is answered at once with this server
    (FH2MP_PUBLIC_HOST, FH2MP_GAME_PORT, default 127.0.0.1:5000)."""
    host = os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")
    port = int(os.environ.get("FH2MP_GAME_PORT", "5000"))
    # Value types checked in Microsoft.Xbox.Services.dll (Windows.Data.Json ValueType, wrong type -> 0x83750005):
    # pollIntervalMilliseconds / fulfillmentState / hostName / region / Key are strings, Value is an object whose
    # external / internal are numbers.
    return json.dumps({"fulfillmentState": "fulfilled", "pollIntervalMilliseconds": "1000", "hostName": host,
                       "region": "North Central US",
                       "portMappings": [{"Key": "NetV2", "Value": {"external": port, "internal": port}}]}).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def client_ip(self):
        """The caller's address. Behind the local reverse proxy (nginx sets X-Real-IP) that is the header; otherwise the socket peer."""
        peer = self.client_address[0]
        if peer in ("127.0.0.1", "::1"):
            real = self.headers.get("X-Real-IP")
            if real:
                return auth.norm_ip(real)
        return auth.norm_ip(peer)

    def _reply_json(self, status, obj, ctype="application/json", extra=()):
        b = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)

    def _v1(self, body):
        status, obj, ctype = launcher_api.handle(LAUNCHER, self.command, self.path, self.headers, body, self.client_ip())
        self._reply_json(status, obj, ctype, [("Cache-Control", "no-store")] if ctype == "application/json" else [("Cache-Control", "max-age=60")])

    def _ops(self, body, proxied):
        status, obj = ops_api.handle(OPS, sys.modules[__name__], self.command, self.path, self.headers, body, self.client_address[0], proxied)
        self._reply_json(status, obj, extra=[("Cache-Control", "no-store")])

    def _remove_old_client(self):
        """A client with a shim below minShim that proves who it is (a live launcher token) is also taken out of the game: its live
        connections end and it stays out until it signs in again from an up-to-date launcher (on_revoke / on_login). Only a verified token
        counts, because anyone can send an X-UserId header for somebody else. At most once a minute per player."""
        token = self.headers.get("X-FH2MP-Token", "")
        if not token or AUTH is None:
            return
        try:
            acct = AUTH.authenticate(token, self.client_ip())
        except auth.AuthError:
            return
        now = time.monotonic()
        if now - _OLD_REMOVED.get(acct.xuid, -1e9) < 60:
            return
        _OLD_REMOVED[acct.xuid] = now
        if len(_OLD_REMOVED) > 20000:
            _OLD_REMOVED.clear()
        log(f"  old shim {CTX.shim or '(none)'!r}: removing {acct.gamertag!r} {acct.xuid:#x} from the game")
        on_revoke(acct.xuid)

    def _identify(self):
        """Sets CTX.xuid / CTX.authed / CTX.tag from the session token. False (after answering) when the request must not go on."""
        CTX.authed, CTX.tag = False, ""
        CTX.shim = self.headers.get("X-FH2MP-Shim", "")          # the client's shim version ("1.0.0-rc2"); empty for clients that do not send it
        need = LAUNCHER.config.get()["minShim"] if LAUNCHER is not None else "0.0.0"
        if not launcher_api.version_at_least(CTX.shim, need):
            log(f"  refused: shim {CTX.shim or '(none)'!r} is older than {need}")
            self._remove_old_client()
            self._reply_json(426, {"error": "shim_too_old", "message": f"FH2MP is out of date. Update the launcher and press Play (needs {need})."})
            return False
        token = self.headers.get("X-FH2MP-Token", "")
        if AUTH is not None and AUTH.mode != "off":
            if token:
                try:
                    acct = AUTH.authenticate(token, self.client_ip())
                except auth.AuthError as e:
                    self._reply_json(e.status, {"error": e.code, "message": e.message})
                    return False
                CTX.xuid, CTX.tag, CTX.authed = acct.xuid, acct.gamertag, True
                announce_auth(acct.xuid, self.client_ip())
                return True
            if AUTH.mode == "required":
                self._reply_json(401, {"error": "login_required", "message": "start the game from the FH2MP launcher"})
                return False
        try:
            CTX.xuid = int(self.headers.get("X-UserId", "0"), 0)
        except ValueError:
            CTX.xuid = 0
        return True

    def _handle(self):
        global SEQ
        SEQ += 1
        n = int(self.headers.get("Content-Length") or 0)
        if (self.path.startswith("/v1/") and n > launcher_api.MAX_BODY) or (self.path.startswith("/ops/") and n > 65536) \
                or n > ugc.MAX_REQUEST_BODY or n < 0:
            self.close_connection = True
            return self._reply_json(413, {"error": "too_large"})
        body = self.rfile.read(n) if n else b""
        proxied = "X-Real-IP" in self.headers or "X-Forwarded-For" in self.headers
        if self.path.startswith("/ops/"):
            return self._ops(body, proxied)
        if self.path.startswith(("/admin/", "/api/")) and (self.client_address[0] not in ("127.0.0.1", "::1") or proxied):
            self.send_response(403)                       # operator and game-server endpoints are loopback only (and never through the proxy)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path.startswith("/v1/"):
            return self._v1(body)
        if self.command == "GET" and self.path.startswith("/gamerpic/"):          # public: the client downloads pictures from here
            return self._gamerpic()
        if self.path.startswith("/admin/"):
            return self._admin()
        if self.path.startswith("/api/"):
            return self._api(body)
        host = self.headers.get("X-FH2MP-Host", "")
        cls = self.headers.get("X-ClassName", "")
        method = self.headers.get("X-Method", "")
        if not self._identify():
            return
        social.touch(CTX.xuid or int(xbl_xuid(self.headers) or 0))
        log(f"#{SEQ} {self.command} {host}{self.path} {cls} {method} body={len(body)} user={CTX.xuid:#x}")
        out = b""
        if host.endswith("forzamotorsport.net") and cls:
            try:
                out = handle_xls(cls, method, body)
            except Exception as e:  # noqa: BLE001
                import traceback
                traceback.print_exc()
                msg = f"fh2mp: {e}".encode()
                out = frame(result=2, error=0x1, text=msg)
        ctype = "bin/xtsw"
        status = 200
        extra = []
        if host == "gameserverds.xboxlive.com" and self.path.endswith("/variants"):
            out, ctype = gsds_variants(), "application/json"
            log(f"  GSDS variants -> {out.decode()}")
        elif host == "gameserverds.xboxlive.com" and "qosservers" in self.path:
            out, ctype = gsds_qos_servers(), "application/json"
            log(f"  GSDS qos -> {out.decode()}")
        elif host == "gameserverms.xboxlive.com" and self.path.rstrip("/").endswith("/clusters"):
            out, ctype = gsms_cluster(), "application/json"
            log(f"  GSMS cluster -> {out.decode()}")
        elif host == "client-strings.xboxlive.com" and self.path.rstrip("/").endswith("/system/strings/validate"):
            # XSAPI StringService.VerifyString (club names, noticeboard posts...): {"stringsToVerify": [...]} ->
            # {"verifyStringResult": [{"resultCode": 0, "offendingString": ""}, ...]}, one entry per string; 0 = fine
            # (keys from Microsoft.Xbox.Services.dll [inferred layout])
            try:
                n = len(json.loads(body or b"{}").get("stringsToVerify", [])) or 1
            except (ValueError, AttributeError):
                n = 1
            out = json.dumps({"verifyStringResult": [{"resultCode": 0, "offendingString": ""} for _ in range(n)]}).encode()
            ctype = "application/json"
            log(f"  strings/validate: {n} string(s) accepted")
        elif host in ("profile.xboxlive.com", "social.xboxlive.com", "peoplehub.xboxlive.com", "userpresence.xboxlive.com") \
                and (res := xbl_social(host, self.command, self.path, body, self.headers)) is not None:
            status, obj = res
            out, ctype = json.dumps(obj).encode(), "application/json"
        elif host == "achievements.xboxlive.com":
            # The game polls GetAchievementsForTitleIdAsync every frame it is set; a 200 with no usable list makes the
            # embedded XSAPI complete with a NULL result and the game dereferences it (docs/matchmaking.md). Fail it.
            out, ctype, status = b'{"code":0,"description":"achievements unavailable"}', "application/json", 404
            log("  achievements -> 404")
        elif host == "sessiondirectory.xboxlive.com" and xbl.parse_path(self.path) is not None:
            # MPSD session document (PUT create/update, GET read, DELETE); see server/xbl.py
            caller = xbl_xuid(self.headers)
            if caller is None and AUTH is not None and AUTH.mode == "required" and self.command != "GET":
                st, doc = 401, None                     # session writes need a signed-in caller
                out, status = b"{}", 401
            elif self.command == "DELETE":
                st, doc = xbl.delete_session(self.path, caller), None
                out, status = b"", st
            else:
                st, doc = (xbl.put_session(self.path, body, caller) if self.command in ("PUT", "POST")
                           else xbl.get_session(self.path, caller))
                status = st
                out = json.dumps(doc).encode() if doc is not None else b"{}"
            ctype = "application/json"
            if doc is not None:
                extra.append(("ETag", xbl.etag(doc)))
            who = ""
            if doc is not None and "defaultTemplate_qos" not in self.path:
                who = " members=" + ",".join(
                    f"{m['constants']['system']['xuid'][-5:]}{'+' if m['properties']['system'].get('active') else '-'}"
                    for m in doc.get("members", {}).values())
            log(f"  MPSD {self.command} session -> {status} ({len(out)} bytes, {xbl.count()} session(s)){who}")
        elif host == "momatch.xboxlive.com" and xbl.parse_momatch(self.path) is not None:
            caller = xbl_xuid(self.headers)
            if caller is None and AUTH is not None and AUTH.mode == "required":
                status, doc = 401, None                 # matchmaking needs a signed-in caller
            elif self.command == "DELETE":
                status, doc = xbl.delete_ticket(self.path, caller), None
            elif self.command == "POST":
                status, doc = xbl.create_ticket(self.path, body, caller)
            else:
                status, doc = xbl.get_ticket(self.path, caller)
            out = json.dumps(doc).encode() if doc is not None else b""
            ctype = "application/json"
            log(f"  MOMATCH {self.command} -> {status} {out.decode()[:300]}")
        elif host == "sessiondirectory.xboxlive.com" and self.command == "GET" and "/sessions" in self.path:
            # MPSD session query (MultiplayerService.GetSessionsAsync). Any 200 answer (empty body or {"results":[]}) makes
            # the embedded XSAPI complete the async with a NULL result and the game dereferences it (crash at
            # game+0x41fd28, docs/matchmaking.md). An HTTP error leaves the async in the Error state, which the game's
            # poll loop (FUN_140415030) skips, as when Azimuth's offline stub failed the request.
            out, ctype, status = b'{"code":0,"description":"no sessions"}', "application/json", 404
            log("  MPSD sessions -> 404")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def _gamerpic(self):
        m = re.match(r"/gamerpic/(\d+)\.png", self.path)
        png = SOCIAL.gamerpic(int(m.group(1))) if m else None
        self.send_response(200 if png else 404)
        self.send_header("Content-Type", "image/png")
        self.send_header("Cache-Control", "max-age=60")
        self.send_header("Content-Length", str(len(png or b"")))
        self.end_headers()
        self.wfile.write(png or b"")
        if png:
            log(f"  gamerpic {m.group(1)[-5:]} served ({len(png)}B)")

    def _api(self, body):
        r = social_api(self.command, self.path, body)
        if r is not None:
            status, obj, raw = r
            b = raw if raw is not None else json.dumps(obj, indent=1).encode()
            self.send_response(status)
            self.send_header("Content-Type", "image/png" if raw is not None else "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        try:
            status, obj = api(self.command, self.path, body)
        except ApiError as e:
            status, obj = e.status, {"error": e.msg}
        b = json.dumps(obj, indent=1).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def _admin(self):
        """GET /admin/send?text=...[&type=512][&sra=...]  |  /admin/list  |  /admin/clear  |
        /admin/drivatar?tag=NAME[&xuid=0x..][&skill=0..4][&wristband=N]  (add a player)  |
        /admin/drivatar?xuid=0x..&set=sideswipeMean=0.8,blockingMean=0.9[&lock=1]  (hand-tune)"""
        from urllib.parse import parse_qs, urlparse
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/admin/gamemode":
            name, template = xbl.create_game_session(int(q.get("type", "1")), [x for x in q.get("xuids", "").split(",") if x])
            out = json.dumps({"name": name, "template": template}) + "\n"
            log(f"  game-mode session created: {name} ({template}) type {q.get('type', '1')}")
        elif u.path == "/admin/send":
            fixed = {"text", "sra", "type"}
            n = add_notification(q.get("text", ""), q.get("sra", ""), int(q["type"], 0) if "type" in q else None,
                                 notif_params(int(q["type"], 0) if "type" in q else MSG_TYPE,
                                              {k: v for k, v in q.items() if k not in fixed}))
            out = f"queued #{n['id']}\n"
        elif u.path == "/admin/drivatar" and "remove" in q:
            out = "removed\n" if DRV.remove(int(q.get("xuid", "0"), 0)) else "no such drivatar\n"
        elif u.path == "/admin/drivatar" and ("set" in q or "lock" in q):
            sets = dict(kv.split("=", 1) for kv in q.get("set", "").split(",") if kv)
            r = DRV.edit(int(q.get("xuid", "0x9000000000000"), 0), sets,
                         None if "lock" not in q else q["lock"] not in ("0", "false"))
            out = "no such drivatar\n" if r is None else (
                f"{r['GamerTag']!r} locked={r.get('_locked', False)} sideswipe={r['UserInfo']['sideswipeMean']:.3f} "
                f"temperament={r['UserInfo']['temperamentMean']:.3f} blocking={r['UserInfo']['blockingMean']:.3f} "
                f"skill={r['UserSkillLevel']}\n")
        elif u.path == "/admin/drivatar":
            r = DRV.add_player(int(q.get("xuid", "0x9000000000001"), 0), q.get("tag", "FH2MP Guest"),
                               int(q["skill"]) if "skill" in q else None,
                               int(q["wristband"]) if "wristband" in q else None)
            out = f"drivatar {r['GamerTag']!r} xuid={r['Xuid']:#x} skill={r['UserSkillLevel']} " \
                  f"wristband={r['wristbandLevel']}\n"
        elif u.path in ("/admin/rotate", "/admin/rivals"):
            evs = RIV.rotate() if u.path == "/admin/rotate" else RIV.events()
            announce_rotation()
            out = "".join(f"#{e['id']} track {e['track']} {e['kind']} {e['attrs']} :: {e['name']}\n" for e in evs)
        elif u.path == "/admin/clear":
            NOTIF.delete_all()
            out = "cleared\n"
        else:
            out = "".join(f"#{n['id']} type={n['type']:#x} read={n['read']} {n['message']!r}\n" for n in NOTIF.all())
        b = out.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    do_GET = do_POST = do_PUT = do_DELETE = _handle


def setup(dsn=None):
    """Connects to PostgreSQL and builds every store (module globals). Used by main() and by the tests."""
    global SC, CARS, DB, NOTIF
    DB = db.open_db(dsn)
    if not DB.one("SELECT 1 FROM information_schema.schemata WHERE schema_name='game'"):
        sys.exit("the game catalog is not loaded: build the bundle with tools/export_gamedata.py and load it with tools/load_gamedata_pg.py")
    SC = xls.Schema(DB)
    CARS = cardb.CarDb(DB)
    log(f"database {DB.path}: game catalog schema `game` present")
    NOTIF = notifstore.NotificationStore(DB)
    global CLUBS, SOCIAL
    CLUBS = clubs.ClubStore(DB)
    SOCIAL = social.SocialStore(DB)
    global UGC
    UGC = ugc.UgcStore(DB)
    setup_auth()
    fixed = UGC.repair_liveries()
    if fixed:
        log(f"user content: repaired {fixed} livery row(s) stored with the old upload prefix")
    b0, b1 = UGC.recompress()
    log(f"user content: {UGC.count()} files, {UGC.stored_bytes()} bytes stored" + (f" (recompressed {b0} -> {b1})" if b0 else ""))
    log(f"{NOTIF.count()} notifications")
    for r in DB.query("SELECT xuid, gamertag, career_level FROM players WHERE gamertag <> ''"):
        TAGS.setdefault(r["xuid"], r["gamertag"])           # identities survive a restart
        if r["career_level"]:
            LEVELS.setdefault(r["xuid"], r["career_level"])
    global DRV
    shipped = [json.loads(r["record"]) for r in DB.query("SELECT record FROM game.drivatar_shipped ORDER BY idx")]
    DRV = drivatar.DrivatarStore(SC, shipped, DB)
    SOCIAL.bot_name, SOCIAL.name_lookup = DRV.bot_name, DRV.tag_for_xuid       # bots show their racing-driver names in lobbies, not Player#
    log(f"drivatar store: {len(DRV.templates)} shipped templates, {DRV.count()} stored players")
    global SB
    SB = scoreboard.ScoreStore(DB, [t["GamerTag"] for t in DRV.templates] + DRV.gamertags(), cars=CARS.fake_pool(), route_length=CARS.route_length, event_info=event_info,
                          event_cars=event_cars)
    log(f"scoreboards: {SB.count()} stored submissions")
    SOCIAL.bot_name = lambda x: SB.fake_name(x) or DRV.bot_name(x)          # the rivals' ids resolve to their racing-driver names, not Player#
    global RIV
    RIV = rivals.RivalCatalog(CARS, None, DB)
    log(f"rival events with the card class enforced: {RIV.enforce_stored_classes()}")
    log(f"rival event texts refreshed: {RIV.refresh_texts()}")
    log(f"rival events: {len(RIV.events())} this rotation (month {RIV.serial()[0]}, re-roll {RIV.serial()[1]})")
    log(f"schema loaded from the database: {SC.count()} struct descriptors")


def online_count():
    now = time.time()
    return sum(1 for t in list(social.LAST_POLL.values()) if now - t < social.ONLINE_SECONDS)


_AUTH_ANNOUNCED = {}


def announce_auth(xuid, ip):
    """Tell the game server (UDP, loopback) that this xuid is signed in from this address, so it accepts that player's OpenConnection.
    Repeated at most every 20 s per player; the game server forgets an entry after a few minutes without news."""
    now = time.monotonic()
    key = (xuid, ip)
    if now - _AUTH_ANNOUNCED.get(key, -1e9) < 20:
        return
    _AUTH_ANNOUNCED[key] = now
    if len(_AUTH_ANNOUNCED) > 20000:
        _AUTH_ANNOUNCED.clear()
    xbl.announce_auth(xuid, ip)


_OLD_REMOVED = {}


def on_login(xuid, ip):
    """A fresh sign-in (launcher login / sign-up): lifts an earlier ban or kick on the game server, then announces the address."""
    xbl.announce_allow(xuid)
    _AUTH_ANNOUNCED.pop((xuid, ip), None)
    announce_auth(xuid, ip)


def on_revoke(xuid):
    """A ban or kick: sessions and tickets go, and the game server ends the player's live UDP connections."""
    for key in [k for k in _AUTH_ANNOUNCED if k[0] == xuid]:
        del _AUTH_ANNOUNCED[key]
    xbl.drop_player(xuid)
    xbl.announce_revoke(xuid)


def setup_auth():
    """Identity service and the launcher API. FH2MP_ENV (prod|dev|test) names the realm that signatures are bound to and is checked
    against the database name, so a dev server can never be pointed at the production database by accident."""
    global AUTH, LAUNCHER
    env = os.environ.get("FH2MP_ENV", "prod")
    dbname = DB.one("SELECT current_database()")[0]
    if (env == "prod" and dbname != "fh2mp") or (env == "dev" and dbname == "fh2mp"):
        sys.exit(f"refusing to start: FH2MP_ENV={env} but the database is {dbname!r}")
    AUTH = auth.AuthService(DB, env=env, mode=os.environ.get("FH2MP_AUTH", "optional"),
                            registration=os.environ.get("FH2MP_REGISTRATION", "open"),
                            token_ip=os.environ.get("FH2MP_TOKEN_IP", "exact"))

    def changed(xuid):
        r = DB.one("SELECT gamertag FROM accounts WHERE xuid=?", (xuid,))
        if r:
            TAGS[xuid] = r["gamertag"]

    AUTH.on_change = changed
    AUTH.on_revoke = on_revoke
    for r in DB.query("SELECT xuid, gamertag FROM accounts"):
        TAGS[r["xuid"]] = r["gamertag"]
    host = os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")
    LAUNCHER = launcher_api.Ctx(
        AUTH, SOCIAL, launcher_api.LauncherConfig(), online_count, log=log, on_login=on_login,
        lsp_public={"url": os.environ.get("FH2MP_LSP_URL", f"http://{host}:{os.environ.get('FH2MP_LSP_PUBLIC_PORT', '5080')}")},
        game_public={"host": host, "port": int(os.environ.get("FH2MP_GAME_PORT", "5000"))})
    global OPS
    OPS = ops_api.OpsConfig.from_env()
    ops_api.load_saved(sys.modules[__name__])
    log(f"ops api: {'enabled' if OPS.token else 'disabled (FH2MP_OPS_TOKEN unset)'}, allowed from {', '.join(str(n) for n in OPS.allow)}")
    log(f"auth: env={env} mode={AUTH.mode} registration={AUTH.registration} accounts={AUTH.stats()['accounts']}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5080
    setup()
    bind = os.environ.get("FH2MP_LSP_BIND", "127.0.0.1")
    srv = ThreadingHTTPServer((bind, port), Handler)
    log(f"fh2mp LSP listening on {bind}:{port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
