# FH2 Eliminator — Architecture & Groundwork

> Status: **groundwork / design** (first pass). Co-developed with Charelene. This is the shared
> map for the work; the code scaffolds it references are checked in alongside it. The concrete
> file-by-file next steps are in [IMPLEMENTATION.md](IMPLEMENTATION.md).

## 1. What we're building

Port the **Eliminator** battle-royale mode (from FH4/FH5/FH6) into **FH2MP**: a drop-in, last-car-standing
mode played inside the game's own online Free Roam world.

Core loop, as the players experience it:

1. **Drop-in.** A lobby of N players is dropped into a shrinking play area of the map. A cinematic
   camera follows the drop, then hands off to the car.
2. **Car drops.** Scattered across the map are pickups holding cars of increasing class
   (D → C → B → A → S1 → S2, roughly). Driving into one swaps your car to that class.
3. **Head-to-head.** Two players challenge each other to a sprint to a destination; the loser is
   eliminated.
4. **Shrink.** The play area shrinks over time; players outside it are eliminated.
5. **Finish.** A final head-to-head decides the winner; eliminated players spectate.

We do **not** re-implement the whole mode in one pass. This document lays the groundwork: where the
code lives, the state machine, the pillars the mode needs (custom UI, camera movements,
car drops, custom audio), the network channel, and the concrete file plan with the anchors we already know.

## 2. Where it lives

The Eliminator is an **online multiplayer mode**, so it belongs in **FH2MP** (server + shim), not as a
standalone platform mod:

- **Server** (`native/`, Go): the authoritative state machine, mirroring how Road Trip is implemented
  in `internal/game/roadtrip.go`. The server already drives the game client through the game's own
  UDP protocol (lobbies, worlds, relay, events). Eliminator reuses that for the actual racing and car
  movement, and adds FH2MP-specific state on a side channel (the same mechanism as lighting/honk).
- **Shim** (`shim/`, C, `fh2-mods\FH2MP.dll`): mirrors the server state, draws the HUD, drives the
  cinematic cameras, spawns/collects car-drop pickups, and relays challenge/pickup/elimination
  messages. It is itself a mod of the **FH2 Mod Platform** and uses the platform's services (hooks,
  HUD layer, pad layer, custom pages, toasts).
- **Platform** (`fh2-mod-platform`, C++/C header): supplies the game-code map (`include/fh2game.h`)
  and the UI/HUD/pad/hook APIs the shim calls. We will add the camera/car/player sites the Eliminator
  needs to `fh2game.h` (the file explicitly invites a PR with the offset + the mod that uses it).

```
                    game's own UDP protocol (lobbies/worlds/events/races)
   game client  <===============================================>  fh2mp-server (Go)
      | FH2MP shim (fh2-mods\FH2MP.dll)                                | eliminator.go
      |  - state mirror, HUD, cameras, car drops                       | (authoritative)
      |  - FH2MP side channel: FH2MPEL1 (challenge/pickup/elim)  <====>| side.go + eliminator
      `--- platform (hooks, HUD/pad layers, pages, toasts) -----------'
```

## 3. The state machine

Server-authoritative; the shim mirrors it. One `Eliminator` instance per world, owned by that world's
goroutine (the `internal/game` rule: one world = one goroutine).

```
ELInitialising
    -> ELWaiting        (lobby fills; "start when N ready")
    -> ELDropping       (the cinematic drop; cameras run, inputs locked)
    -> ELActive         (car drops live, challenges open, area shrinking)
    -> ELHeadToHead     (a 1v1 race is running; the world keeps the others in the world)
    -> ELFinished       (winner crowned, results, teardown)
```

Per-player states (subset): `ELWaiting`, `ELDropped`, `ELActive`, `ELChallenged`,
`ELInHeadToHead`, `ELEliminated` (spectating), `ELFinished`.

Timing/limits (env-tunable like Road Trip's `FH2MP_RT_*`): `FH2MP_EL_*` for drop duration, car-drop
respawn, challenge cooldown, shrink interval/step, head-to-head timeout, etc.

## 4. The four pillars (work items)

### 4.1 Custom UI

Three layers, all provided by the platform and reused by the shim:

- **Menu page** — a "THE ELIMINATOR" tile/page to start/join, using the platform's page API
  (`add_page` / `set_page_xaml` / `set_page_rows` / `set_page_slot`). Start lives here and as a
  title-screen entry (`add_title_entry`), like "(Y) START ON STORM ISLAND".
- **In-game HUD** — the platform **HUD layer** (`fh2mod_hud.h`, `FH2HudInterface`, obtained via
  `get_interface("FH2Platform.HudLayer", …)`). `FH2HudState` already has everything Eliminator needs:
  - `corner` chip + `chips` → players remaining ("23 LEFT"), your class tier.
  - `rows` panel → your car's class, target class of the nearest drop, head-to-head opponent.
  - `countdown` ring → head-to-head 3-2-1 and the final-countdown.
  - `strip` timeline + `marks` + `set_hud_value(FH2_HUD_PLAYHEAD)` → distance to finish / area-shrink.
  - `prompts` → "Press (Y) to challenge", "Press (A) to collect".
  - `tip_before/tip_after` → contextual hint line.
- **Toasts / pop-ups** — the platform's `show_toast` (API 10) for events ("Eliminated by …",
  "Car drop upgraded: S1", "Head-to-head incoming"), and `show_message_box` for the join confirmation.

An Eliminator-specific feed (kills, "X eliminated Y", "Y won") rides `post_hud_feed` (HUD interface v2).

### 4.2 Camera movements

The camera work reuses and extends what the shim already reverse-engineered:

- **Anchor we already have:** `camanim_guard.inc` hooks `game+0x1031e40`, the `Camera::CCamAnimPlayer`
  space-provider transform (mode at `camera+0x188`, provider at `camera+8`; vtable `game+0x2e779d8`).
  `CCamAnimPlayer` is exactly the class the game uses for animated cameras — the drop cam and spectate
  cam should be instances of it, or new animation players driven the same way.
- **Drop cam:** on `ELDropping`, play/position an animation camera tracking the drop, then release.
- **Head-to-head countdown cam:** a short cinematic around the two cars before the sprint.
- **Spectate cam:** for `ELEliminated`, cycle through surviving players' cars (reuse the car-pool
  remote-car positions and the camera's follow/target provider).

We need to add these camera sites to `fh2game.h` (currently it has **no** camera entries; the camanim
guard keeps its own private constants). That is the first RE task: locate the camera manager, the
camera spawn/animation entry points, and the spectate follow provider, and add them with byte checks.

`game+0x1031e40` already has one hook, the camanim crash guard (issue #479). The Eliminator does not
add a second one: when there is a provider to write, it goes into that guard's detour (`cag_detour`).

### 4.5 Custom audio

Two routes, both already in reach, neither needing new reverse engineering:

- **The game's own UI sounds** — the platform's `play_ui_sound` (API 10): the race countdown's three
  beeps, the notification chime and seven more. Enough for the head-to-head 3-2-1 and "eliminated".
- **Our own samples** — `honk_sync.inc` already gives the game's FMOD an in-memory WAV
  (`createSound`) and plays it on the game's audio update thread (`AudioEngineFmod::vfunc_5`,
  `game+0x10140d0`). Stingers and the final-showdown music can reuse that path, played flat instead
  of at a car. The samples ship with the shim or come from the server, like the horn bank.

### 4.3 Car drops

Two sub-problems:

- **Spawn/despawn the pickup.** Follows how the server's `internal/traffic` package and the shim's
  `car_pool.inc` manage cars, and/or the platform's collidable-request API
  (`FH2_GAME_COLLIDABLE_REQUESTS_MAKE/ADD/REMOVE`, `FH2_GAME_CAR_HIDE`). A car drop is a lightweight
  world object with a class label and a position, spawned/despawned by the server's Eliminator state
  and mirrored on every client.
- **Pickup detection + swap.** When the local player enters a drop's radius, the shim sends a
  `FH2MPEL1` pickup message; the server validates, awards the class, and tells the client to swap the
  car (reusing the car-select/sync path in `carsel_sync.inc` / `car_pool.inc`).

Tier progression and drop placement live server-side (deterministic seed per world, so all clients
agree).

### 4.4 Match flow / head-to-head

- **Challenge:** player A targets B (HUD + pad layer button). The shim sends `FH2MPEL1` challenge;
  the server relays to B; B's HUD shows "Accept / Decline" (pad buttons).
- **Race:** on accept, the server starts a 2-player event using the game's existing online-event
  flow (`race_wait.inc` / `event_stock.inc` on the shim; `events.go` on the server) to a destination
  the server picks inside the current play area.
- **Elimination:** the loser is marked `ELEliminated`; their client drops to spectate cam; the HUD
  updates the remaining count. Last player standing → `ELFinished`.

## 5. The side channel (`FH2MPEL1`)

New side channel, same shape as lighting/honk (see `shim/src/lighting_wire.h`,
`shim/src/honk_wire.h`, `native/internal/game/side.go`):

- **Magic:** `FH2MPEL1`, header `{ff ff fe ff 00 00 00 00 00 00 00}` + magic.
- **Framing:** request/reply with a type byte, the player's `xuid`, `seq`, ticket `epoch`, `car`,
  HMAC-SHA256 over the payload (`hmac_sha256` in `sha256.inc`; `signed()` in `side.go`).
- **Who answers:** only a world that hosts the mode. Anywhere else the server drops the request
  without a reply, and that silence is what keeps the shim's Eliminator HUD and pad chords off (it
  then polls every 2 s instead of 4 times a second).
- **Messages (v1, as built):** one request type carrying a command, one reply type carrying the
  whole snapshot.
  - Request commands: `EL_CMD_JOIN`, `EL_CMD_LEAVE`, `EL_CMD_READY` (a plain poll),
    `EL_CMD_CHALLENGE` (target owner id, 0 = the nearest player still in, which the server works out
    from the cars' positions), `EL_CMD_ACCEPT` / `EL_CMD_DECLINE` (0 = whoever challenged me),
    `EL_CMD_DROP_CLAIM` (drop id).
  - Reply: phase, players left, shrink level, flags (area shrinking, you are out, you are in), then
    4 bytes about the asking player (class tier, who challenged them), then the drop records. Its
    length field is the reply's own length.
- **Still to add:** a head-to-head outcome / elimination message, and the final placement.

The wire definitions mirror `honk_wire.h`/`lighting_wire.h` and live in
`shim/src/eliminator_wire.h`; the server side lives next to `side.go`.

## 6. Concrete file plan

**FH2MP (this repo):**

| File | Purpose |
|---|---|
| `docs/eliminator/design.md` | this document |
| `native/internal/game/eliminator.go` | server Eliminator mode (state machine, drop placement, shrink, head-to-head, results) |
| `native/internal/game/eliminator_test.go` | unit tests for the state machine |
| `native/internal/game/side.go` (extend) | route `FH2MPEL1` to the Eliminator handler |
| `shim/src/eliminator_wire.h` | `FH2MPEL1` wire framing (C, no game/OS deps) |
| `shim/src/eliminator.inc` | client: state mirror, HUD, cameras, car-drop pickups, challenge UI |
| `shim/src/fh2mp_mod.c` (extend) | `#include "eliminator.inc"` next to the other sync features; register the HUD/pad layer + page |

**Platform (separate repo, for the shared game map):**

| File | Purpose |
|---|---|
| `include/fh2game.h` (extend) | camera manager/anim sites, car-spawn sites, player position/velocity, HUD-suspend (if not already covered) |

## 7. Known unknowns / next RE tasks

1. Camera manager + how `CCamAnimPlayer` instances are created, positioned, and released; where the
   game's own cinematic/spectate cameras live (so we can drive or reuse them). → new `fh2game.h`
   entries with byte checks.
2. The cheapest way to spawn a "car drop" pickup (traffic-car path vs. collidable preset vs. the
   car-pool allocator) and to detect "player entered radius".
3. Where the local player's car + position/velocity are read (for pickups and for the spectate cam),
   and the car-swap call path (extend `carsel_sync` / `car_pool`).
4. How the server starts a *private 2-player* event against a chosen destination (extend the Road
   Trip / event path so it isn't a full-lobby flow).
5. Confirming the HUD/pad layer preview is stable enough for a shipped mode, or whether we fall back
   to the game's own notification/XAML (`set_file`) for the critical bits.

## 8. Build & test

- **Server:** `cd native && go build ./... && go vet ./... && go test ./internal/game/...`
- **Shim:** `shim/build.sh` (needs `x86_64-w64-mingw32-gcc` + `windres`, as the root README says),
  and `shim/tests/test_eliminator.sh` for the wire.
- **Platform:** `./scripts/build.sh` (llvm-mingw + CMake) once `fh2game.h` entries are added.

## 9. Open questions for Charelene

1. Do we gate Eliminator behind an FH2MP version + server flag, or ship it always-on for a test build?
2. Car-drop pickup model: world object vs. invisible trigger + HUD-only marker? (affects `4.3`)
3. Head-to-head: reuse the game's 2-player online race (preferred) or implement a custom destination
   sprint on the side channel?
4. Should Eliminator's state machine live in `internal/game` (world-owned, like Road Trip) or in a new
   package? (recommended: `internal/game`, one goroutine rule).
