#!/usr/bin/env python3
"""FH2 Turn 10 LSP stand-in (Xls web services over HTTP, Content-Type bin/xtsw).

The shim's IXMLHTTPRequest2 (shim/src/xhr2.inc) redirects every request the game makes through
XMLHTTP60 to this server (default 127.0.0.1:5080, plain HTTP). The original host arrives in
X-FH2MP-Host; only forzamotorsport.net hosts are Turn 10 Xls calls, the rest (xboxlive.com ...) are
answered with an empty 200.

Request : headers X-ClassName / X-Method, body = "<Call>_Parameters" struct, big endian.
Response: 12-byte header (Result u32, ErrorCode u32, DataLength u32; all big endian) followed by the
          "<Call>_Results" struct. Result 0 = OK; 2 = error (ErrorCode | 0x80510000), DataLength bytes
          of message text follow the header. Schemas come from the user's own exe (tools/xls_schema.py).

Usage: python3 -I server/fh2mp_lsp.py [port] <path-to-forza_durango_release_final.exe>
       (the exe path can also be given in the FH2MP_EXE environment variable)
"""
import os
import struct
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xls  # noqa: E402

SEQ = 0
SC = None
STATE = {}


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

NOTIF = []          # dicts: id, type, message, sra, sent, read
NOTIF_NEXT_ID = [int(time.time())]  # unique across restarts so the client never sees a reused id
POLL_SECONDS = 5
MSG_TYPE = 0x200    # notificationType carrying MessageNotificationParameters (to be confirmed in game)


def filetime(unix=None):
    return int(((time.time() if unix is None else unix) + 11644473600) * 10_000_000)


def add_notification(message, sra="", ntype=None):
    n = {"id": NOTIF_NEXT_ID[0], "type": MSG_TYPE if ntype is None else ntype, "message": message,
         "sra": sra, "sent": filetime(), "read": False}
    NOTIF_NEXT_ID[0] += 1
    NOTIF.append(n)
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
    return None


def notif_clear(req):
    NOTIF[:] = [n for n in NOTIF if n["id"] != req["notificationId"]]
    return None


HANDLERS = {
    ("Forza.WebServices.User", "Logon"): ("ForzaUserLogon", logon),
    ("Xls.WebServices.Notifications", "RetrieveForUser"): ("", notif_retrieve),
    ("Xls.WebServices.Notifications", "MarkRead"): ("", notif_mark_read),
    ("Xls.WebServices.Notifications", "Clear"): ("", notif_clear),
}


def handle_xls(cls, method, body):
    key = (cls.split(",")[0].strip(), method)
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
        """GET /admin/send?text=...[&type=512][&sra=...]  |  /admin/list  |  /admin/clear"""
        from urllib.parse import parse_qs, urlparse
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/admin/send":
            n = add_notification(q.get("text", ""), q.get("sra", ""), int(q["type"], 0) if "type" in q else None)
            out = f"queued #{n['id']}\n"
        elif u.path == "/admin/clear":
            NOTIF.clear()
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
    log(f"schema loaded from {exe}: {len(SC.thunks)} struct descriptors")
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    log(f"fh2mp LSP listening on 127.0.0.1:{port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
