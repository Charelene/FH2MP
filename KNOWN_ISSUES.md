# Known issues

What we know is broken or wrong in FH2MP, with workarounds where there are any. Last updated 2026-10-25.
"Fix ready" means the fix is written and being tested on our test server; it reaches you with the next server update or launcher release.
Found something not listed here? Please [open an issue](https://github.com/Charelene/FH2MP/issues/new/choose) and attach your log files (the form tells you which).

## Online play

**Joining car crashes the lobby at `game+0x105d747` (#52).** (fix prepared, not deployed)
At 2026-10-25 20:10:15 Toronto time, car 2280 joined with an unsupported engine swap. The server replaced the engine with stock
(engine 2410), but retained camshaft 2592000 from engine 2592. Charelene's received-object ring contains this exact combination.
The native engine-part loader rejects the mismatch and returns NULL; the RedlineRPM reader then dereferences it.
Local validation now checks engine sub-part ownership after selecting the final engine and replaces incompatible rows with its stock parts.
The captured-list regression fails on the old validator and passes with the fix. Valid supported swaps remain unchanged.


**Race class synchronization.** (initial fix deployed in v1.0.2; host handover follow-up local)
In South Scramble lobby `56614b63`, the host's earlier 716-bit settings explicitly select B. The later 795-bit write omits that
unchanged field. The server incorrectly replaced the whole settings snapshot and lost the restriction. Local code now merges partial
updates, preserves pending selections during ownership grants, and checks class/PI at invitation acceptance, participant selection,
loading and GO. The active event's rule cannot be changed during loading. The exact captured sequence reproduces the old failure and
passes with the fix. Post-release logs on October 26 also show former hosts writing settings without requesting ownership again:
their settings were discarded while their new race requests were accepted using another host's snapshot. A local follow-up retains
those edits and selects them on the accepted race request before invitations. A subsequent test audit also caught ignored mixed sparse
car selections and an invalid three-second expiry on pending settings; both are corrected locally. Independent packet-flow tests now
check invite contents, admission, loading, grid/GO and host changes. Deployment and native verification remain pending.
See `release/TEST_AUDIT.md` and `docs/race-class-followup-2026-10-26.md`.

**Festival Drag Race / South Scramble: stationary opponents despite arriving results.** (relay fix prepared; in-game verification pending)
Local code now forwards live prediction packets even when their timestamp is behind the server's cached high-water mark. Native receiving
car instances have their own clock baseline, reset during scene transitions; the relay must not discard updates on their behalf.
Ambiguously ordered input cannot replace the rephasing cache, and the server stops synthesizing updates from that cache until ordered
live input resumes. Tests cover delivery, timestamp wrap, unchanged physics, cross-event phasing, and suppression of stale cached replay.
Race scores are also isolated to their event roster. This fixes the identified relay behavior; a live drag race has not yet confirmed
whether it accounts for every reported freeze.

**A rental still shows R 999 to its own driver.** (local fix, not deployed)
The existing rental correction changed what other players saw, but did not send the corrected class/PI back to the driver's own game. Local code now sends an authority update to the owner too; in-game verification pending.

**Car physics crashes at `game+0x12e26dd`.** (local server fix for an identified input; live verification pending)
Reports #36, #37, #39, #41 and #42 contain this tire-table failure. At 22:12:43 UTC the server accepted tire id `2391004` because it existed
in another part table. The native tire loader returns without initializing its output when the tire query has no row; the later physics
builder then uses those uninitialized curve dimensions. Local validation replaces missing or wrong-table tire ids with the car's stock tire.
This prevents that input from reaching other players; it does not repair the sender's local mod/save. The available uploaded dumps do
not retain the original physics exception context, so other inputs causing the same fault have not been ruled out.

**Additional reports #46–47.** (investigating; server fixes held for maintenance)
#46's platform log records a fresh `game+0x12e26dd` tire-physics fault about 1.35 seconds before its dump's null-function call;
the null call is therefore a likely downstream failure, not established as a separate cause. #47's dump confirms a null graphics
resource dereference at `azimuth+0x1d237d` (`resource+0x160`). Its attached tire report is uncorrelated: the platform log belongs to a
later session than the dump. The local uploader now checks both ends of the dump's session before attaching a mod crash report.

**The game closes or you get dropped when someone joins Free Roam.** (investigating)
The game window closes with an Azimuth error such as "CreatePlacementTexture2D requires GPU-write/CPU-map..." or "The game crashed ... exception 0xc0000005 at game+0x17aa83b". It happens at the moment another player enters your lobby, and sometimes several players are dropped at once. It is not your graphics card or system: players on Windows/NVIDIA and Linux/AMD were dropped in the same second. We do not know the cause yet. Workaround: start the game again; you rejoin a lobby normally. A change that keeps the game running when Azimuth would stop for this texture error is being tested; it ships with the next launcher release.

**King and Infection get stuck on "setting up event".** (fix ready)
If the player who started the event loses connection while it is being set up, the other players waited forever. The server now drops that player from the event and carries on with the rest. Until the update: leave the event and start again.

## Boards and rivals

**Speed traps and zones announce a new PB on slower passes.** (local shim fix; in-game verification pending)
Reported by JonKaasKanon and reproduced by the maintainer. The notification compares against the online result,
while score uploads require beating the PB in the save. A missing online result reads as zero, so repeated slower
passes can announce a PB without uploading anything. The local patch retains the saved PB before each pass updates
it and uses the higher of that value and the online score for the notification comparison. No save reset is needed.
Offline hook checks and a release build pass; this has not been installed, released or verified in-game.

**Speed trap, speed zone and other Bonus Board results did not update when you improved.** (fix ready)
Every trap and zone was treated as one board, so a better run on one trap changed nothing unless it beat your best on any trap. Each trap and zone now has its own board. Your existing results are kept.

**The rival you are compared against sometimes looked far too slow and never changed.** (probably the same cause, fix ready)

**Rival times could beat the world record.** (fix ready for Festival Routes)
The computer rivals on Festival Route boards now stay slower than the community world records. Speed trap and zone records are not applied yet: we cannot tell which board is which trap.

**Monthly Rivals: the car selector allowed cars outside the class shown on the event.** (fix ready)
For themed events such as "Made in Italy" the card showed a class but only the theme was enforced. New events enforce both. Events that are live now keep their old rules until the next rotation.

## Launcher and account

**Crash reports silently omitted normal-sized minidumps and could miss a crash line after a restart.** (server fixes deployed; launcher fix awaits release)
Report `89b10352722bb565` / GitHub #30 omitted a 4,897,508-byte Azimuth dump because both uploader and server capped dumps at 1,000,000 bytes. The original remains on the reporter's PC. The production server now accepts up to 32 MiB of combined dumps with a 96 MiB JSON request limit (nginx 100 MiB), retains the first failure in the latest failing session before secondary crash-handler exceptions, and recognizes `azimuth+...` stop addresses. Every stored attachment is linked through the website staff login; 23 existing issues and one duplicate-report comment were updated. The launcher changes collect additional mod logs and the matching platform dump, share an 8 MiB text budget across files, and exclude Mod Platform crash text from other sessions; they still need release. Already omitted dumps require a new upload from the original PC.

**Startup crashes in FH2MP or the Mod Platform.** (FH2MP fix prepared; Mod Platform failures unresolved)
#155 confirms a separate FH2MP scanner failure during play: the shipped 1.0.2 DLL reads a candidate MZ header directly after
`IsBadReadPtr`. Its dump matches `FH2MP.dll+0x2893`; this scan repeats after startup. A local correction copies XSAPI headers through
the safe memory-read helper. Windows/Wine checks cover both DOS and NT mappings becoming inaccessible after the page query, and
the release build passes. It has not been installed or released. See [the newer crash investigation](docs/crash-followup-2026-10-26.md).
#76 confirms that the Mod Platform failure persists with launcher/shim 1.0.2: its retained 592,527-byte dump faults at
`d3dcompiler_47.dll+0x18fa`, directly reading an MZ header from candidate memory during startup. The platform also reports a missing
game Mount folder; this separate setup failure prevents installing the online car database. See [the #76 triage](docs/crash-76.md).
#32 faults while FH2MP reads a cached game header that Azimuth has unmapped. The shim now copies and validates headers through Windows'
safe memory-read API. Dumps for #33, #35 and #45 pinpoint the platform's equivalent unsafe MZ-header comparison; #34 has the same platform
offset as #33. The launcher downloads the original upstream DLL; this release does not patch the Mod Platform.
#30 is a separate early initialization failure from a run with FH2MP 1.0.0 and platform 2.7.1; its omitted dump remains needed.

**Missing optional database rows or Cinematics media can crash the game.** (local shim fixes, not released)
#31 dereferences a missing optional TorqueScale rowset. The shim supplies an empty rowset for missing inputs, preserving valid upgrade
modifiers and reference releases. #43 dereferences the missing `game:/media/Cinematics/` directory during startup; its call now follows
the existing false/skip branch when the directory query returns nothing. Neither change has been verified in a running game yet.

**Azimuth descriptor exhaustion and XMA2 audio stops.** (investigating)
#29, #38 and #44 exhaust Azimuth's fixed pool of 60 recorded-command-list descriptor blocks, reported at `azimuth+0x136b9b`.
Ownership/release of those blocks still needs runtime verification. #40 stops at `azimuth+0xbd699`
because XMA2 input is rejected; its subsequent Wine exception is secondary. These have separate causes from the tire and startup faults.

**Installing the shim after updating to 1.0.1 can fail on `d3dcompiler_47.dll`, even with the game closed.** (reported by Cole and AVALON, pending fix)
Windows reports that the file is being used by another process. Cole reports the game had not been started and the Azimuth folder had full access. AVALON reports the same issue and suggests manually removing `d3dcompiler_47.dll`, relaunching the launcher, and installing again; this workaround has not been independently verified. Investigate platform DLL replacement and file-lock handling. The error should distinguish a file lock from folder permissions and give specific recovery steps, rather than suggesting moving Azimuth without explaining whether it means the whole folder or just the executable.

**Updating to 1.0.1 required three launcher restarts.** (reported, investigating)
The reported sequence was: update the shim to 1.0.1, update the launcher to 1.0.1, then update the shim again, restarting the launcher three times in total. The second shim update was unexpected. The cause is not confirmed; launcher and shim updates should complete together without repeating an already completed update.

**Your in-game name shows "Player 1" instead of your FH2MP name.** (investigating, [#3](https://github.com/Charelene/FH2MP/issues/3))
Your name is correct on the server and in the launcher; the game's own name display does not pick it up. We are checking what other players see in the game.

**The Online Road Trip tile on the Home page was greyed out.** (fix ready for the next release)
Online Road Trip from the Social tab always worked.

## Linux and Wine

Some Linux players report graphics hangs and an Azimuth "swap chain presents flip buffer" error when the game closes. These come from Azimuth's translation layer, not from FH2MP.

## DLC content

A few Rivals tracks from the Storm Island and Porsche content have no world-record data yet.
