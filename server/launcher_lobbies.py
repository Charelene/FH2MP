"""Launcher seat leases. All state is protected by xbl._LOBBY_LOCK.

A hold only influences the next ordinary matchmaking request; it never moves a
connected client. New-lobby requests are materialised using that client's real
ticket attributes, so we do not invent a game session configuration.
"""
import time
import uuid

import auth
import xbl

HOLD_SECONDS = 300
HOLDS = {}                         # decimal xuid -> lease
# Real FreeRoam_qos, RoadTrip_qos and CarMeet_qos tickets all use GSI ...0102.
# GSI identifies the image set, not the requested game mode.
HOPPER_KINDS = {"freeroam_qos": "freeroam", "roadtrip_qos": "roadtrip"}


def lobby_kind(key):
    return HOPPER_KINDS.get(key[1].lower())


def prune(now=None):
    now = time.time() if now is None else now
    for x, h in list(HOLDS.items()):
        if h["expiresAt"] <= now:
            del HOLDS[x]


def seats(name, present=(), now=None):
    prune(now)
    return {x for x, h in HOLDS.items() if h["id"] == name} - set(present)


def current(xuid):
    with xbl._LOBBY_LOCK:
        prune()
        return dict(HOLDS[str(xuid)]) if str(xuid) in HOLDS else None


def cancel(xuid):
    with xbl._LOBBY_LOCK:
        HOLDS.pop(str(xuid), None)


def reserve(xuid, lobby=None, kind=None):
    if kind is not None and kind not in ("freeroam", "roadtrip"):
        raise auth.AuthError(400, "bad_mode", "Choose Free Roam or Road Trip.")
    with xbl._LOBBY_LOCK:
        prune()
        pops = xbl.hub_populations()
        if pops is None:
            raise auth.AuthError(503, "unavailable", "The game server did not answer. Try again shortly.")
        flagged = xbl.flagged_set(pops)
        key = None
        if lobby:
            for k, names in xbl._LOBBIES.items():
                if lobby in names:
                    key = k
                    break
            if key is None or lobby_kind(key) is None:
                raise auth.AuthError(409, "lobby_gone", "This lobby is no longer available.")
            kind = lobby_kind(key)
            rec = xbl._SESSIONS.get((key[0].lower(), xbl.TARGET_TEMPLATE.lower(), lobby.lower()))
            if rec is None:
                raise auth.AuthError(409, "lobby_gone")
            occupied = xbl.lobby_occupancy(lobby, rec, pops)
            if not xbl.pool_fits(lobby, pops, flagged, int(xuid) in flagged):
                raise auth.AuthError(409, "lobby_unavailable", "This lobby is unavailable for your player pool.")
            already = str(xuid) in seats(lobby) or f"{int(xuid):x}" in pops.get(lobby, [])
            if occupied >= min(xbl._max_players(rec, 12), 12) and not already:
                raise auth.AuthError(409, "lobby_full", "This lobby is full.")
        elif kind:
            lobby = str(uuid.uuid4())
        else:
            raise auth.AuthError(400, "bad_request")
        h = {"id": lobby, "kind": kind, "expiresAt": int(time.time()) + HOLD_SECONDS, "key": key}
        HOLDS[str(xuid)] = h
        return dict(h)


def pick(xuid, key, attrs, ref, pops):
    """Called under the matchmaking lock. Returns a compatible held lobby, or None."""
    prune()
    h = HOLDS.get(str(xuid))
    if not h or lobby_kind(key) != h["kind"]:
        return None
    if h["key"] is not None and tuple(h["key"]) != key:
        return None                    # another island/location must not consume this hold
    if pops is None:
        raise auth.AuthError(503, "unavailable", "Cannot verify the reserved lobby right now.")
    name = h["id"]
    if h["key"] is None:
        xbl._new_lobby(key[0], attrs, ref, name=name)
        xbl._LOBBIES.setdefault(key, []).append(name)
    else:
        rec = xbl._SESSIONS.get((key[0].lower(), xbl.TARGET_TEMPLATE.lower(), name.lower()))
        flags = xbl.flagged_set(pops)
        occupied = xbl.lobby_occupancy(name, rec, pops) if rec is not None else 0
        if rec is None or not xbl.pool_fits(name, pops, flags, int(xuid) in flags):
            HOLDS.pop(str(xuid), None)
            return None
        # The held seat is already counted in occupancy.
        if occupied > min(xbl._max_players(rec, 12), 12):
            HOLDS.pop(str(xuid), None)
            return None
    HOLDS.pop(str(xuid), None)
    return name
