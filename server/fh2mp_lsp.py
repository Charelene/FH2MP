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

Usage: python3 -I server/fh2mp_lsp.py [port] <path-to-forza_durango_release_final.exe>
       (the exe path can also be given in the FH2MP_EXE environment variable)
"""
import json
import os
import re
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import threading  # noqa: E402
import xls  # noqa: E402
import drivatar  # noqa: E402
import scoreboard  # noqa: E402
import cardb  # noqa: E402
import xbl  # noqa: E402

SEQ = 0
SC = None
STATE = {}
CTX = threading.local()   # per-request: xuid (from X-UserId)
DRV = None                # drivatar.DrivatarStore
CARS = None               # cardb.CarDb (the user's own game database)
SB = None                 # scoreboard.ScoreStore
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
    TAGS[lg["xuid"]] = lg["gamerTag"]
    LEVELS[lg["xuid"]] = lg["currentCareerLevel"]
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
        "userData": {**r["userData"], "qwXuid": lg["xuid"], "wzGamerTag": lg["gamerTag"]},
        # The client keeps at most 7 categories; it shows the ones with a tab (1, 3, 5, 6, 7) in the Message Center.
        "notificationCategories": [
            {"type": t, "name": name, "description": desc, "backgroundImageFileId": 0}
            for t, (name, desc) in sorted(CATEGORIES.items())
        ],
    })
    return {"responseData": r}


# ---------------------------------------------------------------- notifications (Message Center)

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")   # runtime state, git-ignored
NOTIF_FILE = os.path.join(DATA_DIR, "notifications.json")
NOTIF = []          # dicts: id, type, message, sra, sent, read
NOTIF_NEXT_ID = [int(time.time())]  # unique across restarts so the client never sees a reused id
POLL_SECONDS = 5
MSG_TYPE = 0x200    # notificationType carrying MessageNotificationParameters (to be confirmed in game)


def filetime(unix=None):
    return int(((time.time() if unix is None else unix) + 11644473600) * 10_000_000)


def notif_save():
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = NOTIF_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"next_id": NOTIF_NEXT_ID[0], "items": NOTIF}, f, indent=1)
    os.replace(tmp, NOTIF_FILE)


def notif_load():
    try:
        with open(NOTIF_FILE) as f:
            d = json.load(f)
        NOTIF[:] = d["items"]
        NOTIF_NEXT_ID[0] = max(NOTIF_NEXT_ID[0], d.get("next_id", 0))
        log(f"loaded {len(NOTIF)} stored notifications")
    except FileNotFoundError:
        pass


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
    return xls.S.fields_of(SC.exe, SC.thunks, s) if isinstance(s, int) else SC.call_struct(s)


def add_notification(message, sra="", ntype=None, params=None, xuid=0):
    n = {"id": NOTIF_NEXT_ID[0], "type": MSG_TYPE if ntype is None else ntype, "message": message,
         "sra": sra, "sent": filetime(), "read": False}
    if params:
        n["params"] = params
    if xuid:
        n["xuid"] = xuid
    NOTIF_NEXT_ID[0] += 1
    NOTIF.append(n)
    notif_save()
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
    me = getattr(CTX, "xuid", 0)
    return [n for n in NOTIF if not n.get("xuid") or n["xuid"] == me]


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
    for n in NOTIF:
        if n["id"] == req["notificationId"]:
            n["read"] = True
    notif_save()
    return None


def notif_clear(req):
    NOTIF[:] = [n for n in NOTIF if n["id"] != req["notificationId"]]
    notif_save()
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
    recs = DRV.pool(max(n, len(DRV.templates) + len(DRV.players)))
    log(f"  ForCache numberOfDrivatars={n}: serving {len(recs)} ({len(DRV.players)} player + shipped)")
    return {"drivatarInfos": recs}


def drv_for_race(req):
    xuid, tag = _me()
    me = DRV.for_player(xuid, tag)
    me["wristbandLevel"] = DRV.bucket_wristband(req["bucketId"])   # the car the player is driving now
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
        "submittedTime": scoreboard.iso(sc_row["t"]), "carId": sc_row["car"], "carClassId": sc_row["cls"],
        "carPerformanceIndex": sc_row["cpi"], "usedSTM": sc_row["stm"], "usedTCS": sc_row["tcs"],
        "usedABS": sc_row["abs"], "isClean": sc_row["clean"], "ghostFileId": 0, "bounty": 0})
    return rowdict


def sb_submit_score(req):
    xuid, tag = _me()
    d = req["data"]
    s = SB.submit(xuid, tag, req["type"], d)
    log(f"  SubmitScore board=({d['scoreboardType']},{req['type']}) track={d['trackId']} car={d['carId']} "
        f"score={s['score']:.3f} clean={d['isClean']}")
    return None


def sb_submit_xp(req):
    xuid, _ = _me()
    total = SB.add_xp(xuid, req["xp"])
    log(f"  SubmitXpScore +{req['xp']:g} -> total {total:g}")
    return None


def sb_best(req):
    xuid, _ = _me()
    b = SB.best_of(xuid, req["scoreboardType"], req["scoreType"], req["trackId"])
    log(f"  GetBestScore board=({req['scoreboardType']},{req['scoreType']}) gsid={req['gameScoreboardId']} "
        f"track={req['trackId']} car={req['carId']}: "
        f"{'%.3f' % b['score'] if b else 'none'}")
    if not b:
        return {"bestScore": {"score": 0, "isClean": False}, "scoreFound": False}
    return {"bestScore": {"score": scoreboard.dbl_to_u64(b["score"]), "isClean": b["clean"]}, "scoreFound": True}


def sb_rival(req):
    xuid, _ = _me()
    r = SB.rival_for(xuid, req["scoreboardType"], req["scoreType"], req["trackId"])
    res = xls.default_struct(SC, SC.call_struct("ForzaWebServicesScoreboardGetRival_Results"))
    if r is None:
        log("  GetRival: no rival")
        return res
    sd = res["rival"]["scoreData"]
    _score_row(r, sd)
    log(f"  GetRival board=({req['scoreboardType']},{req['scoreType']}) event={req['rivalEventId']} "
        f"track={req['trackId']} pivot={req['pivotId']} rivalXuid={req['rivalXuid']:#x}: {r['tag']} {r['score']:.3f}")
    return res


def sb_rows(req, results="ForzaWebServicesScoreboardGetRowsAnthem_Results"):
    xuid, _ = _me()
    page, mine, nreal, nfake = SB.rows(xuid, req["scoreboardType"], req["scoreType"], req["trackId"],
                                      max(1, req["startAt"]), min(200, max(0, req["maxResults"])))
    FRIENDS_VIEW = 2    # the leaderboard's "FILTER: FRIENDS" (GetRows view=2); we have no friends, so it is just you
    if req["view"] == FRIENDS_VIEW:
        page = [dict(mine, position=1)] if mine is not None else []
        mine = dict(mine, position=1) if mine is not None else None
    res = xls.default_struct(SC, SC.call_struct(results))
    tmpl = res["userRow"]
    res["rows"] = [_score_row(r, json.loads(json.dumps(tmpl))) for r in page]
    if mine is not None:
        res["userRow"] = _score_row(mine, res["userRow"])
    log(f"  GetRows[{results[len('ForzaWebServicesScoreboard'):-8]}] board=({req['scoreboardType']},{req['scoreType']}) view={req['view']} "
        f"track={req['trackId']} pivot={req['pivotId']}: {len(page)} rows ({nreal} real, {nfake} fake), "
        f"you={'#%d' % mine['position'] if mine else '-'}")
    return res


TIME_TRIAL_TRACKS = [578, 912, 902, 856]   # trackIds the client itself asked rivals/best scores for


def sb_time_trials(req):
    """The rival events listed under Rivals. Category numbers are unknown, so each category gets two events
    named after it; the Rivals tab titles then show which category belongs to which page."""
    now = time.time()
    tts = []
    for cat in range(0, 4):
        for k in range(2):
            tts.append({"id": 100 + cat * 10 + k, "category": cat, "scoreType": 1,
                        "name": f"FH2MP Rival C{cat}-{k + 1}", "description": f"Category {cat} test rival event {k + 1}.",
                        "notRegisteredMessage": "", "trackId": TIME_TRIAL_TRACKS[(cat * 2 + k) % len(TIME_TRIAL_TRACKS)],
                        "trackName": f"Test track {cat}-{k + 1}",
                        "hopperId": 0, "gameOptionsIndex": 0, "startDate": scoreboard.iso(now - 86400),
                        "endDate": scoreboard.iso(now + 86400 * 30), "isUserRegistered": True, "isNew": k == 0,
                        "isFeatured": k == 0, "isActive": True})
    n = req["maxTimeTrials"]
    log(f"  GetTimeTrials max={n}: serving {min(n, len(tts))} rival events")
    return {"timeTrials": tts[:max(n, 0) or len(tts)]}


# ---------------------------------------------------------------- permissions

def permissions_table(req):
    """Every feature enabled. The client looks features up by methodId (FUN_14126c2b0 -> FUN_140f09930):
    a feature missing from the table reads as 4 ("locked", e.g. the "server not available" popup when
    downloading a gifted car), an entry with access 0 reads as allowed."""
    n = min(req["maxPermissions"] if req else 0, 255) or 255
    log(f"  GetPermissionsTable max={req['maxPermissions'] if req else None}: enabling features 1..{n}")
    return {"permissions": [{"methodId": i, "tier": 0, "isEnabled": True, "access": 0} for i in range(1, n + 1)],
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


def gift_credits(req):
    n = _gift(req["giftId"], (0x8500,))
    n["claimed"] = True
    notif_save()
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
    n["claimed"] = True
    notif_save()
    res = xls.default_struct(SC, SC.call_struct("ForzaGiftingRetrieveCarGift_Results"))
    res["car"].update(car)
    res["giftListing"].update({"giftId": req["giftId"], "senderXuid": p.get("SenderXuid", 0),
                               "reason": p.get("Reason", 0)})
    res["carHistory"]["base"]["originalOwner"] = p.get("SenderGamerTag", "")[:31]
    log(f"  RetrieveCarGift #{req['giftId']}: carId {car_id} (make {car['make']}, {car['year']}) "
        f"from {p.get('SenderGamerTag', '')!r}; blob sizes asked {req['liveryDetailsBufferSize']}/"
        f"{req['partsInstalledBufferSize']}/{req['partsInTrunkBufferSize']}")
    return res


HANDLERS = {
    ("Forza.WebServices.User", "GetPermissionsTable"): ("", permissions_table),
    ("Forza.WebServices.User", "GetEconomySettings"): ("", economy_settings),
    ("Forza.WebServices.Gifting", "RetrieveCreditsGift"): ("", gift_credits),
    ("Forza.WebServices.Gifting", "RetrieveCarGift"): ("", gift_car),
    ("Forza.WebServices.Scoreboard", "GetRows"): ("", lambda req: sb_rows(req, "ForzaWebServicesScoreboardGetRows_Results")),
    ("Forza.WebServices.Scoreboard", "GetTimeTrials"): ("", sb_time_trials),
    ("Forza.WebServices.Scoreboard", "SubmitScore"): ("", sb_submit_score),
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


def handle_xls(cls, method, body):
    key = (cls.split(",")[0].strip(), method)
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
            "read": n["read"], "sent": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(n["sent"] / 1e7 - 11644473600))}


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
            return 200, {"notifications": [_notif_json(n) for n in NOTIF]}
        if method == "DELETE":
            n = len(NOTIF)
            NOTIF.clear()
            notif_save()
            return 200, {"removed": n}
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
            if ntype in (0x8500, 0x8501, 0x8502) and not fields.get("GiftId"):
                fields["GiftId"] = NOTIF_NEXT_ID[0] & 0x7FFFFFFF
            try:
                params = notif_params(ntype, fields)
                xuid = d.get("xuid", 0)
                xuid = int(xuid, 0) if isinstance(xuid, str) else int(xuid)
            except (ValueError, TypeError) as e:
                raise ApiError(400, str(e))
            if "text" in d and "Message" not in {f.name for f in notif_fields(ntype)}:
                raise ApiError(400, f"type {ntype:#x} has no message text; set \"fields\" instead")
            n = add_notification(str(d.get("text", "")), str(d.get("sra", "")), ntype, params, xuid)
            if car_id is not None:
                n["carId"] = car_id
                notif_save()
            return 201, _notif_json(n)
        raise ApiError(405, "method not allowed")
    try:
        nid = int(parts[2])
    except ValueError:
        raise ApiError(404, "bad notification id")
    n = next((x for x in NOTIF if x["id"] == nid), None)
    if n is None:
        raise ApiError(404, "no such notification")
    if method == "GET":
        return 200, _notif_json(n)
    if method == "DELETE":
        NOTIF.remove(n)
        notif_save()
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

    def _handle(self):
        global SEQ
        SEQ += 1
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if self.path.startswith("/admin/"):
            return self._admin()
        if self.path.startswith("/api/"):
            return self._api(body)
        host = self.headers.get("X-FH2MP-Host", "")
        cls = self.headers.get("X-ClassName", "")
        method = self.headers.get("X-Method", "")
        try:
            CTX.xuid = int(self.headers.get("X-UserId", "0"), 0)
        except ValueError:
            CTX.xuid = 0
        log(f"#{SEQ} {self.command} {host}{self.path} {cls} {method} body={len(body)}")
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
        elif host == "achievements.xboxlive.com":
            # The game polls GetAchievementsForTitleIdAsync every frame it is set; a 200 with no usable list makes the
            # embedded XSAPI complete with a NULL result and the game dereferences it (docs/matchmaking.md). Fail it.
            out, ctype, status = b'{"code":0,"description":"achievements unavailable"}', "application/json", 404
            log("  achievements -> 404")
        elif host == "sessiondirectory.xboxlive.com" and xbl.parse_path(self.path) is not None:
            # MPSD session document (PUT create/update, GET read, DELETE); see server/xbl.py
            if self.command == "DELETE":
                st, doc = xbl.delete_session(self.path), None
                out, status = b"", 204
            else:
                st, doc = (xbl.put_session(self.path, body, xbl_xuid(self.headers)) if self.command in ("PUT", "POST")
                           else xbl.get_session(self.path, xbl_xuid(self.headers)))
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
            if self.command == "DELETE":
                status, doc = xbl.delete_ticket(self.path), None
            elif self.command == "POST":
                status, doc = xbl.create_ticket(self.path, body, xbl_xuid(self.headers))
            else:
                status, doc = xbl.get_ticket(self.path)
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

    def _api(self, body):
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
        elif u.path == "/admin/clear":
            NOTIF.clear()
            notif_save()
            out = "cleared\n"
        else:
            out = "".join(f"#{n['id']} type={n['type']:#x} read={n['read']} {n['message']!r}\n" for n in NOTIF)
        b = out.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    do_GET = do_POST = do_PUT = do_DELETE = _handle


def main():
    global SC
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5080
    exe = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("FH2MP_EXE")
    if not exe or not os.path.exists(exe):
        sys.exit("usage: fh2mp_lsp.py [port] <path-to-forza_durango_release_final.exe>  (or set FH2MP_EXE)")
    SC = xls.Schema(exe)
    global CARS
    CARS = cardb.CarDb(exe)
    log(f"car database: {'found' if CARS.db else 'NOT found'} ({CARS.path})")
    notif_load()
    global DRV
    DRV = drivatar.DrivatarStore(SC, os.path.join(os.path.dirname(exe), "media", "drivatarglobalcache", "DefaultCache.bin"), DATA_DIR)
    log(f"drivatar store: {len(DRV.templates)} shipped templates, {len(DRV.players)} stored players")
    global SB
    SB = scoreboard.ScoreStore(DATA_DIR, [t["GamerTag"] for t in DRV.templates] + [r["GamerTag"] for r in DRV.players.values()])
    log(f"scoreboards: {len(SB.subs)} stored submissions")
    log(f"schema loaded from {exe}: {len(SC.thunks)} struct descriptors")
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    log(f"fh2mp LSP listening on 127.0.0.1:{port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
