"""How many players a world holds. The game server (fh2mp_server.py) and the LSP (fh2mp_lsp.py, xbl.py, launcher_lobbies.py) are separate processes
and both ask this module, so FH2MP_WORLD_CAPACITY has to be set the same in both.

Only Free Roam grows. An event, Road Trip and Car Meet stay at CLIENT_LIMIT: the client's own formats hold 12 (the starting grid's key buffer,
MaxPlayers 1..12, MsgTagBuckets, the Road Trip vote arrays and festival grid). Free Roam past 12 needs the shim's party HUD patch
(shim/src/party_hud.inc) on every client; a client without it crashes when the 13th player arrives (game+0x144661d, issue #47), so the default stays 12.
Above 17 players the private universes run out (PRIVATE_UNIVERSE_COUNT): the extra players get none and the event/free-roam phasing is not kept for them."""
import os

CLIENT_LIMIT = 12
FREE_ROAM_MAX = 72          # FH4's lobby size; the relay's owner ids (1..127 per world) and the HUD patch's 36 rows are the limits above it in practice
FREE_ROAM_TYPE = 2          # Nerd::NetworkLobbyType: 1 RoadTrip, 2 FreeRoam, 3 CarMeet
ENV = "FH2MP_WORLD_CAPACITY"


def free_roam():
    """Players in one Free Roam world: FH2MP_WORLD_CAPACITY from 12 to 72, 12 when it is unset or not a number."""
    try:
        n = int(os.environ.get(ENV, ""))
    except ValueError:
        return CLIENT_LIMIT
    return max(CLIENT_LIMIT, min(n, FREE_ROAM_MAX))


def for_type(lobby_type):
    """By Nerd::NetworkLobbyType (the game server's worlds are keyed by it)."""
    return free_roam() if lobby_type == FREE_ROAM_TYPE else CLIENT_LIMIT


def for_kind(kind):
    """By "freeroam" or "roadtrip" (launcher_lobbies.HOPPER_KINDS)."""
    return free_roam() if kind == "freeroam" else CLIENT_LIMIT


def for_hopper(hopper):
    """By matchmaking hopper name (FreeRoam_qos, RoadTrip_qos, CarMeet_qos)."""
    return free_roam() if str(hopper or "").lower().startswith("freeroam") else CLIENT_LIMIT
