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
    constants["system"].setdefault("xuid", key if key.isdigit() else "0")
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


def put_session(path, body, xuid=None):
    """Create or update a session. Returns (http status, document or None)."""
    key = parse_path(path)
    if key is None:
        return 400, None
    try:
        req = json.loads(body.decode("utf-8")) if body else {}
    except ValueError:
        return 400, None
    k = tuple(x.lower() for x in key)
    rec = _SESSIONS.get(k)
    created = rec is None
    if created:
        rec = {"scid": key[0], "template": key[1], "name": key[2], "corr": str(uuid.uuid4()),
               "branch": str(uuid.uuid4()), "start": _now(), "change": 0,
               "constants": {}, "properties": {}, "members": {}, "servers": {}}
        _SESSIONS[k] = rec
    if "constants" in req:
        _merge(rec["constants"], req["constants"])
    if "properties" in req:
        _merge(rec["properties"], req["properties"])
    for name, m in (req.get("members") or {}).items():
        if name == "me" and xuid:     # "me" is the caller: key members by xuid so several clients can share a session
            name = xuid
            if isinstance(m, dict):
                m.setdefault("constants", {}).setdefault("system", {})["xuid"] = xuid
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


def delete_session(path):
    key = parse_path(path)
    if key is not None:
        _SESSIONS.pop(tuple(x.lower() for x in key), None)
    return 200


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


def parse_momatch(path):
    m = MOMATCH_PATH.match(path.split("?")[0])
    return (m.group("scid"), m.group("hopper"), m.group("id")) if m else None


MEMBER_IDLE_SECONDS = 60.0   # a player that stops polling the lobby session (crash, closed window) drops out of it


def _prune(rec):
    """Forget members that have not touched the session for a while (so a crashed client does not hold a seat)."""
    now = time.time()
    seen = rec.setdefault("seen", {})
    for k in list(rec["members"]):
        if k.isdigit() and now - seen.get(k, now) > MEMBER_IDLE_SECONDS:
            rec["members"].pop(k, None)
            rec["change"] += 1


def _touch(rec, xuid):
    if xuid:
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
    tid = str(uuid.uuid4())
    _TICKETS[tid] = {"scid": scid, "hopper": hopper, "ref": req.get("ticketSessionRef"),
                     "attrs": req.get("ticketAttributes") or {}, "preserve": req.get("preserveSession", "never"),
                     "xuid": xuid}
    return 200, {"ticketId": tid, "waitTime": 1}


def get_ticket(path):
    key = parse_momatch(path)
    t = _TICKETS.get(key[2]) if key else None
    if t is None:
        return 404, None
    name = _lobby_for(t["scid"], t["hopper"], t["attrs"], t["ref"])
    # The matchmaker puts the ticket session's members into the target session; the game checks that it is in there.
    ref = t["ref"] or {}
    src = _SESSIONS.get((str(ref.get("scid", "")).lower(), str(ref.get("templateName", "")).lower(),
                         str(ref.get("name", "")).lower()))
    dst = _SESSIONS.get((t["scid"].lower(), TARGET_TEMPLATE.lower(), name.lower()))
    if src and dst:
        for k, m in src["members"].items():
            if m is not None:
                if k not in dst["members"]:
                    mm = copy.deepcopy(m)
                    # matched members start inactive; the game then PUTs properties.system.active = true
                    mm.setdefault("properties", {}).setdefault("system", {})["active"] = False
                    dst["members"][k] = mm
                    dst["change"] += 1
                    _touch(dst, k)
    # the momatch calls carry no caller identity, but the ticket session's member is the player that asked
    for k in (src["members"] if src else {}):
        if k.isdigit():
            announce_player(k, name)
    announce_player(t.get("xuid"), name)
    return 200, {"ticketStatus": "Found", "waitTime": 0, "preserveSession": t["preserve"],
                 "ticketSessionRef": t["ref"],
                 "targetSessionRef": {"scid": t["scid"], "templateName": TARGET_TEMPLATE, "name": name},
                 "ticketAttributes": t["attrs"]}


def delete_ticket(path):
    key = parse_momatch(path)
    if key and key[2]:
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
