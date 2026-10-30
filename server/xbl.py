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
import threading
import time
import uuid

SESSION_PATH = re.compile(
    r"^/serviceconfigs/(?P<scid>[^/]+)/sessiontemplates/(?P<template>[^/]+)/sessions/(?P<name>[^/?]+)",
    re.IGNORECASE)

_SESSIONS = {}   # (scid, template, name) lowercase -> record
_SESSIONS_LOCK = threading.RLock()     # PUTs come from several handler threads: an update is built on a copy and swapped in under this
MAX_SESSIONS_PER_OWNER = 32      # live sessions one player may have created; the game makes a few per mode change and they expire
MAX_SESSIONS = 20000
MAX_SESSION_BYTES = 256 * 1024   # a stored record as JSON (constants, properties, members, servers)
SESSION_IDLE_SECONDS = 6 * 3600.0   # a session nobody read, wrote or matched into for this long is forgotten (players in a world
EMPTY_SESSION_IDLE_SECONDS = 900.0  # do not poll it); one without members goes sooner
TICKET_SECONDS = 900.0
MAX_TICKETS_PER_PLAYER = 8
TICKET_WAIT_TIME = int(os.environ.get("FH2MP_TICKET_WAIT", "0"))
_last_sweep = 0.0
NAME_LOOKUP = lambda xuid: ""   # xuid -> gamertag, set by fh2mp_lsp; the real MPSD fills a member's gamertag itself, the game never sends it


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


def _member_name(key):
    """The registered gamertag of the member whose entry is keyed by this xuid (GitHub #3: the game shows what the session says)."""
    if not key.isdigit():
        return ""
    try:
        return NAME_LOOKUP(int(key)) or ""
    except Exception:  # noqa: BLE001 - a failed lookup must never break a session document
        return ""


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
        "gamertag": _member_name(key) or m.get("gamertag") or "Player 1",   # the registered name wins over anything a client sends
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
    system.setdefault("maxMembersCount", _max_players(rec, 100))
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


def _int_or_none(v):
    """An int from a JSON number or a numeric string ("12" is what the GSDS reply hands the game), else None."""
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().isdigit():
        return int(v.strip())
    return None


def _max_players(rec, default):
    """constants.custom.gameServerPlatform.maxAllowedPlayers of a record, or `default` when it is missing or not a sane number (a
    record stored before validation existed must stay readable)."""
    gsp = ((rec.get("constants") or {}).get("custom") or {}).get("gameServerPlatform")
    n = _int_or_none(gsp.get("maxAllowedPlayers")) if isinstance(gsp, dict) else None
    return n if n is not None and 1 <= n <= 100 else default


def _valid_shape(req):
    """Types the document builder relies on: constants / properties / servers are objects, their system and custom parts too, and
    maxAllowedPlayers is a number from 1 to 100."""
    for part in ("constants", "properties", "servers"):
        v = req.get(part, {})
        if not isinstance(v, dict):
            return False
        if part != "servers" and any(not isinstance(v.get(sub, {}), dict) for sub in ("system", "custom")):
            return False
    gsp = req.get("constants", {}).get("custom", {}).get("gameServerPlatform", {})
    if not isinstance(gsp, dict):
        return False
    if "maxAllowedPlayers" in gsp:
        n = _int_or_none(gsp["maxAllowedPlayers"])
        if n is None or not 1 <= n <= 100:
            return False
    return True


def sweep(now=None):
    """Forgets sessions nobody used for SESSION_IDLE_SECONDS (EMPTY_SESSION_IDLE_SECONDS without members) and matchmaking tickets
    older than TICKET_SECONDS. Called from the session calls at most once a minute."""
    global _last_sweep
    now = time.time() if now is None else now
    if now - _last_sweep < 60.0:
        return
    _last_sweep = now
    with _SESSIONS_LOCK:
        for k in [k for k, rec in _SESSIONS.items()
                  if now - rec.get("used", now) > (SESSION_IDLE_SECONDS if rec["members"] else EMPTY_SESSION_IDLE_SECONDS)]:
            del _SESSIONS[k]
    for tid in [t for t, rec in list(_TICKETS.items()) if now - rec.get("at", now) > TICKET_SECONDS]:
        _TICKETS.pop(tid, None)


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
    if not _valid_shape(req):
        return 400, None
    members_req = req.get("members") or {}
    if not isinstance(members_req, dict) or any(m is not None and not isinstance(m, dict) for m in members_req.values()):
        return 400, None
    for m in members_req.values():
        if m is not None and any(not isinstance(m.get(part, {}), dict) or not isinstance(m.get(part, {}).get("system", {}), dict)
                                 for part in ("constants", "properties")):
            return 400, None
    sweep()
    k = tuple(x.lower() for x in key)
    with _SESSIONS_LOCK:
        cur = _SESSIONS.get(k)
        created = cur is None
        if xuid:
            # decide everything before changing anything, so a refused request leaves the session as it was
            if any(not _own_member_key(name, xuid) for name in members_req):
                return 403, None                    # nobody writes or removes somebody else's member entry
            if not created:
                is_member = xuid in cur["members"] or cur.get("owner") == xuid
                joining = any(_own_member_key(name, xuid) and m is not None for name, m in members_req.items())
                if not is_member and not joining:
                    return 403, None
                if not is_member and ("constants" in req or "properties" in req or "servers" in req):
                    return 403, None                # joining does not let a stranger rewrite the session
            elif sum(1 for r in _SESSIONS.values() if r.get("owner") == xuid) >= MAX_SESSIONS_PER_OWNER:
                return 429, None
        if created and len(_SESSIONS) >= MAX_SESSIONS:
            return 503, None
        if created:
            rec = {"scid": key[0], "template": key[1], "name": key[2], "corr": str(uuid.uuid4()),
                   "branch": str(uuid.uuid4()), "start": _now(), "change": 0, "owner": xuid or None,
                   "constants": {}, "properties": {}, "members": {}, "servers": {}}
        else:
            rec = copy.deepcopy(cur)                # the update is built on a copy and only stored when it is valid
        if "constants" in req:
            _merge(rec["constants"], req["constants"])
        if "properties" in req:
            _merge(rec["properties"], req["properties"])
        for name, m in members_req.items():
            if name == "me" and xuid:     # "me" is the caller: key members by xuid so several clients can share a session
                name = xuid
            if m is None:
                rec["members"].pop(name, None)
                continue
            m = copy.deepcopy(m)
            if xuid:
                # the advertised identity comes from the authenticated caller, for "me" and for an explicit own entry alike
                m.setdefault("constants", {}).setdefault("system", {})["xuid"] = xuid
            _merge(rec["members"].setdefault(name, {}), m)
        rec["change"] += 1
        rec["used"] = time.time()
        _touch(rec, xuid)
        if "qos" not in key[1].lower():
            # A game-mode session the game created itself (Car Meet -> Free Roam / Road Trip): it asks the game-server platform
            # for a server and reads the address from the session, so list ours; and put its members into this session's world.
            if not rec.get("servers"):
                rec["servers"] = servers_block()
        try:
            doc = session_document(rec)
            if len(json.dumps(rec)) > MAX_SESSION_BYTES:
                return 413, None
        except (TypeError, ValueError, AttributeError):
            return 400, None
        _SESSIONS[k] = rec
    if "qos" not in key[1].lower():
        for mk in rec["members"]:
            if mk.isdigit() and mk == xuid:
                announce_player(mk, key[2])
    # The real service answers 201 for a new session; XSAPI's REST layer in this build returns a null session for it,
    # so create and update both answer 200 (docs/matchmaking.md).
    return 200, doc


def get_session(path, xuid=None):
    key = parse_path(path)
    if key is None:
        return 404, None
    sweep()
    rec = _SESSIONS.get(tuple(x.lower() for x in key))
    if rec is None:
        return 404, None
    rec["used"] = time.time()
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
_LOBBIES = {}   # lobby_key -> [session name, ...]: every lobby of that hopper / location, oldest first
_RESERVED = {}  # session name -> {xuid: time}: players matched into a lobby that the game server has not seen connect yet
_LOBBY_LOCK = threading.Lock()
LOBBY_CAPACITY = 12
RESERVE_SECONDS = 30.0     # a matched player that has not connected after this long no longer holds a seat (the game connects within ~10 s)
_POP_DOWN_UNTIL = 0.0      # the game server did not answer: do not ask again before this


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


def announce_auth(xuid, ip, key=""):
    """Tell the game server that `xuid` is signed in from `ip` (the UDP OpenConnection must come from there) with a session whose UDP
    ticket key is `key` (hex, auth.udp_ticket_key), see fh2mp_server.Hub.control."""
    if not xuid or not ip:
        return
    _control("auth %x %s %s" % (int(xuid), ip, key) if key else "auth %x %s" % (int(xuid), ip))


def announce_logout(xuid, key):
    """A session signed out: the game server forgets its ticket key and ends the connections that key opened (not a ban)."""
    if xuid and key:
        _control("logout %x %s" % (int(xuid), key))


def announce_allow(xuid):
    """A fresh sign-in: lifts an earlier revoke of this player on the game server."""
    if xuid:
        _control("allow %x" % int(xuid))


def announce_revoke(xuid):
    """Ban or kick: the game server forgets the sign-in and ends the player's live connections."""
    if xuid:
        _control("revoke %x" % int(xuid))


def quarantine_tools(xuid, tools):
    """Flag the authenticated sender and wait for the hub before matchmaking can run. No caller-supplied target identity."""
    import socket
    tools = sorted(set(tools))
    if not xuid or not tools or not set(tools) <= {"redline", "nitrosystem"}:
        return None
    target = os.environ.get("FH2MP_SERVER_CONTROL", "127.0.0.1:5090").rsplit(":", 1)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as so:
            so.settimeout(1.0)
            so.connect((target[0], int(target[1])))
            so.send(("tools %x %s" % (int(xuid), ",".join(tools))).encode())
            reply = so.recv(64).decode("ascii")
            return reply if reply in ("ok", "rejoin") else None
    except (OSError, ValueError):
        return None


def drop_player(xuid):
    """Takes a player out of every session and ticket (ban / kick)."""
    xuid = str(xuid)
    import launcher_lobbies
    launcher_lobbies.cancel(xuid)
    for res in _RESERVED.values():
        res.pop(xuid, None)
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


def for_tunnel(doc):
    """The session document as a client sees it when its game talks to the launcher's local UDP-over-TCP proxy (header X-FH2MP-Tunnel):
    the game server is on 127.0.0.1 (the port comes from the launcher through the shim's FH2MP_SERVER_PORT). Edits `doc` in place."""
    custom = (((doc or {}).get("servers") or {}).get("gameServerPlatform") or {}).get("properties", {}).get("custom")
    if isinstance(custom, dict) and "hostName" in custom:
        custom["hostName"] = "127.0.0.1"
    return doc


def servers_block():
    """The allocated game server, as the game reads it (FUN_14183b360): servers.gameServerPlatform.properties.custom."""
    return {"gameServerPlatform": {"properties": {"custom": {
        "status": "Active", "datacenter": "North Central US",
        "secureDeviceAddress": base64.b64encode(b"FH2MP-SDA-0000000").decode(),
        "hostName": os.environ.get("FH2MP_PUBLIC_HOST", "127.0.0.1")}}}}


def hub_populations():
    """{lobby name: [hex xuid, ...]} of the players the game server has in each lobby, asked over its control port
    (a "pop" datagram; see fh2mp_server.Hub.populations). None when it does not answer: the caller then counts MPSD members."""
    global _POP_DOWN_UNTIL
    import socket
    if time.monotonic() < _POP_DOWN_UNTIL:
        return None
    target = os.environ.get("FH2MP_SERVER_CONTROL", "127.0.0.1:5090").rsplit(":", 1)
    so = None
    try:
        so = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        so.settimeout(0.3)
        so.sendto(b"pop", (target[0], int(target[1])))
        data, _ = so.recvfrom(65535)
    except (OSError, ValueError):
        _POP_DOWN_UNTIL = time.monotonic() + 5.0
        return None
    finally:
        if so is not None:
            so.close()
    parts = data.decode("utf-8", "replace").split()
    if not parts or parts[0] != "pop":
        return None
    out = {}
    for item in parts[1:]:
        name, _, xs = item.partition("=")
        out[name] = [x for x in xs.split(",") if x]
    return out


def lobby_occupancy(name, rec, pops, now=None):
    """Players in a lobby, counting the ones matched into it that are still on their way. With the game server's report that is who is
    connected plus the recent matches it has not seen yet; without it, the session's members."""
    now = time.time() if now is None else now
    import launcher_lobbies
    if pops is None:
        members = {k for k in rec["members"] if k.isdigit()}
        return len(members | launcher_lobbies.seats(name, now=now))
    present = {x.lstrip("0") for x in pops.get(name, [])}
    res = _RESERVED.get(name, {})
    for x, t in list(res.items()):
        if now - t > RESERVE_SECONDS or ("%x" % int(x)).lstrip("0") in present:
            del res[x]             # it connected (the report counts it from now on, and a later leave must not leave a ghost) or never came
    held = launcher_lobbies.seats(name, now=now)
    present_decimal = {str(int(x, 16)) for x in pops.get(name, []) if x}
    return len(pops.get(name, [])) + len((set(res) | held) - present_decimal)


def flagged_set(pops):
    """The kicked players the game server reports (its "!flagged" entry), as ints; empty when it is not reachable."""
    out = set()
    for x in (pops or {}).get("!flagged", []):
        try:
            out.add(int(x, 16))
        except ValueError:
            pass
    return out


def pool_fits(name, pops, flagged, mine):
    """Whether a player may be matched into this lobby: a flagged player (kicked by the cheat checks, see fh2mp_server.Hub.flag) only
    into a lobby whose players are all flagged, anybody else only into one with no flagged player in it. Players still on their way
    (_RESERVED) count. An empty lobby fits both; the first to be matched into it decides."""
    members = {int(x, 16) for x in (pops or {}).get(name, []) if x}
    members |= {int(x) for x in _RESERVED.get(name, {}) if str(x).isdigit()}
    import launcher_lobbies
    members |= {int(x) for x in launcher_lobbies.seats(name)}
    members.discard(0)
    if mine:
        return all(m in flagged for m in members)
    return not any(m in flagged for m in members)


def _lobby_for(scid, hopper, attrs, ref, xuid=None):
    """The lobby a ticket is matched into: of the hopper's lobbies the one with the most players that still has room (12 per lobby), so
    that players arriving one after another fill lobbies up (12, 12, 1) instead of leaving every lobby partly empty (8, 10, 6, 1).
    Players leave lobbies that were filled earlier, so the newest lobby is not the only one with seats."""
    key = lobby_key(scid, hopper, attrs)
    with _LOBBY_LOCK:
        names = _LOBBIES.setdefault(key, [])
        pops = hub_populations()
        import launcher_lobbies
        preferred = launcher_lobbies.pick(xuid, key, attrs, ref, pops) if xuid else None
        best, best_n = preferred, LOBBY_CAPACITY if preferred else -1
        flagged = flagged_set(pops)
        mine = int(xuid) in flagged if xuid and str(xuid).isdigit() else False
        for name in list(names):
            rec = _SESSIONS.get((scid.lower(), TARGET_TEMPLATE.lower(), name.lower()))
            if rec is None:
                names.remove(name)
                _RESERVED.pop(name, None)
                continue
            _prune(rec)
            cap = min(_max_players(rec, LOBBY_CAPACITY), LOBBY_CAPACITY)
            n = lobby_occupancy(name, rec, pops)
            if flagged and not pool_fits(name, pops, flagged, mine):
                continue
            if n < cap and n > best_n:
                best, best_n = name, n
        name = best
        if name is None:
            name = _new_lobby(scid, attrs, ref)
            names.append(name)
        rec = _SESSIONS.get((scid.lower(), TARGET_TEMPLATE.lower(), name.lower()))
        if rec is not None:
            rec["used"] = time.time()
        if xuid:
            for res in _RESERVED.values():
                res.pop(str(xuid), None)        # one seat per player: a new match replaces the one it made before
            _RESERVED.setdefault(name, {})[str(xuid)] = time.time()
    return name


def _new_lobby(scid, attrs, ref, name=None):
    name = name or str(uuid.uuid4())
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
    if not isinstance(gsp, dict):
        gsp = custom["gameServerPlatform"] = {}
    gsp["maxAllowedPlayers"] = LOBBY_CAPACITY      # server-owned: the ticket session the client wrote does not set the lobby size
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
    attrs = req.get("ticketAttributes") or {}
    if not isinstance(attrs, dict) or (ref is not None and not isinstance(ref, dict)):
        return 400, None
    sweep()
    if xuid:
        mine = sorted((rec.get("at", 0), t) for t, rec in list(_TICKETS.items()) if rec.get("xuid") == xuid)
        for _at, t in mine[:max(0, len(mine) - MAX_TICKETS_PER_PLAYER + 1)]:
            _TICKETS.pop(t, None)                  # the caller's own oldest tickets make room; nobody else's
    tid = str(uuid.uuid4())
    _TICKETS[tid] = {"scid": scid, "hopper": hopper, "ref": ref,
                     "attrs": attrs, "preserve": req.get("preserveSession", "never"),
                     "xuid": xuid, "at": time.time()}
    # waitTime is the estimated wait in seconds. The game's first poll of the ticket came 5-6 s after the POST whatever it said (1 until
    # 2026-10-29), which is most of a Free Roam search that is otherwise answered instantly: FH2MP_TICKET_WAIT tries other values on dev.
    return 200, {"ticketId": tid, "waitTime": TICKET_WAIT_TIME}


def get_ticket(path, xuid=None):
    key = parse_momatch(path)
    t = _TICKETS.get(key[2]) if key else None
    if t is None or (xuid and t.get("xuid") and t["xuid"] != xuid):
        return 404, None
    # A ticket keeps the lobby it was first matched into: the game polls it every second and the lobby it is sent to must not change
    name = t.get("lobby")
    if name is None or (t["scid"].lower(), TARGET_TEMPLATE.lower(), name.lower()) not in _SESSIONS:
        import auth
        try:
            name = t["lobby"] = _lobby_for(t["scid"], t["hopper"], t["attrs"], t["ref"], t.get("xuid"))
        except auth.AuthError as e:
            return e.status, {"error": e.code, "message": e.message}
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


def hub_lobbies():
    """Live public summaries from the UDP hub, or None when it cannot be reached."""
    import socket
    target = os.environ.get("FH2MP_SERVER_CONTROL", "127.0.0.1:5090").rsplit(":", 1)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as so:
            so.settimeout(0.3)
            so.sendto(b"lobbies", (target[0], int(target[1])))
            data, _ = so.recvfrom(65535)
        result = json.loads(data)
        return result if isinstance(result, dict) and isinstance(result.get("lobbies"), list) else None
    except (OSError, ValueError):
        return None
