# Known issues

What we know is broken or wrong in FH2MP, with workarounds where there are any. Last updated 2026-10-20.
"Fix ready" means the fix is written and being tested on our test server; it reaches you with the next server update or launcher release.
Found something not listed here? Please [open an issue](https://github.com/Charelene/FH2MP/issues/new/choose) and attach your log files (the form tells you which).

## Online play

**The game closes or you get dropped when someone joins Free Roam.** (investigating)
The game window closes with an Azimuth error such as "CreatePlacementTexture2D requires GPU-write/CPU-map..." or "The game crashed ... exception 0xc0000005 at game+0x17aa83b". It happens at the moment another player enters your lobby, and sometimes several players are dropped at once. It is not your graphics card or system: players on Windows/NVIDIA and Linux/AMD were dropped in the same second. We do not know the cause yet. Workaround: start the game again; you rejoin a lobby normally. A change that keeps the game running when Azimuth would stop for this texture error is being tested; it ships with the next launcher release.

**King and Infection get stuck on "setting up event".** (fix ready)
If the player who started the event loses connection while it is being set up, the other players waited forever. The server now drops that player from the event and carries on with the rest. Until the update: leave the event and start again.

## Boards and rivals

**Speed trap, speed zone and other Bonus Board results did not update when you improved.** (fix ready)
Every trap and zone was treated as one board, so a better run on one trap changed nothing unless it beat your best on any trap. Each trap and zone now has its own board. Your existing results are kept.

**The rival you are compared against sometimes looked far too slow and never changed.** (probably the same cause, fix ready)

**Rival times could beat the world record.** (fix ready for Festival Routes)
The computer rivals on Festival Route boards now stay slower than the community world records. Speed trap and zone records are not applied yet: we cannot tell which board is which trap.

**Monthly Rivals: the car selector allowed cars outside the class shown on the event.** (fix ready)
For themed events such as "Made in Italy" the card showed a class but only the theme was enforced. New events enforce both. Events that are live now keep their old rules until the next rotation.

## Launcher and account

**Your in-game name shows "Player 1" instead of your FH2MP name.** (investigating, [#3](https://github.com/Charelene/FH2MP/issues/3))
Your name is correct on the server and in the launcher; the game's own name display does not pick it up. We are checking what other players see in the game.

**The Online Road Trip tile on the Home page was greyed out.** (fix ready for the next release)
Online Road Trip from the Social tab always worked.

## Linux and Wine

Some Linux players report graphics hangs and an Azimuth "swap chain presents flip buffer" error when the game closes. These come from Azimuth's translation layer, not from FH2MP.

## DLC content

A few Rivals tracks from the Storm Island and Porsche content have no world-record data yet.
