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
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import threading  # noqa: E402
import xls  # noqa: E402
import drivatar  # noqa: E402

SEQ = 0
SC = None
STATE = {}
CTX = threading.local()   # per-request: xuid (from X-UserId)
DRV = None                # drivatar.DrivatarStore
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
        # Names are the numeric type so the in-game tab titles reveal the type -> page mapping.
        "notificationCategories": [
            {"type": t, "name": f"Category {t:#x}", "description": f"Category {t:#x} description",
             "backgroundImageFileId": 0}
            for t in range(1, 8)  # the client keeps at most 7 categories
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


def add_notification(message, sra="", ntype=None):
    n = {"id": NOTIF_NEXT_ID[0], "type": MSG_TYPE if ntype is None else ntype, "message": message,
         "sra": sra, "sent": filetime(), "read": False}
    NOTIF_NEXT_ID[0] += 1
    NOTIF.append(n)
    notif_save()
    log(f"  + notification #{n['id']} type={n['type']:#x}: {message!r}")
    return n


def notif_retrieve(req):
    results, block = [], bytearray()
    for n in NOTIF:
        if n["type"] == 0x100:  # NewDlcNotificationParameters {Message, OfferId}
            pb = xls.encode(SC, SC.call_struct("NewDlcNotificationParameters"),
                            {"Message": n["message"], "OfferId": n["sra"]})
        else:                   # MessageNotificationParameters {Message, SRAUri}
            pb = xls.encode(SC, SC.call_struct("MessageNotificationParameters"),
                            {"Message": n["message"], "SRAUri": n["sra"]})
        results.append({"id": n["id"], "xuidRecipient": 0, "notificationType": n["type"],
                        "hasThumbnail": False, "isRead": n["read"], "deleteAction": 0,
                        "expirationDate": filetime(time.time() + 86400 * 365), "sentDate": n["sent"],
                        "cbParameters": len(pb)})
        block += pb
    return {"results": results, "parameterBlock": bytes(block), "nextRetrieveSeconds": POLL_SECONDS,
            "reconcileActiveNotifications": True, "activeNotificationIds": [n["id"] for n in NOTIF]}


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


HANDLERS = {
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
    result = HANDLERS[key][1](req) if key in HANDLERS else None
    if result is None:
        if rfields is None:
            return frame()
        result = xls.default_struct(SC, rfields)
        if key not in HANDLERS:
            log(f"  default {rname}")
    return frame(xls.encode(SC, rfields, result))


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
        self.send_response(200)
        self.send_header("Content-Type", "bin/xtsw")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def _admin(self):
        """GET /admin/send?text=...[&type=512][&sra=...]  |  /admin/list  |  /admin/clear  |
        /admin/drivatar?tag=NAME[&xuid=0x..][&skill=0..4][&wristband=N]  (add a player)  |
        /admin/drivatar?xuid=0x..&set=sideswipeMean=0.8,blockingMean=0.9[&lock=1]  (hand-tune)"""
        from urllib.parse import parse_qs, urlparse
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/admin/send":
            n = add_notification(q.get("text", ""), q.get("sra", ""), int(q["type"], 0) if "type" in q else None)
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
    notif_load()
    global DRV
    DRV = drivatar.DrivatarStore(SC, os.path.join(os.path.dirname(exe), "media", "drivatarglobalcache", "DefaultCache.bin"), DATA_DIR)
    log(f"drivatar store: {len(DRV.templates)} shipped templates, {len(DRV.players)} stored players")
    log(f"schema loaded from {exe}: {len(SC.thunks)} struct descriptors")
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    log(f"fh2mp LSP listening on 127.0.0.1:{port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
