"""Bot players for capacity and HUD tests on the dev server: they fill a Free Roam world past 12 so the party HUD, the relay load and the matchmaker can be
watched with the real clients a tester has.

A bot is a Peer that is NOT in Server.peers (that dict is every loop's idea of a client with a socket), so nothing sends to it and nothing times it out. It
lives in Server.bots and is seen by the few places that must count it: introducing a newcomer (replay_to), relay owner ids, the world's size, the lobby
populations and the status. What it shows the others comes from one real player, its source:
  - its objects (the player, the car objects, the console: routers 1, 2 and 3) are a copy of the source's under a relay owner id of its own, with the
    source's user id replaced by one of the bot's own,
  - its car is the source's live CarPredictionFull moved sideways (a fixed offset on a ring around the source), so it drives wherever the source drives and
    its timestamps are as fresh as the source's.
Nothing is made up that a real client did not send: the objects and the physics are the source's, only the identity and the place differ.

Off unless FH2MP_BOTS=1, and never when FH2MP_ENV=prod. The control port takes `bots add <n> [lobby]`, `bots clear [lobby]` and `bots status`."""
import math
import os
import struct
import sys
import time

BOT_XUID_BASE = 0x9F00000000000      # far from the test accounts (0x90000...) and from real Xbox ids
CLONE_ROUTERS = (1, 2, 3)            # LobbyPlayer, the car objects (ForzaCarNetworked), ConsoleSynced
RING_START_M, RING_STEP_M, RING_SIZE = 30.0, 12.0, 8   # bots sit on rings of 8 around the source: the first 30 m out, 12 m more for each ring
# not closer: they trail the source by its latency (8 m at 200 km/h and 150 ms) and a bot that touches the real car throws it (dev 2026-11-03)
MAX_BOTS = 75
FREE_ROAM_TYPE = 2
POSITION_BITS = 96                   # the first 96 bits of a CarPredictionFull body are x, y, z as f32 (car_position in fh2mp_server.py)


def enabled() -> bool:
    return os.environ.get("FH2MP_BOTS") == "1" and os.environ.get("FH2MP_ENV") != "prod"


def _s(server):
    """The module the running server was loaded from (it can be __main__, so it is not imported by name)."""
    return sys.modules[type(server).__module__]


def replace_user_id(payload: int, size: int, old: int, new: int):
    """Every 64-bit window of an object payload equal to `old` becomes `new` (like fh2mp_server.rewrite_xuid, for any id). -> (payload, count)."""
    mask = (1 << 64) - 1
    found, off = 0, 0
    while off + 64 <= size:
        if (payload >> off) & mask == old:
            payload = (payload & ~(mask << off)) | (new << off)
            found += 1
            off += 64
        else:
            off += 1
    return payload, found


def ring_offset(k: int):
    """(dx, dz) in metres of the k-th bot of a source: 8 to a ring, the first ring RING_START_M out and RING_STEP_M between rings."""
    radius = RING_START_M + RING_STEP_M * (k // RING_SIZE)
    angle = 2 * math.pi * (k % RING_SIZE) / RING_SIZE
    return radius * math.cos(angle), radius * math.sin(angle)


def shifted_body(body: int, dx: float, dz: float) -> int:
    """A CarPredictionFull body with its position moved by (dx, 0, dz); every other bit, the timestamp and the universe among them, is the source's."""
    mask = (1 << POSITION_BITS) - 1
    x, y, z = struct.unpack("<3f", (body & mask).to_bytes(12, "little"))
    if not all(math.isfinite(v) for v in (x, y, z)):
        return body
    moved = int.from_bytes(struct.pack("<3f", x + dx, y, z + dz), "little")
    return (body & ~mask) | moved


def real_peers(server):
    return [p for p in server.peers.values() if p.connected]


def sources(server):
    """Players a bot can copy: entered, with their car and the objects a bot needs (and a user id to replace)."""
    out = []
    for p in server.peers.values():
        if not (p.connected and p.entered and p.xuid and p.car_id is not None and p.car_prediction is not None):
            continue
        if any(router == 1 for (router, _oid) in p.objs) and any(router == 2 for (router, _oid) in p.objs):
            out.append(p)
    return out


def _clone_objects(src, bot):
    count = 0
    for (router, oid), hist in src.objs.items():
        if router not in CLONE_ROUTERS:
            continue
        entries = []
        for (mtype, tid, seq, size, payload) in hist:
            payload, n = replace_user_id(payload, size, src.xuid, bot.xuid)
            count += n
            entries.append((mtype, tid, seq, size, payload))
        bot.objs[(router, oid)] = entries
        bot.relay_next[(router, oid)] = len(entries)
    return count


def add(server, n: int, limit: int):
    """Adds up to n bots to this world, never more than `limit` players in all. -> (added, why not more or "")."""
    S = _s(server)
    srcs = sources(server)
    if not srcs:
        return 0, "no player here has entered with a car yet"
    room = min(n, MAX_BOTS - len(server.bots), limit - len(real_peers(server)) - len(server.bots))
    added, why = 0, ""
    for _ in range(max(room, 0)):
        owner = server.free_owner()
        if owner is None:
            why = "no relay owner id is free"
            break
        k = len(server.bots)
        src = srcs[k % len(srcs)]
        server.bot_serial += 1
        bot = S.Peer(("bot", server.bot_serial))
        bot.xuid, bot.owner_id = BOT_XUID_BASE + server.bot_serial, owner
        bot.world, bot.lobby_type = src.world, src.lobby_type
        bot.connected = bot.entered = bot.introduced = True
        bot.car_id, bot.model_id = src.car_id, src.model_id
        bot.connected_at = bot.last_rx = time.monotonic()
        bot.bot_source, bot.bot_offset = src, ring_offset(k)
        _clone_objects(src, bot)
        server.bots.append(bot)
        for q in server.others(bot):
            server.replay_from(q, bot)
        follow(server, src, src.car_prediction, only=bot)
        added += 1
        server.log(f"bots: bot {bot.xuid:#x} added as owner {owner}, following owner {src.owner_id}, {bot.bot_offset[0]:.0f}/{bot.bot_offset[1]:.0f} m away, "
                   f"{len(bot.objs)} objects; {len(real_peers(server)) + len(server.bots)} players here")
    if added < n and not why:
        why = (f"the world holds {limit} players" if limit - len(real_peers(server)) - len(server.bots) <= 0 else f"the limit is {MAX_BOTS} bots")
    return added, why


def remove(server, bot, why: str = "removed") -> None:
    """The bot's objects are deleted for everybody and its owner id rests like a leaver's."""
    S = _s(server)
    if bot in server.bots:
        server.bots.remove(bot)
    for (router, oid) in bot.objs:
        id_bits, _type_bits = S.ROUTER_BITS[router]
        n = bot.relay_next.get((router, oid), 0)
        server.relay_numbered(bot, (router, oid), n, lambda sq, router=router, oid=oid, id_bits=id_bits:
                              S.relay_delete_message(router, oid, bot.owner_id, id_bits, sq))
    server.owner_quarantine[bot.owner_id] = time.monotonic() + S.OWNER_QUARANTINE_S
    bot.connected = False
    server.log(f"bots: bot {bot.xuid:#x} (owner {bot.owner_id}) {why}; {len(server.bots)} bot(s) left")


def clear(server) -> int:
    n = len(server.bots)
    for bot in list(server.bots):
        remove(server, bot, "cleared")
    return n


def follow(server, source, prediction, only=None) -> None:
    """The source's prediction, as each bot that follows it: moved by the bot's offset and relayed under the bot's owner id."""
    if not server.bots or prediction is None:
        return
    cid, body, size = prediction
    for bot in server.bots:
        if bot.bot_source is source and (only is None or bot is only):
            server.relay_prediction(bot, prediction=(cid, shifted_body(body, *bot.bot_offset), size))


def maintain(server) -> None:
    """Once a second from the tick: bots whose source left follow another player, and with nobody left they go, so an empty world can close."""
    if not server.bots:
        return
    now = time.monotonic()
    if now < server.bots_next:
        return
    server.bots_next = now + 1.0
    real = sources(server)
    if not real_peers(server):
        clear(server)
        return
    for bot in list(server.bots):
        src = bot.bot_source
        if src.connected and src.car_prediction is not None and server.peers.get(src.addr) is src:
            continue
        if not real:
            remove(server, bot, "lost its source")
            continue
        bot.bot_source = real[bot.owner_id % len(real)]
        server.log(f"bots: bot {bot.xuid:#x} now follows owner {bot.bot_source.owner_id}")


def status(server) -> str:
    return f"{len(server.bots)} bot(s), {len(real_peers(server))} real player(s)"


def command(hub, parts) -> str:
    """The control port's `bots` command. -> the reply text."""
    if os.environ.get("FH2MP_ENV") == "prod":
        return "refused: bots never run on production"
    if os.environ.get("FH2MP_BOTS") != "1":
        return "off: set FH2MP_BOTS=1 on this server to use bots"
    verb = parts[1] if len(parts) > 1 else "status"
    arg = parts[2] if len(parts) > 2 else ""
    lobby = parts[3] if len(parts) > 3 else (parts[2] if verb in ("clear", "status") and len(parts) > 2 else "")
    S = _s(hub)
    worlds = [(key, w) for key, w in hub.worlds.items() if key[0] == FREE_ROAM_TYPE and (not lobby or key[1] == lobby)]
    if verb in ("clear", "status"):
        if verb == "clear":
            return f"cleared {sum(clear(w) for _key, w in worlds)} bot(s)"
        return "; ".join(f"{key[1] or 'default'}: {status(w)}" for key, w in worlds) or "no Free Roam world"
    if verb == "add":
        try:
            n = int(arg)
        except ValueError:
            return "usage: bots add <n> [lobby] | bots clear [lobby] | bots status"
        worlds = [(key, w) for key, w in worlds if real_peers(w)]
        if not worlds:
            return "no Free Roam world with a player in it" + (f" named {lobby}" if lobby else "")
        key, w = max(worlds, key=lambda kw: len(real_peers(kw[1])))
        added, why = add(w, n, S.capacity.for_type(FREE_ROAM_TYPE))
        return f"added {added} of {n} in {key[1] or 'default'}" + (f" ({why})" if why else "") + f"; {status(w)}"
    return "usage: bots add <n> [lobby] | bots clear [lobby] | bots status"
