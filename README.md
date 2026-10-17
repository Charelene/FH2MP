# FH2MP: Forza Horizon 2 online, back from the dead (sort of)

> **Work in progress. Not ready for end users.**
> This is a research project with a working demo, not a product. Expect crashes, desyncs,
> hand-edited config files and the occasional "why is there a third car?". There is no installer,
> no support, and no promise that your game will survive contact with it.

On 2023-08-22 Microsoft switched off the online services for Forza Horizon 2, and the multiplayer
went with them. FH2MP is an attempt to bring it back: the **real game client**, talking the
**game's own network protocol**, to a **replacement server** we write ourselves.

## Demo: Online Free Roam, two real clients

![Two game clients in the same Online Free Roam session: each sees the other player's car and name tag, and the Players list shows both](assets/demo-free-roam.jpg)

Two instances of the game join one server and see each other in the open world:

- both players appear in the **Players** list with their own names, and a "joined the session" toast fires
- each player's car is relayed live (position, steering, wheels) and drawn in the other player's world
- the join flow, session objects, weather and clock are served by our server, not stubbed in the client

Online events work too: one player starts an event, the other gets the invite, both pick a car, line up on
the starting grid and race, with each player's progress shown to the other.

**Online Road Trip** has its first working pieces (initial work, see the progress table). Two players can
join a Road Trip session, vote on one of three randomly drawn destinations (each with its own car class and race
types, drawn from the game's own event table), pick a car, and drive to the event with the game's own group route
and countdown. Then they race it on a shared starting grid and see the XP championship results. Moving between the
four events of a destination, and the Road Trip details around it, are still being worked out.

**Matchmaking and Car Meets** now go through the game's own cloud flow instead of a typed-in address. The game asks
(our stand-ins for) the Xbox Live session, matchmaking and game-server services for a lobby, gets a server address back and
connects. Every client has its own identity, lobbies fill up to 12 and then a new one opens, and many Car Meet, Free Roam
and Road Trip lobbies run side by side on one server. Inside a Car Meet, one player can start **Online Free Roam** from the
Y button, the other accepts with Y, and both load into the same Free Roam world.

This runs on the game's own NetV2 stack (the UDP protocol the Xbox One build shipped with).
We did not write a new multiplayer mod on top of the game; we wrote the server the game expected
to find, and taught the client where it is.

<details>
<summary><b>More screenshots</b>: Car Meet invite, online event, Road Trip vote</summary>

Each strip is two real game clients running side by side.

**Car Meet invite.** One player starts Online Freeroam from the Car Meet; the other gets the prompt and a countdown.

![Both players on the Car Meet screen while an Online Freeroam invite counts down: the player who started it sees Leave, the other sees Join](assets/car-meet-invite.jpg)

**Joined.** Both load into the same Free Roam world, found through the game's own matchmaking.

![The same two players after the invite: both are driving in the same Online Freeroam world and see each other's car and name tag](assets/car-meet-free-roam.jpg)

**Online event.** Invite, car select, starting grid and race, with both cars on track.

![Two clients in an online sprint event, each with the other's car beside them and both listed in the Players panel](assets/online-event.jpg)

**Road Trip vote.** Three randomly drawn destinations, built from the game's own event table.

<img src="assets/road-trip-vote.jpg" alt="The Online Road Trip destination vote with three destinations, their class and event lineup, and a voting timer" width="600">

</details>

## Progress

Percentages are a rough, honest estimate of "how close is this to how the original service behaved",
not a measurement. 100% means "I would not notice anything missing in a play session".

| Feature | Progress | State |
|---------|----------|-------|
| **Online Free Roam** (join, Players list, name tags, live car relay) | `█████████░` 90% | Works with two clients. Missing: a reliability layer, anything beyond two clients is untested. |
| **Matchmaking through the game's own flow** (identity, lobbies, session hand-off) | `███████░░░` 70% | Works with two clients, each with its own identity, and many lobbies of every type at once. Full lobbies roll over to a new one. Missing: lobby balancing beyond that, and a startup race that sometimes ends in "config server error" (relaunch). |
| **Car Meets** (online lobby, cars shown, invite to Online Free Roam) | `██████░░░░` 60% | Two clients meet, see each other's cars and start Online Free Roam from the Car Meet; the other accepts and joins. Missing: Showdown races, Road Trip invites (untested), Drivatars that are the same on every client, and one stray automatic "cancel" the client sends after an invite starts. |
| **Online events** (invite, accept, car select, starting grid, race, finish) | `███████░░░` 70% | A full sprint race runs end to end with two players and ends on its own, including the 30 s "last finisher" timer. Circuit end detection, events started by the *second* player (softlocks) and the results flow are not done. |
| **Online Road Trip** (vote, car select, drive to event, race, championship results) | `████░░░░░░` 40% | First working pieces, initial work only. Works with two clients: join, a three-way destination vote with random classes and race types (no party modes yet), car select that ends early once everyone picked, the game's own group route and drive countdown, the event itself, and the XP championship screens. In progress: carrying on through all four events of a destination, then voting again (event 2 onwards has timing problems), dynamic drive times, team race types, matching the original start locations, and the overlapping cars on the pre-race and standings screens. |
| **Event scoreboard and progress** | `██████░░░░` 60% | Lap times, progress and positions are relayed. Position ordering during a race is only checked on one-lap events. |
| **Car-to-car collisions** | `█████░░░░░` 50% | Hits are relayed and look right, but pushing a car slowly still gives small periodic bumps. The server timing is clean (measured), so the rest is the client's own correction of the other car. |
| **Message Center and Community News** | `████████░░` 80% | Messages from our server show up in both. Not done: the personal inbox and a few message types. |
| **Drivatars** | `██████░░░░` 60% | The game's Drivatar cache is served and your own Drivatar is personalized and uploaded. Quality and variety of served Drivatars is limited, and each client still builds its own Car Meet crowd (the server will have to decide it). Names can come from a private list in `server/data/` (not part of the repo); otherwise they are generated. |
| **Rivals and leaderboards** | `███░░░░░░░` 30% | Speed traps and Rivals events trigger and scores are stored; the leaderboard pages themselves still come up blank. |
| **Traffic sync** | `█░░░░░░░░░` 10% | Each client simulates its own AI traffic. A shared traffic simulation is a different, much larger job. |
| **Reliable delivery** (retransmission on the server side) | `█░░░░░░░░░` 10% | Fine on loopback, untested on a real network. |
| **Bucket List cars online** | `░░░░░░░░░░` 0% | Not started. |
| **Friends, presence, join-in-progress** | `░░░░░░░░░░` 0% | Not started. |

**Overall, "find a friend through a Car Meet, play Online Free Roam and an event" is at roughly 70%.**
The rows from Traffic sync downwards are the long tail.

## Other known gaps

- Only tested on one setup (see below), on loopback with two local clients. Real networks, NAT and more than
  two players are unexplored. Windows, other Azimuth versions and other game builds are unknown territory.
- Sessions are lost when the server restarts; clients have to rejoin (after a restart they also have to matchmake again).
- Free Roam and Road Trip lobbies are only exercised from a Car Meet invite and the direct-IP launch option; a Road Trip invite and Showdown races have not been tried.

## How it works, briefly

| Piece | What it does |
|-------|--------------|
| **Azimuth** (third party, not included) | A compatibility layer that runs the Xbox One build of FH2 on a PC. It stubs every online call as "offline". |
| **`shim/`** | A small in-process `winmm.dll` proxy that Azimuth loads from its own folder (Azimuth 0.9.10 checks its `azimuth-tls.dll` helper's hash, so that file can no longer be replaced). It re-enables real sockets, gives every install its own Xbox identity, lets the game's matchmaking and online Car Meet entries run, completes the QoS, secure-address and token steps Azimuth stubs out, passes launch options to the game, and patches a few spots (for example, keeping the traffic module in single-player mode and setting the lobby type for an invite join). |
| **`server/`** | Python, standard library only. `fh2mp_server.py` speaks NetV2: handshake, acks, object sync, session/lobby/weather objects, events and the race flow, and relays one client's objects, car state, collisions and scores to the others. A hub on one UDP port keeps a separate world per lobby type and lobby, and runs the Car Meet protocol (invites, join window, hand-off to the Free Roam session). `fh2mp_lsp.py` stands in for the game's HTTP cloud services (Message Center, Drivatars, Rivals) and, with `xbl.py`, for the Xbox Live session, matchmaking and game-server services the game uses to find a lobby. |
| **`shim/fh2dbg.py`** | A live debugger client for the shim's built-in debugger: breakpoints, memory watches and crash reports on a running game, without rebuilding anything. |

The protocol was recovered from the client itself, not from captured Microsoft traffic.
No Microsoft or Xbox Live service is contacted, ever. There was nothing left to sniff.

## Requirements (what it was actually tested on)

- A **legitimately owned copy** of Forza Horizon 2 (Xbox One build **1.0.0.64**). No game files are in this repo and none ever will be.
- **Azimuth 0.9.10** (the shim's hooks are checked against byte signatures from that build and skip anything that does not match; 0.9.7 is no longer supported).
- Optionally the **FH2 Extra Options 0.2.88** mod: put its `Azimuth/winmm.dll`, ini and PNG in an `fh2extra` folder next to `azimuth.exe`; the shim chain-loads it and routes winmm calls through it (the mod initializes inside its `timeBeginPeriod` export).
- Install the shim as `winmm.dll` next to `azimuth.exe` (`shim/build.sh` builds `shim/build/winmm.dll`) and launch with `WINEDLLOVERRIDES=winmm=n,b` so Wine uses it instead of its builtin.
- Linux with Wine/Proton (CachyOS, Proton-CachyOS 11). Windows should work in principle, but is untested.
- `python3` (standard library only) for the server; `x86_64-w64-mingw32-gcc` to build the shim.


## AI disclosure

This project is **AI-assisted**. If that makes you want to close the tab: fair enough, that is your call. But people also once argued against the loom, the steam engine, the telephone, the automobile and the smartphone, and yet here we are,
reading this on a screen built in a factory, powered by a grid, delivered over a global network. A tool that helps one person get through work that used toneed a team and a month is still just a tool, and the output is still judged the old way: does the client connect, does the packet decode, does the car show up on the other screen. Bugs are ours either way.

## Legal and ethics

- Unofficial fan research for interoperability with a game you own. Not affiliated with or endorsed by
  Microsoft, Turn 10, Playground Games, or the Azimuth developer.
- No game files, Azimuth binaries or extracted assets are included or may be contributed.
- The server only talks to clients you point at it. It never contacts Microsoft or Xbox Live services.

## Related projects

- **Azimuth** by Almost There Games: the layer that makes any of this possible.
- **FH2 Extra Options** by RokkuDayo: quality-of-life patches; compatible with the shim's chain-loading.

## License

[GPL-3.0](LICENSE). The game, Azimuth and the other mods mentioned above belong to their respective authors and are not covered by it.
