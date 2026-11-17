# FH2 Eliminator — Implementation checklist

This is the concrete, file-by-file plan for turning the Eliminator groundwork into a working mode.
It pairs with `design.md`. The scaffolding is already in place; this document lists what is done and
what the next steps are, with exact anchors.

## What is already scaffolded

| Piece | File | State |
|---|---|---|
| Wire protocol (client↔server framing, `FH2MPEL1`) | `shim/src/eliminator_wire.h` | done (draft message set) |
| Wire test, both sides on the same frozen vectors | `shim/tests/test_eliminator.{c,sh}`, `shim/tests/vectors/eliminator_*.hex`, `TestEliminatorWireVectors` | done |
| Client scaffold: custom UI (HUD/pad/page), car-drop state mirror, worker thread | `shim/src/eliminator.inc` | done (draft; no camera or pickup code yet) |
| Client wiring: `#include`, `late_init` start, `fh2mod_init` UI registration | `shim/src/fh2mp_mod.c` | done |
| Vendored platform HUD/pad headers | `third_party/fh2-mod-platform/fh2mod_hud.h`, `fh2mod_pad.h` | copied from platform 2.9.0 |
| Camera-anim game sites (verified bytes) | `fh2-mod-platform/include/fh2game.h` | a local, uncommitted edit in a platform clone; nothing in FH2MP uses it yet (FH2MP vendors `fh2mod.h` only) |
| Server scaffold: state machine + `FH2MPEL1` handler | `native/internal/game/eliminator.go` | done (drop placement, shrink, challenge bookkeeping, finish condition, a player who leaves is forgotten) |
| Server wiring: `World.El` for a Free Roam world named `elim-...`, route cache, `Hub` and worker dispatch, tick | `native/internal/game/{world,hub,workers,side,tick,peer}.go` | done (minimal) |

**Verified.** `go build ./...`, `go vet ./internal/game/` and `go test ./internal/game/` pass,
`gofmt -l` is clean, the shim builds, and `shim/tests/test_eliminator.sh` passes. The `fh2game.h`
camera bytes were checked against the real `forza_durango_release_final.exe` (TimeDateStamp
`0x565501ED`, PE32+): RVA `0x1031E40` maps to `.text` and reads exactly the
`FH2_GAME_CAMERA_ANIM_TRANSFORM_BYTES` pattern. Nothing has run in the game or against a live server.

**How it stays out of everyone else's way.** A match is a Free Roam world of its own whose name
starts with `elim-`. Only such a world has an Eliminator, and only such a world answers `FH2MPEL1`.
The shim's HUD and its LB chords follow the server's replies, so anywhere else they never come on.
The shim polls every 2 s while nothing answers and 4 times a second while something does.
`FH2MP_NO_ELIMINATOR=1` switches the shim's side off (UI registration included) and the server's
handler off.

**Trying it in game.** Send each player to a match's world with the control message
`assign <xuid> elim-1` (any name that starts with `elim-`), the way a private Free Roam is assigned;
the server log says "this Free Roam world hosts a match" when the world is made. When the players
enter Free Roam they get the HUD within about 2 s, join with LB + X or the page's row, and with two
players in the mode goes Waiting, Drop, Active. What it cannot show yet: nobody is ever eliminated,
so the mode only ends when a player leaves. A finished match stays finished: the next one is a new
world under a new name.

## The two protocols (do not mix them up)

- `proto/fh2mp.tl` is the **MTProto chat API** (friends/clubs/presence). The shim never speaks it;
  Eliminator game-mode traffic must **not** go there.
- Eliminator traffic goes on **NetV2** (the game's own wire) for racing/car movement, plus a new
  **side channel** (`FH2MPEL1`, like lighting/honk/carsel/mods) for the Eliminator-specific state.

## Next steps (in dependency order)

### 1. Matchmaking: name a match's world and send its players there

There is no lobby type for the mode, and there cannot be one. The lobby type only picks the session
the game builds (the map and its base rules), and the game registers six, one entity group file each
(`game+0x80e7d0`): 1 Road Trip, 2 Free Roam, 3 Car Meet on the mainland, and 4 / 5 / 6 the same three
on Storm Island. The type reaches the game in a 3-bit field (`SHLobbyTypeBits`), and 7 has no group
file. A mode runs inside a session, as the game's own King and Infection run inside type 2.

So a match is a Free Roam world (type 2) of its own, told apart by its name: `Hub.WorldFor` gives a
Free Roam world whose name starts with `elim-` (`ElimWorldPrefix`) the Eliminator state. Players
reach it through the existing control message `assign <xuid> <lobby>`, as for a private Free Roam
and for invites. The game and the shim see an ordinary Free Roam session.

What remains:

1. **Who names the match and assigns the players.** Today the control message is sent by hand. The
   web services should do it: a queue the shim's Join row (or a launcher button) puts a player in,
   a new `elim-<id>` name once enough players wait, one `assign` for each of them, then the players'
   games are sent into Free Roam.
2. **Capacity.** A match's world has Free Roam's capacity (`internal/capacity`). A battle royale
   wants more players than that; see what the game's session and the car pool take first.
3. **The mode's own objects.** If car drops need world objects Free Roam's NetV2 router does not
   carry, they need their own router/var table entries (CarMeet, Weather and RoadTrip each have
   theirs to copy from).

### 2. Car drops (the biggest net-new piece)

`car_pool.inc` only *widens* the game's remote-car slot cache — nothing in the repo spawns a free-standing
pickup. Two halves:

- **Server** — placement/respawn are now implemented in `Eliminator.placeDrops`/`claimDrop`
  (seeded per world, so every client agrees); the client already decodes the list (`el_response`).
- **Client spawn + pickup** — the net-new part. The exe's RTTI gives concrete leads, verified this
  round:
  - spawn the pickup as **parked traffic**: `ParkedTraffic@GameAI`, `ParkedState` (namespace
    `State@Traffic@Behavior@AIOpenWorld`), and `InstantSpawnTraffic@States` / `InstantSpawn@GameAI`;
  - sync it to every client as a **`NetworkTrafficCar`** (the game's own synced traffic-car path —
    the server's `internal/traffic` package already drives this);
  - alternatively `CarSpawnModule@EntityModel` / `VehicleSpawnData@AIOpenWorld` for an entity-module
    spawn;
  - detect pickup with **`DistanceTriggerFactory`** (a radius trigger), or poll the local player
    position from the `eliminator.inc` worker (`read_game_memory` is thread-safe) and send
    `EL_CMD_DROP_CLAIM` (already round-trips);
  - despawn with `DespawnTraffic@States` / `UnloadTrafficCars@States`.

### 3. Cinematic cameras (the other net-new piece)

`camanim_guard.inc` proved the types (`CCamAnimPlayer`, mode `camera+0x188`, space provider `camera+8`,
transform `game+0x1031e40`) but does not move the camera. `eliminator.inc` has no camera hook: that
function already carries the guard's hook (a crash fix), so the drop-in/spectate provider goes into
the guard's detour (`cag_detour`) when there is one to write.

**Verified against the exe this round:** the class is `Camera::CCamAnimPlayer` (RTTI
`.?AVCCamAnimPlayer@Camera@@`), and its vtable slot 0 (deleting destructor) is `game+0x1031130` —
both now recorded in `fh2game.h`. The exe's RTTI also exposes the camera system the Eliminator
cameras should reuse (class → intended use):

- **Animation (drop-in / cinematic):** `CCamAnimPlayer`, `CinematicCamera`, `CinematicGameCamera`,
  `CinematicCameraLocator` (namespace `Cinematics`).
- **Follow / spectate:** `CCamFollow` (+ `CCamFollowHigh`/`CCamFollowLow`), `CCamFreeTargetCar`
  (a free camera targeting a car — the natural spectate cam), `CCamDriver`, `CCamBumperHigh`,
  `CCamCockpit`, `CCamHood`, `CCamRearView`, `CCamWheel`, `CCamSide`, `CCamFree`, `CCamFreeTrack`.
- **Camera state machine (how the game switches):** `SetForcedCamera`, `SwitchToChosenCamera`,
  `ResetCameraToPlayer`, `CameraState` (namespace `SaveStates`), plus `ForzaMultiCam`/`CMultiCam`/
  `MultiCamView` (the multi-camera switchboard) and `CPreRaceTrackCamera` (the grid intro cam).

Remaining RE (in order):

1. resolve the vtable RVAs for the follow/spectate and cinematic classes above (chase each RTTI
   Complete Object Locator → its vtable, the standard MSVC layout), and add them to `fh2game.h`;
2. locate the camera manager global (compare the HUD's `WorldManager` at `*(game+0x34C0F40)` → world
   → module list `world+0xF8`) and how it picks the active camera;
3. find how a `CCamAnimPlayer`/`CinematicCamera` instance is created, positioned and released;
4. then write the drop-in/spectate provider in `cag_detour`.

### 4. Head-to-head races

Reuse the server-driven event flow (`events.go`: `MsgRequestRace` → `ReadyRequest` → `StartingGrid` →
`StartRace`) for a *private 2-player* event to a destination the server picks inside the play area. The
client already hooks around that flow (`race_wait.inc`, `event_stock.inc`, `carsel_sync.inc`). The
Eliminator-specific part is: on accept, start that event; on loss, mark `ELEliminated` and drop to spectate.

### 5. Finalize the wire message set

The `FH2MPEL1` framing in `eliminator_wire.h` is a draft. Lock the message set with the server
(`eliminator.go` constants must stay in lockstep), add per-message validation, and add the client
reports (type 3, like `carsel_wire.h`) for diagnostics. Any change to the bytes means regenerating
`shim/tests/vectors/eliminator_request.hex` and `eliminator_reply.hex`: `TestEliminatorWireVectors`
prints what the server builds when a file differs.

The server does not check a drop claim against the car's position yet (it has the positions:
`Peer.CarPos`). Add that once drops sit at real places on the map.

### 6. Custom audio

See design.md 4.5: `play_ui_sound` for the game's own beeps and chime, `honk_sync.inc`'s FMOD path
for our own samples. Nothing is built yet.

## Build & test

- **Server:** `cd native && go build ./... && go vet ./... && go test ./internal/game/...`
  (needs Go 1.27+, per the root README; the game package tests replay recorded fixtures).
- **Shim:** `shim/build.sh` (`x86_64-w64-mingw32-gcc` + `windres`; the client `.inc` compiles into the
  single `fh2mp_mod.c` translation unit).
- **Platform:** `./scripts/build.sh` (llvm-mingw + CMake) once `fh2game.h` entries are relied on.

## Open questions for Charelene (unchanged from design.md)

1. Gate Eliminator behind a version + server flag, or ship always-on for a test build?
2. Car-drop pickup model: world object vs. invisible trigger + HUD-only marker?
3. Head-to-head: reuse the game's 2-player race (preferred) or a custom destination sprint?
4. `internal/game` (world-owned, like Road Trip) — recommended, and already the shape of the scaffold.
