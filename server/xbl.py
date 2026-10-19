"""Minimal stand-in for the Xbox Live Multiplayer Session Directory (MPSD), enough for the game's embedded XSAPI.

The game (through Microsoft's XSAPI, loaded by Azimuth) writes sessions with
  PUT /serviceconfigs/{scid}/sessionTemplates/{template}/sessions/{name}
and reads them back with GET on the same path. The reply must be a full session document: XSAPI's
multiplayer_session::_Deserialize (open source, github.com/microsoft/xbox-live-api, branch 1807_xdk_qfe_preview,
Source/Services/Multiplayer) requires constants.system and properties.system, and each member needs
constants.system and properties.system. Everything else is optional, so the document echoes what the game
sent and adds the fields the real service would (correlationId, startTime, branch, changeNumber, membersInfo).
Nothing here talks to Microsoft; sessions live in memory and are lost when the server restarts.
"""
import base64
import copy
import json
import os
import re
import time
import uuid

SESSION_PATH = re.compile(
    r"^/serviceconfigs/(?P<scid>[^/]+)/sessiontemplates/(?P<template>[^/]+)/sessions/(?P<name>[^/?]+)",
    re.IGNORECASE)

_SESSIONS = {}   # (scid, template, name) lowercase -> record


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())


def _merge(dst, src):
    """Recursive dict merge (MPSD PUT is a partial update)."""
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def parse_path(path):
    m = SESSION_PATH.match(path.split("?")[0])
    if not m:
        return None
    return m.group("scid"), m.group("template"), m.group("name")


def _member_doc(index, m, now, key=""):
    constants = copy.deepcopy(m.get("constants", {}))
    constants.setdefault("system", {})
    if key.isdigit():
        constants["system"]["xuid"] = key           # the entry's key is the identity the server vouched for
    else:
        constants["system"].setdefault("xuid", "0")
    constants["system"].setdefault("initialize", False)
    constants["system"]["index"] = index
    constants.setdefault("custom", {})
    props = copy.deepcopy(m.get("properties", {}))
    props.setdefault("system", {})
    props["system"].setdefault("active", True)
    props["system"].setdefault("ready", False)
    props["system"].setdefault("secureDeviceAddress", "")
    props.setdefault("custom", {})
    return {
        "constants": constants,
        "properties": props,
        "gamertag": m.get("gamertag", "Player 1"),
        "reserved": False,
        "joinTime": now,
        "nat": "open",
        "turn": False,
        "next": index + 1,
    }


def session_document(rec):
    """Build the JSON document XSAPI expects from a stored record."""
    now = rec["start"]
    system = copy.deepcopy(rec["constants"].get("system", {}))
    system.setdefault("version", 1)
    system.setdefault("visibility", "open")
    max_players = (rec["constants"].get("custom", {}).get("gameServerPlatform", {}) or {}).get("maxAllowedPlayers", 100)
    system.setdefault("maxMembersCount", int(max_players))
    system.setdefault("capabilities", {})
    props_system = copy.deepcopy(rec["properties"].get("system", {}))
    props_system.setdefault("joinRestriction", "none")
    props_system.setdefault("readRestriction", "none")
    props_system.setdefault("closed", False)
    props_system.setdefault("locked", False)
    props_system.setdefault("allocateCloudCompute", False)
    members = {}
    for i, key in enumerate(sorted(rec["members"])):
        members[str(i)] = _member_doc(i, rec["members"][key], now, key)
    return {
        "correlationId": rec["corr"],
        "startTime": now,
        "branch": rec["branch"],
        "changeNumber": rec["change"],
        "contractVersion": 105,
        "constants": {"system": system, "custom": copy.deepcopy(rec["constants"].get("custom", {}))},
        "properties": {"system": props_system, "custom": copy.deepcopy(rec["properties"].get("custom", {}))},
        "membersInfo": {"first": 0, "next": len(members), "count": len(members), "accepted": len(members)},
        "members": members,
        "servers": copy.deepcopy(rec.get("servers", {})),
    }


def _own_member_key(name, xuid):
    """The member entry a caller may write: "me" (XSAPI's spelling) or its own xuid."""
    return name == "me" or name == xuid


def put_session(path, body, xuid=None):
    """Create or update a session. Returns (http status, document or None).

    `xuid` is the authenticated caller; the server's own calls leave it None and are not restricted. A caller may create a
    session (and becomes its owner), join it, and write, update or remove only its own member entry. Only a member or the owner
    may change the session's properties or servers."""
    key = parse_path(path)
    if key is None:
        return 400, None
    try:
        req = json.loads(body.decode("utf-8")) if body else {}
    except ValueError:
        return 400, None
    if not isinstance(req, dict):
        return 400, None
    k = tuple(x.lower() for x in key)
    rec = _SESSIONS.get(k)
    created = rec is None
    members_req = req.get("members") or {}
    if not isinstance(members_req, dict):
        return 400, None
    if xuid:
        # decide everything before changing anything, so a refused request leaves the session as it was
        if any(not _own_member_key(name, xuid) for name in members_req):
            return 403, None                    # nobody writes or removes somebody else's member entry
        if not created:
            is_member = xuid in rec["members"] or rec.get("owner") == xuid
            joining = any(_own_member_key(name, xuid) and m is not None for name, m in members_req.items())
            if not is_member and not joining:
                return 403, None
            if not is_member and ("constants" in req or "properties" in req or "servers" in req):
                return 403, None                # joining does not let a stranger rewrite the session
    if created:
        rec = {"scid": key[0], "template": key[1], "name": key[2], "corr": str(uuid.uuid4()),
               "branch": str(uuid.uuid4()), "start": _now(), "change": 0, "owner": xuid or None,
               "constants": {}, "properties": {}, "members": {}, "servers": {}}
        _SESSIONS[k] = rec
    if "constants" in req:
        _merge(rec["constants"], req["constants"])
    if "properties" in req:
        _merge(rec["properties"], req["properties"])
    for name, m in members_req.items():
        if name == "me" and xuid:     # "me" is the caller: key members by xuid so several clients can share a session
            name = xuid
        if m is not None and not isinstance(m, dict):
            return 400, None
    for name, m in members_req.items():
        if name == "me" and xuid:
            name = xuid
        if m is not None and xuid:
            # the advertised identity comes from the authenticated caller, for "me" and for an explicit own entry alike
            consts = m.setdefault("constants", {})
            if not isinstance(consts, dict) or not isinstance(consts.get("system", {}), dict):
                return 400, None
            consts.setdefault("system", {})["xuid"] = xuid
        if m is None:
            rec["members"].pop(name, None)
            continue
        cur = rec["members"].setdefault(name, {})
        _merge(cur, m)
    rec["change"] += 1
    _touch(rec, xuid)
    if "qos" not in key[1].lower():
        # A game-mode session the game created itself (Car Meet -> Free Roam / Road Trip): it asks the game-server platform
        # for a server and reads the address from the session, so list ours; and put its members into this session's world.
        if not rec.get("servers"):
            rec["servers"] = servers_block()
        for k in rec["members"]:
            if k.isdigit() and k == xuid:
                announce_player(k, key[2])
    # The real service answers 201 for a new session; XSAPI's REST layer in this build returns a null session for it,
    # so create and update both answer 200 (docs/matchmaking.md).
    return 200, session_document(rec)


def get_session(path, xuid=None):
    key = parse_path(path)
    if key is None:
        return 404, None
    rec = _SESSIONS.get(tuple(x.lower() for x in key))
    if rec is None:
        return 404, None
    _touch(rec, xuid)
    _prune(rec)
    return 200, session_document(rec)


def delete_session(path, xuid=None):
    """Returns the http status. The owner deletes the session; another member of it only leaves; anybody else is refused.
    The server's own calls (xuid None) are not restricted."""
    key = parse_path(path)
    if key is None:
        return 204
    k = tuple(x.lower() for x in key)
    rec = _SESSIONS.get(k)
    if rec is None:
        return 204
    if xuid and rec.get("owner") != xuid:
        if xuid not in rec["members"]:
            return 403
        rec["members"].pop(xuid, None)
        rec["change"] += 1
        return 204
    _SESSIONS.pop(k, None)
    return 204


def etag(doc):
    """Strong ETag for a session document (the real service sends one with every session)."""
    return '"%s"' % (doc.get("changeNumber", 0),)


def count():
    return len(_SESSIONS)


# ---- Xbox Live matchmaking (momatch.xboxlive.com) ---------------------------------------------------------------------
# The game's XSAPI creates a ticket with POST /Serviceconfigs/{scid}/hoppers/{hopper} {ticketSessionRef, ticketAttributes, ...}
# (contract 103), polls GET .../hoppers/{hopper}/tickets/{id} and reads targetSessionRef once the status is "Found".
# Every ticket is matched at once to a lobby session of this server (one per hopper/location for now).

MOMATCH_PATH = re.compile(r"^/serviceconfigs/(?P<scid>[^/]+)/hoppers/(?P<hopper>[^/?]+)(?:/tickets/(?P<id>[^/?]*))?",
                          re.IGNORECASE)
TARGET_TEMPLATE = "defaultTemplate"

_TICKETS = {}   # ticket id -> record
_LOBBIES = {}   # (scid, hopper) -> session name


def announce_player(xuid, lobby):
    """Tell the game server which lobby (= world) this player was matched into: UDP text to its control port
    (env FH2MP_SERVER_CONTROL, default 127.0.0.1:5090; the test server on 5001 listens on 5091)."""
    if not xuid:
        return
    import socket
    target = os.environ.get("FH2MP_SERVER_CONTROL", "127.0.0.1:5090").rsplit(":", 1)
    try:
        so = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        so.sendto(("assign %x %s" % (int(xuid), lobby)).encode(), (target[0], int(target[1])))
        so.close()
    except (OSError, ValueError):
        pass


def _control(text):
    """One text datagram to the game server's control port (loopback)."""
    import socket
    target = os.environ.get("FH2MP_SERVER_CONTROL", "127.0.0.1:5090").rsplit(":", 1)
    try:
        so = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        so.sendto(text.encode(), (target[0], int(target[1])))
        so.close()
    except (OSError, ValueError):
        pass


def announce_auth(xuid, ip):
    """Tell the game server that `xuid` is signed in from `ip` (the UDP OpenConnection must come from there), see fh2mp_server.Hub.control."""
    if not xuid or not ip:
        return
    _control("auth %x %s" % (int(xuid), ip))


def announce_allow(xuid):
    """A fresh sign-in: lifts an earlier revoke of this player on the game server."""
    if xuid:
        _control("allow %x" % int(xuid))


def announce_revoke(xuid):
    """Ban or kick: the game server forgets the sign-in and ends the player's live connections."""
    if xuid:
        _control("revoke %x" % int(xuid))


def drop_player(xuid):
    """Takes a player out of every session and ticket (ban / kick)."""
    xuid = str(xuid)
    for rec in _SESSIONS.values():
        if rec["members"].pop(xuid, None) is not None:
            rec["change"] += 1
    for tid in [t for t, rec in _TICKETS.items() if rec.get("xuid") == xuid]:
        del _TICKETS[tid]


def parse_momatch(path):
    m = MOMATCH_PATH.match(path.split("?")[0])
    return (m.group("scid"), m.group("hopper"), m.group("id")) if m else None


MEMBER_IDLE_SECONDS = 60.0         # a matched player that never joins the session (crash, closed window) drops out of it
ACTIVE_MEMBER_IDLE_SECONDS = 1800.0   # a player that joined (active) is in the game world and does not poll the session there:
                                      # dropping it after a minute made the other clients see themselves removed and leave (a "kick"),
                                      # and left the lobby empty for the next player who was matched into it


def _is_active(m):
    return bool(((m or {}).get("properties") or {}).get("system", {}).get("active"))


def _prune(rec):
    """Forget members that have not touched the session for a while (so a crashed client does not hold a seat)."""
    now = time.time()
    seen = rec.setdefault("seen", {})
    for k in list(rec["members"]):
        limit = ACTIVE_MEMBER_IDLE_SECONDS if _is_active(rec["members"][k]) else MEMBER_IDLE_SECONDS
        if k.isdigit() and now - seen.get(k, now) > limit:
            rec["members"].pop(k, None)
            rec["change"] += 1


def _touch(rec, xuid):
    if xuid and xuid in rec["members"]:
        rec.setdefault("seen", {})[xuid] = time.time()


def lobby_key(scid, hopper, attrs):
    """Players share a lobby when they asked for the same hopper, game-server image set and location (each Car Meet car park,
    each Free Roam region / island, ... is its own set of lobbies)."""
    return (scid, hopper, str(attrs.get("gsiSetId", "")), str(attrs.get("Location", "")))


def servers_block():
    """The allocated game server, as the game reads it (FUN_14183b360): servers.gameServerPlatform.properties.custom."""
    return {"gameServerPlatform": {"properties": {"custom": {
        "status": "Active", "datacenter": "North Central US",
        "secureDeviceAddress": base64.b64encode(b"FH2MP-SDA-0000000").decode(),
        "hostName": os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")}}}}


def _lobby_for(scid, hopper, attrs, ref):
    key = lobby_key(scid, hopper, attrs)
    name = _LOBBIES.get(key)
    if name is not None:
        rec = _SESSIONS.get((scid.lower(), TARGET_TEMPLATE.lower(), name.lower()))
        if rec is not None:
            _prune(rec)
        cap = int((rec or {}).get("constants", {}).get("custom", {}).get("gameServerPlatform", {}).get("maxAllowedPlayers", 12))
        if rec is None or len(rec["members"]) >= cap:
            name = None   # lobby gone or full: start another one
    if name is None:
        name = str(uuid.uuid4())
        _LOBBIES[key] = name
        # The game reads constants.custom.gameServerPlatform.{gameVariantId,gsiSetId[,location]} from the target session
        # (FUN_14183bb50) and throws on a missing key, so take them from the ticket session it wrote itself.
        custom = {}
        if ref:
            src = _SESSIONS.get((str(ref.get("scid", "")).lower(), str(ref.get("templateName", "")).lower(),
                                 str(ref.get("name", "")).lower()))
            if src:
                custom = copy.deepcopy(src["constants"].get("custom", {}))
        gsp = custom.setdefault("gameServerPlatform", {})
        gsp.setdefault("gameVariantId", "00000000-0000-0000-0000-000000000001")
        gsp.setdefault("gsiSetId", attrs.get("gsiSetId", "00000000-0000-0000-0000-000000000102"))
        gsp.setdefault("maxAllowedPlayers", 12)
        custom.setdefault("hopperId", attrs.get("hopperId", 0))
        body = {"constants": {"system": {"version": 1, "visibility": "open"}, "custom": custom},
                "properties": {"system": {"joinRestriction": "none", "readRestriction": "none"}, "custom": {}}}
        put_session("/serviceconfigs/%s/sessionTemplates/%s/sessions/%s" % (scid, TARGET_TEMPLATE, name),
                    json.dumps(body).encode())
        # The allocated game server, as the game reads it (FUN_14183b360): servers.gameServerPlatform.properties.custom
        # {status Active|Queued|Error|Quarantined, datacenter, secureDeviceAddress, hostName}.
        rec = _SESSIONS.get((scid.lower(), TARGET_TEMPLATE.lower(), name.lower()))
        if rec is not None:
            rec["servers"] = servers_block()
    return name


def create_ticket(path, body, xuid=None):
    key = parse_momatch(path)
    if key is None:
        return 400, None
    scid, hopper, _ = key
    try:
        req = json.loads(body.decode("utf-8")) if body else {}
    except ValueError:
        return 400, None
    ref = req.get("ticketSessionRef")
    if xuid and ref:
        # the ticket may only name a session the caller is in: it is the caller's own request to be matched
        src = _SESSIONS.get((str(ref.get("scid", "")).lower(), str(ref.get("templateName", "")).lower(),
                             str(ref.get("name", "")).lower())) if isinstance(ref, dict) else None
        if src is None or (xuid not in src["members"] and src.get("owner") != xuid):
            return 403, None
    tid = str(uuid.uuid4())
    _TICKETS[tid] = {"scid": scid, "hopper": hopper, "ref": req.get("ticketSessionRef"),
                     "attrs": req.get("ticketAttributes") or {}, "preserve": req.get("preserveSession", "never"),
                     "xuid": xuid}
    return 200, {"ticketId": tid, "waitTime": 1}


def get_ticket(path, xuid=None):
    key = parse_momatch(path)
    t = _TICKETS.get(key[2]) if key else None
    if t is None or (xuid and t.get("xuid") and t["xuid"] != xuid):
        return 404, None
    name = _lobby_for(t["scid"], t["hopper"], t["attrs"], t["ref"])
    # The matchmaker puts the ticket session's members into the target session; the game checks that it is in there.
    ref = t["ref"] or {}
    src = _SESSIONS.get((str(ref.get("scid", "")).lower(), str(ref.get("templateName", "")).lower(),
                         str(ref.get("name", "")).lower()))
    dst = _SESSIONS.get((t["scid"].lower(), TARGET_TEMPLATE.lower(), name.lower()))
    if src and dst:
        for k, m in src["members"].items():
            if m is not None and (not t.get("xuid") or k == t["xuid"]):     # a ticket matches its creator, not whoever else sits in the session
                if k not in dst["members"]:
                    mm = copy.deepcopy(m)
                    # matched members start inactive; the game then PUTs properties.system.active = true
                    mm.setdefault("properties", {}).setdefault("system", {})["active"] = False
                    dst["members"][k] = mm
                    dst["change"] += 1
                    _touch(dst, k)
    # the momatch calls carry no caller identity, but the ticket session's member is the player that asked
    for k in (src["members"] if src else {}):
        if k.isdigit() and (not t.get("xuid") or k == t["xuid"]):
            announce_player(k, name)
    announce_player(t.get("xuid"), name)
    return 200, {"ticketStatus": "Found", "waitTime": 0, "preserveSession": t["preserve"],
                 "ticketSessionRef": t["ref"],
                 "targetSessionRef": {"scid": t["scid"], "templateName": TARGET_TEMPLATE, "name": name},
                 "ticketAttributes": t["attrs"]}


def delete_ticket(path, xuid=None):
    key = parse_momatch(path)
    t = _TICKETS.get(key[2]) if key and key[2] else None
    if t is not None and not (xuid and t.get("xuid") and t["xuid"] != xuid):
        _TICKETS.pop(key[2], None)
    return 204


def create_game_session(mode, xuids=()):
    """A Free Roam (mode 1) or Road Trip (mode 2) session made on the server's request (Car Meet game-mode invite). Same shape
    as a matched lobby: gameServerPlatform constants, our game server in servers. Returns (name, template)."""
    scid = "788f0100-a4bc-46c3-b19b-d1001a96545b"
    name = str(uuid.uuid4())
    gsi = "00000000-0000-0000-0000-00000000010%d" % (0 if mode == 1 else 2)
    body = {"constants": {"system": {"version": 1, "visibility": "open"},
                          "custom": {"hopperId": 0, "EnableFullArbitration": True,
                                     "gameServerPlatform": {"gameVariantId": "00000000-0000-0000-0000-000000000001",
                                                            "gsiSetId": gsi, "maxAllowedPlayers": 12}}},
            "properties": {"system": {"joinRestriction": "none", "readRestriction": "none"}, "custom": {}}}
    put_session("/serviceconfigs/%s/sessionTemplates/%s/sessions/%s" % (scid, TARGET_TEMPLATE, name), json.dumps(body).encode())
    rec = _SESSIONS.get((scid.lower(), TARGET_TEMPLATE.lower(), name.lower()))
    if rec is not None:
        rec["servers"] = servers_block()
        # like the matchmaker does for a match, put the invited players into the session as inactive members: the game
        # looks for itself there and then PUTs properties.system.active = true (without it, it never joins and leaves again)
        for x in xuids:
            rec["members"][x] = {"constants": {"system": {"xuid": x}}, "properties": {"system": {"active": False}}}
            rec["change"] += 1
            _touch(rec, x)
    return name, TARGET_TEMPLATE
