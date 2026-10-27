# FH2 Mod Platform (third-party)

[FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) by **Trinculo54**, MIT License (see `LICENSE` in this folder).

The platform is the `d3dcompiler_47.dll` next to `azimuth.exe` that loads every mod in `fh2-mods\` and gives them shared services: hooks
that are checked against each other and against FH2 Extra Options, string tables, file overrides, the game database, title-screen notices
and crash reports. FH2MP is one of those mods (`fh2-mods\FH2MP.dll`), so it runs alongside the other FH2 mods instead of competing with
them for `winmm.dll`.

What is here:

| File | What |
|---|---|
| `fh2mod.h` | The platform's API header, verified identical in official release v2.7.3 to the vendored v2.7.1 header (commit `6e851d89df50d11d5cb7f8f19293ff2d3a0e3fcb`). The shim compiles against it. |
| `LICENSE` | The platform's MIT licence. It applies to `fh2mod.h` (and to the platform DLL, which FH2MP does not ship). |
| `VERSION` | The platform release FH2MP is built and tested against. |

The DLL itself is not in this repository or in FH2MP's releases: the launcher downloads the author's release zip
(`launcher/fh2mp_launcher/upstream.py`, pinned in `constants.UPSTREAM` by the zip's and the DLL's SHA-256) when the Azimuth folder has no
platform or an older one, and keeps the release's `LICENSE.txt` (as `LICENSE-FH2ModPlatform.txt`) and `LICENSE-MinHook.txt` next to the DLL.
The platform includes [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause, Tsuda Kageyu) and [SQLite](https://sqlite.org)
(public domain).

To move to a newer platform: copy its `include/fh2mod.h` and `LICENSE` here, update `VERSION`, and change `UPSTREAM["platform"]` in
`launcher/fh2mp_launcher/constants.py`.
