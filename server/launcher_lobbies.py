"""Launcher seat leases. All state is protected by xbl._LOBBY_LOCK.

A hold only influences the next ordinary matchmaking request; it never moves a
connected client. New-lobby requests are materialised using that client's real
ticket attributes, so we do not invent a game session configuration.
"""
import time
import uuid

import auth
import capacity
import diag
import xbl

LOG = diag.get("lobbies")
_RATE = diag.RateLimit()

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
            LOG.info("reservation expired unused " + diag.kv(xuid=x, lobby=str(h["id"])[:8], kind=h["kind"], held_s=HOLD_SECONDS))


def seats(name, present=(), now=None):
    prune(now)
    return {x for x, h in HOLDS.items() if h["id"] == name} - set(present)


def current(xuid):
    with xbl._LOBBY_LOCK:
        prune()
        return dict(HOLDS[str(xuid)]) if str(xuid) in HOLDS else None


def cancel(xuid):
    with xbl._LOBBY_LOCK:
        h = HOLDS.pop(str(xuid), None)
    if h:
        LOG.info("reservation cancelled by the player " + diag.kv(xuid=xuid, lobby=str(h["id"])[:8], kind=h["kind"]))


def reserve(xuid, lobby=None, kind=None):
    if kind is not None and kind not in ("freeroam", "roadtrip"):
        raise auth.AuthError(400, "bad_mode", "Choose Free Roam or Road Trip.", xuid=xuid, detail=f"kind {str(kind)[:20]!r}")
    with xbl._LOBBY_LOCK:
        prune()
        pops = xbl.hub_populations()
        if pops is None:
            raise auth.AuthError(503, "unavailable", "The game server did not answer. Try again shortly.", xuid=xuid, detail="population query got no answer")
        flagged = xbl.flagged_set(pops)
        key = None
        if lobby:
            for k, names in xbl._LOBBIES.items():
                if lobby in names:
                    key = k
                    break
            if key is None or lobby_kind(key) is None:
                raise auth.AuthError(409, "lobby_gone", "This lobby is no longer available.", xuid=xuid,
                                     detail=f"lobby {str(lobby)[:8]} " + ("is not in the lobby index" if key is None else f"is of hopper {key[1]!r}, which is neither FreeRoam nor RoadTrip"))
            kind = lobby_kind(key)
            rec = xbl._SESSIONS.get((key[0].lower(), xbl.TARGET_TEMPLATE.lower(), lobby.lower()))
            if rec is None:
                raise auth.AuthError(409, "lobby_gone", xuid=xuid, detail=f"lobby {str(lobby)[:8]} is indexed but its MPSD session is gone")
            occupied = xbl.lobby_occupancy(lobby, rec, pops)
            if not xbl.pool_fits(lobby, pops, flagged, int(xuid) in flagged):
                raise auth.AuthError(409, "lobby_unavailable", "This lobby is unavailable for your player pool.", xuid=xuid,
                                     detail=f"lobby {str(lobby)[:8]}: pool does not fit (player flagged={int(xuid) in flagged})")
            already = str(xuid) in seats(lobby) or f"{int(xuid):x}" in pops.get(lobby, [])
            room = capacity.for_kind(kind)
            if occupied >= min(xbl._max_players(rec, room), room) and not already:
                raise auth.AuthError(409, "lobby_full", "This lobby is full.", xuid=xuid,
                                     detail=f"lobby {str(lobby)[:8]} holds {occupied} of {min(xbl._max_players(rec, room), room)}")
        elif kind:
            lobby = str(uuid.uuid4())
        else:
            raise auth.AuthError(400, "bad_request", xuid=xuid, detail="neither a lobby nor a kind was given")
        previous = HOLDS.get(str(xuid))
        h = {"id": lobby, "kind": kind, "expiresAt": int(time.time()) + HOLD_SECONDS, "key": key}
        HOLDS[str(xuid)] = h
    LOG.info("reservation made " + diag.kv(xuid=xuid, lobby=str(lobby)[:8], kind=kind, existing=key is not None, replaces=str(previous["id"])[:8] if previous else None,
                                           hold_s=HOLD_SECONDS))
    return dict(h)


def pick(xuid, key, attrs, ref, pops):
    """Called under the matchmaking lock. Returns a compatible held lobby, or None."""
    prune()
    h = HOLDS.get(str(xuid))
    if not h:
        return None
    if lobby_kind(key) != h["kind"]:
        _every(("pick-kind", xuid, key[1]), f"reservation not used for this ticket: hopper {key[1]!r} is not the held {h['kind']} " + diag.kv(xuid=xuid, lobby=str(h["id"])[:8]))
        return None
    if h["key"] is not None and tuple(h["key"]) != key:
        _every(("pick-key", xuid, key), "reservation not used for this ticket: " + diag.kv(xuid=xuid, lobby=str(h["id"])[:8], held_template=h["key"][0], ticket_template=key[0],
                                                                                          held_hopper=h["key"][1], ticket_hopper=key[1]))
        return None                    # another island/location must not consume this hold
    if pops is None:
        raise auth.AuthError(503, "unavailable", "Cannot verify the reserved lobby right now.", xuid=xuid, detail="population query got no answer during matchmaking")
    name = h["id"]
    if h["key"] is None:
        xbl._new_lobby(key[0], attrs, ref, name=name, hopper=key[1])
        xbl._LOBBIES.setdefault(key, []).append(name)
        LOG.info("reservation used: new world made for it " + diag.kv(xuid=xuid, lobby=str(name)[:8], kind=h["kind"]))
    else:
        rec = xbl._SESSIONS.get((key[0].lower(), xbl.TARGET_TEMPLATE.lower(), name.lower()))
        flags = xbl.flagged_set(pops)
        occupied = xbl.lobby_occupancy(name, rec, pops) if rec is not None else 0
        fits = rec is not None and xbl.pool_fits(name, pops, flags, int(xuid) in flags)
        if rec is None or not fits:
            HOLDS.pop(str(xuid), None)
            LOG.warn("reservation dropped, the player is matched normally: " + diag.kv(
                xuid=xuid, lobby=str(name)[:8], why="the world's session is gone" if rec is None else "the world is not in this player's pool", flagged=int(xuid) in flags))
            return None
        # The held seat is already counted in occupancy.
        room = capacity.for_kind(h["kind"])
        if occupied > min(xbl._max_players(rec, room), room):
            HOLDS.pop(str(xuid), None)
            LOG.warn("reservation dropped, the player is matched normally: " + diag.kv(
                xuid=xuid, lobby=str(name)[:8], why="the world filled up since", occupied=occupied, max=min(xbl._max_players(rec, room), room)))
            return None
        LOG.info("reservation used: joins the held world " + diag.kv(xuid=xuid, lobby=str(name)[:8], kind=h["kind"], occupied=occupied))
    HOLDS.pop(str(xuid), None)
    return name


def _every(key, msg, interval=60.0):
    """A ticket is polled every few seconds: a reason that keeps holding is written once a minute per player."""
    ok, skipped = _RATE.allow(key, interval)
    if ok:
        LOG.info(msg + (f" (and {skipped} more tickets in the last {interval:.0f} s)" if skipped else ""))
