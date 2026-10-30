/*
 * fh2mod.h -- FH2 Mod Platform API (MIT licensed, see LICENSE).
 *
 * The FH2 Mod Platform is the d3dcompiler_47.dll (and, the same file, the
 * DWrite.dll) next to azimuth.exe. It loads every DLL in the fh2-mods folder
 * and offers them the services in FH2ModApi below, so mods don't each
 * reinvent them and don't step on each other's files.
 *
 * A mod is a 64-bit Windows DLL in fh2-mods that exports:
 *
 *     FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api);
 *
 * The platform calls it once, on its own thread (never under the Windows
 * loader lock), right after loading the DLL. Do your setup there, start
 * your own threads for anything slow, and return 0. The api pointer stays
 * valid for the life of the process.
 *
 * Strings passed in are copied; nothing you get back needs freeing. All
 * functions are thread-safe.
 *
 * Compatibility: api->api_version only ever grows, and new members are only
 * ever added at the end. Check api->size (or api_version) before using a
 * member newer than the version you built against.
 */
#ifndef FH2MOD_H
#define FH2MOD_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH2MOD_API_VERSION 9

#ifdef __cplusplus
#define FH2MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define FH2MOD_EXPORT __declspec(dllexport)
#endif

/* Opaque handle for one registered mod. */
typedef struct FH2Mod FH2Mod;
/* Opaque handle for one hook (API 3). */
typedef struct FH2Hook FH2Hook;
/* One row on the Mods page (API 4), defined below FH2ModApi. */
typedef struct FH2Setting FH2Setting;
/* A page of your own, and one of its rows (API 8), defined below FH2ModApi. */
typedef struct FH2Page FH2Page;
typedef struct FH2PageRow FH2PageRow;
/* A title-screen entry (API 9), defined below FH2ModApi. */
typedef struct FH2TitleEntry FH2TitleEntry;
typedef struct FH2Hotkey FH2Hotkey;

/* The CPU's registers at an instruction hook (API 7): see hook_instruction.
   rsp is what it was at the hooked instruction; changing it has no effect. */
typedef struct FH2Xmm {
    union {
        float f[4];
        double d[2];
        uint32_t u32[4];
        uint64_t u64[2];
    };
} FH2Xmm;
typedef struct FH2Context {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint64_t reserved;
    FH2Xmm xmm[16];
} FH2Context;
typedef void (*FH2InstructionFn)(FH2Context* ctx, void* user);

/* A command (API 9, add_command): runs on the platform's command thread,
   not the game's, with the rest of the request's line in `args`. Write
   the answer into `reply` (UTF-8, at most reply_size bytes with its NUL;
   the platform cuts anything longer). Start it with "error: " for a
   failure, which fh2mod.exe exits 1 on. */
typedef void (*FH2CommandFn)(void* user, const char* name, const char* args, char* reply, size_t reply_size);

typedef struct FH2ModInfo {
    uint32_t size;                /* sizeof(FH2ModInfo) */
    const char* name;             /* display name, UTF-8: "WheresMyBuddy" */
    const char* version;          /* "1.3.0" */
    const char* github_repo;      /* "Owner/Repo" to check for new releases, or NULL */
    const char* platform_version; /* the platform version this mod ships with, e.g. FH2MOD_PLATFORM_VERSION */
    int check_updates;            /* 0 = never go online for this mod */
    /* API 6 (platform 2.5.0): what your mod adds, in a sentence or two, for the MODS tab's
       Installed mods page. May be NULL. Leave it out (an older FH2ModInfo) on older platforms. */
    const char* description;
    /* API 9 (platform 2.8.0): nonzero for a mod that shouldn't run in online play, because it
       changes driving or what other players see (a nitro boost, say). When the game is started
       from the FH2MP launcher, register_mod then returns NULL, so return from fh2mod_init
       straight away (call register_mod first, before you do anything else); from the next
       launch the platform doesn't load the DLL at all online. Players can still choose for
       themselves on MODS > Installed mods ("Load when playing online"). Older platforms
       ignore it. */
    int offline_only;
} FH2ModInfo;

typedef struct FH2ModApi {
    uint32_t size;                /* sizeof(FH2ModApi) as the platform knows it */
    uint32_t api_version;         /* FH2MOD_API_VERSION of the platform */
    const char* platform_version; /* "2.2.0" */
    const wchar_t* azimuth_dir;   /* folder with azimuth.exe, no trailing slash */
    const wchar_t* game_dir;      /* the game's Mount folder (game:\), or "" if not found */

    /* Call once from fh2mod_init. Returns your handle for the calls below.
       The platform then shows a title-screen notice when a newer release of
       your mod is out on GitHub (checked at most every 6 hours, and again as
       soon as a different version of your mod is installed), or when
       the installed platform is older than info->platform_version. */
    FH2Mod* (*register_mod)(const FH2ModInfo* info);

    /* One line to fh2-mods\platform.log, prefixed with your mod's name. */
    void (*log)(FH2Mod* mod, const char* utf8_line);

    /* ---- String tables -----------------------------------------------------
       The game's text lives in string tables (media\Stripped\StringTables\
       <language>.zip\<Table>.str). Edits are applied to every language and
       written to Forza Horizon 2 Extra Options' mediaoverride folder, merged
       with every other mod's edits to the same table. The game reads its
       string tables once at startup: edits made in fh2mod_init are always in
       time; later ones show from the next launch. See docs/string-tables.md
       for how to find keys. */

    /* Set the string with `key` in `table` ("Main.str") to `text`. */
    void (*set_string)(FH2Mod* mod, const char* table, uint16_t key, const wchar_t* text);
    /* Replace every string in `table` that is exactly `from` with `to`. */
    void (*replace_string)(FH2Mod* mod, const char* table, const wchar_t* from, const wchar_t* to);
    /* Forget this mod's edits to `table`, or to every table if NULL. */
    void (*clear_strings)(FH2Mod* mod, const char* table);
    /* Write the changed tables now. Done for you after fh2mod_init returns;
       call it yourself after edits you make later. */
    void (*apply_strings)(FH2Mod* mod);

    /* ---- Title screen ------------------------------------------------------
       Lines shown under "PRESS (A) TO START", with every other mod's. Keep
       each under ~40 characters. "\n" separates lines; NULL or "" clears.
       Written when apply_strings runs (or right away after init). */
    void (*set_notice)(FH2Mod* mod, const char* utf8_lines);

    /* ---- The game ----------------------------------------------------------
       Azimuth maps forza_durango_release_final.exe (the 24 Nov 2015 Xbox One
       build) into its own process when you press Play. */

    /* Base address of the mapped game, or NULL if it isn't mapped yet. */
    void* (*game_base)(void);
    /* Waits until the game is mapped (or timeout_ms passes; 0xFFFFFFFF =
       forever) and returns its base, or NULL. */
    void* (*wait_for_game)(uint32_t timeout_ms);

    /* ==== API version 2 (platform 2.1.0): check api_version >= 2 ======== */

    /* ---- Game files --------------------------------------------------------
       Replace a file the game reads from game:\media, through Extra Options'
       mediaoverride folder. `media_path` is relative to media, with a zip's
       name standing in for the zip: "drivatarglobalcache/DefaultCache.bin",
       or "UI/somefile.xml" for a file inside media\UI.zip. One mod per
       file: returns 1 if written (or already identical), 0 if another mod
       already replaces it or the write failed (the log says which). String
       tables and the game database have their own functions above and
       below, and can't be replaced whole. */
    int (*set_file)(FH2Mod* mod, const char* media_path, const void* data, size_t size);
    /* Stop replacing a file (or all of yours, if NULL): the override is
       deleted and the game reads its own copy again. Files a mod no longer
       replaces at all (because it was removed) are cleaned up shortly after
       startup. */
    void (*clear_file)(FH2Mod* mod, const char* media_path);

    /* ---- Game database -----------------------------------------------------
       media\Stripped\gamedbRC.slt is a SQLite database with most of the
       game's data (cars, events, AI drivers). add_sql queues statements
       (UTF-8, several per call is fine) to run against a copy of it; every
       mod's statements run in load order, each mod's in a transaction of its
       own, and the copy goes to mediaoverride. If a mod's SQL fails, only
       that mod's changes are rolled back, and the error is logged. The copy
       is rebuilt only when something changed. Statements added in
       fh2mod_init are applied for you; later ones need apply_database. */
    void (*add_sql)(FH2Mod* mod, const char* utf8_sql);
    /* Drop all of your statements (call apply_database after). */
    void (*clear_sql)(FH2Mod* mod);
    /* Rebuild the database copy now if anything changed. Slow (seconds):
       call it from your own thread. The game reads the database once, at
       startup. */
    void (*apply_database)(FH2Mod* mod);

    /* ---- Other mods --------------------------------------------------------
       Mods can offer each other functions. Publish a struct of function
       pointers under a name, like "WheresMyBuddy.Friends", and treat it like
       this API: never change a published member, only add new ones at the
       end and bump the version. */

    /* Offer `iface` (which must stay valid) as `name`, version `version`.
       Another mod can't take a name you published. */
    void (*publish_interface)(FH2Mod* mod, const char* name, uint32_t version, const void* iface);
    /* The interface published as `name`, if its version is at least
       `min_version`; otherwise NULL. Mods start in alphabetical order, so
       look up interfaces after wait_for_mods. */
    const void* (*get_interface)(const char* name, uint32_t min_version);
    /* Waits until every mod's fh2mod_init has returned (or timeout_ms
       passes). Returns 1 when they have. Call it from your own thread:
       from inside fh2mod_init it returns 0 straight away. */
    int (*wait_for_mods)(uint32_t timeout_ms);
    /* The version string of the registered mod called `name`, or NULL if
       it isn't installed. */
    const char* (*mod_version)(const char* name);

    /* ==== API version 3 (platform 2.2.0): check api_version >= 3 ======== */

    /* ---- Hooks -------------------------------------------------------------
       Change what game code does, through the platform, so every mod's
       changes to the same code fit together: several mods can hook one
       function, each calls on to the next through `original`, and they can
       be switched on and off in any order. Hooks on exactly the same function,
       call site or pointer share it; the platform refuses a hook whose bytes
       overlap any other hook's, or game code that Extra Options (or a mod
       patching the game by itself) already changed, and logs who hooked
       what. Crash reports list every hook in place.

       Hooks in game code have to wait for wait_for_code (below). Hooks
       elsewhere (Azimuth, a Windows DLL) can be made at any time.

       Each call makes a hook, switched off, and returns it (or NULL, with
       the reason in the log). `name` is for the log and crash reports, like
       "RadioStreamFmod::createSound"; it may be NULL. `detour` is your
       function, with the same signature and calling convention as the code
       it replaces. `*original` is set before the call returns: call it from
       your detour to run the code you replaced (and any other mod's detour
       behind yours). It's safe to call at any time; while your hook is off
       it goes straight to the game's own code.

       When several mods hook the same place, the most recently enabled
       detour runs first. */

    /* An inline hook at the start of a function (`target`), with MinHook. */
    FH2Hook* (*hook_function)(FH2Mod* mod, const char* name, void* target, void* detour, void** original);
    /* One CALL rel32 or JMP rel32 instruction at `call_site`: only that
       call goes to your detour; `*original` is the function it called. */
    FH2Hook* (*hook_call)(FH2Mod* mod, const char* name, void* call_site, void* detour, void** original);
    /* A function pointer in memory, like a vtable slot: `*slot` is replaced,
       and `*original` is what it held. */
    FH2Hook* (*hook_pointer)(FH2Mod* mod, const char* name, void** slot, void* detour, void** original);
    /* Switch `hook` on or off, or all of your hooks if it's NULL (in one go:
       the game's threads are paused once, for all of them). Returns 1 if
       they all are now; 0 if any couldn't be, with the reason in the log. */
    int (*enable_hooks)(FH2Mod* mod, FH2Hook* hook);
    int (*disable_hooks)(FH2Mod* mod, FH2Hook* hook);
    /* Like wait_for_game, but also waits until Extra Options has finished
       patching the game (it checks the code it patches first, and patches
       nothing if any of it was changed). Returns the game's base when it's
       safe to hook game code, or NULL on timeout. Call it from your own
       thread. */
    void* (*wait_for_code)(uint32_t timeout_ms);

    /* ==== API version 4 (platform 2.3.0): check api_version >= 4 ======== */

    /* ---- In-game settings --------------------------------------------------
       On/Off rows on the game's own Mods page (Pause > Settings > MODS),
       next to every other mod's. The tile and the page only appear when at
       least one mod adds a row. Add your rows from fh2mod_init: the page is
       written when every mod's fh2mod_init has returned, and rows added
       after that only show from the next launch.

       Your mod keeps owning the value: pass the current one in, and when the
       player changes it, on_change tells you the new one, so you can save it
       wherever you keep your settings (your ini) and use it. The platform
       doesn't store values. Rows marked needs_restart get a note in their
       description, and changing one brings up the game's own message box
       when the player leaves the Mods page, offering to restart the game.
       Returns 1 if the row was added, 0 if not (the log says why). */
    int (*add_setting)(FH2Mod* mod, const FH2Setting* setting);
    /* The row's current value, or -1 if you have no row called `key`. */
    int (*get_setting)(FH2Mod* mod, const char* key);
    /* You changed the value yourself (a hotkey, say): the page shows it the
       next time it opens. on_change isn't called. */
    void (*set_setting)(FH2Mod* mod, const char* key, int value);

    /* ==== API version 5 (platform 2.4.0): check api_version >= 5 ======== */

    /* Sliders on the Mods page: an FH2Setting with kind = FH2_SETTING_SLIDER
       (see FH2Setting below). On an older platform a slider would come out as
       an On/Off row, so only add one when api_version >= 5. */
    /* A slider's current value, or 0 if you have no slider called `key`. */
    float (*get_slider)(FH2Mod* mod, const char* key);
    /* You changed a slider's value yourself: the page shows it the next time
       it opens. on_slide isn't called. */
    void (*set_slider)(FH2Mod* mod, const char* key, float value);

    /* ==== API version 7 (platform 2.6.0): check api_version >= 7 ======== */

    /* ---- Precise changes to game code ---------------------------------------
       For when hooking a whole function, one call or one pointer is too
       blunt: a constant in the middle of a function, a branch to flip, a
       register to read at one exact instruction. Both kinds below come back
       as an FH2Hook, made switched off, and are switched with enable_hooks
       and disable_hooks like any other hook. The same rules hold: game code
       has to wait for wait_for_code, the bytes are checked against the exe on
       disk and against every other hook, and it's all in the log and in
       crash reports. Addresses are exact: use them on the game build you
       found them in (the one Azimuth runs). */

    /* Bytes written in place: `size` bytes (at most 32) of `bytes` at `at`,
       which has to hold `expect` (the same size) first; pass NULL to take
       what's there as long as it matches the exe on disk. Disabling puts
       the original bytes back. Only one patch can cover a byte, and a patch
       can't overlap any other hook. */
    FH2Hook* (*patch_bytes)(FH2Mod* mod, const char* name, void* at, const void* expect, const void* bytes, uint32_t size);

    /* An instruction hook: when the game reaches the instruction at `at`
       (anywhere in a function, not only its start), `fn(ctx, user)` is
       called with the CPU's registers in `ctx`, before that instruction
       runs. Change them in `ctx` (not rsp) and the game carries on with your
       values. `at` has to be the start of an instruction, after the
       function's prolog, with nothing in the function jumping into the
       bytes the hook replaces (the platform checks; the log says why when it
       refuses). Several mods may hook the same instruction: the most
       recently enabled `fn` runs first. `fn` runs on the game's thread,
       often every frame: keep it short. If it crashes, that hook is
       switched off and the crash goes in the log. */
    FH2Hook* (*hook_instruction)(FH2Mod* mod, const char* name, void* at, FH2InstructionFn fn, void* user);

    /* ==== API version 8 (platform 2.7.0): check api_version >= 8 ======== */

    /* ---- Pages -------------------------------------------------------------
       A page of your own in the game's menus, laid out like the Mods page: a
       list of rows, each with a label on the left and a value on the right,
       and the selected row's description beside the list. Your mod's page
       under MODS > Installed mods gets a row that opens it. A page can also
       take over a pause-menu tile that does nothing on PC: FH2Page.tile.

       Add pages from fh2mod_init (one added later only shows from the next
       launch). Their rows can change at any time: the page is written each
       time it's opened, from the rows you last set. Returns 1 if the page was
       added, 0 if not (the log says why). */
    int (*add_page)(FH2Mod* mod, const FH2Page* page);
    /* Your page `page_key` now has these `count` rows (copied), in this
       order; the next time it opens, it shows them. Call it whenever they
       change, or from the page's on_open. Returns 1, or 0 if you have no page
       called `page_key` (or a row is unusable; the log says which). */
    int (*set_page_rows)(FH2Mod* mod, const char* page_key, const FH2PageRow* rows, uint32_t count);

    /* ---- Pages of your own design --------------------------------------
       Instead of rows, a page on a tile can be XAML you write: the game's
       own controls, styles and textures, laid out however you like, inside
       the frame of the game's options pages (background, player bar, help
       bar). `content` replaces what's between the title and the help bar
       (the HUD options page's BasicOptions panel), and must name the
       element to focus first x:Name="FirstItem". `resources` (may be NULL)
       goes into the page's ResourceDictionary. `help` (may be NULL) replaces
       the help bar's DefaultHelpPanel; NULL leaves only Back. All three are
       UTF-8 XAML fragments, using the page's xmlns (local: is ForzaUI).
       Like rows, the XAML is copied, and the page is written from the last
       you set each time it opens. A page with XAML shows it on its tile;
       under MODS > Installed mods it shows its rows if it has any, and
       otherwise its row says which tile opens it. Returns 1, or 0 if you
       have no page called `page_key`, `content` has no FirstItem, or a
       fragment's tags don't balance (the page keeps the XAML it had).

       Bindings: slot n (0..15) is an int of the page's, "FH2Mod_Slot_<n>",
       which XAML can bind two-way (a list's SelectedIndex, say), and
       "FH2Mod_Slot_<n>_Is_<v>" is true while it's v, 0..9999 (for
       Visibility, with the converters FH2Mod_ShowIf, FH2Mod_ShowIfHidden
       and FH2Mod_HideIf the platform puts in the page's resources). Slots
       are 0 when the page opens; set the ones you want otherwise in
       on_open. */
    int (*set_page_xaml)(FH2Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
    /* A slot's value (0 if there's no such page or slot). */
    int (*get_page_slot)(FH2Mod* mod, const char* page_key, int slot);
    /* Sets a slot. From on_button or on_slot, the page's bindings see it
       straight away (from on_slot, except the slot being set). From
       on_open, it's what the page opens with. Anywhere else, it's only
       stored: the page doesn't show it, and opening resets it. Returns 1,
       or 0 for no such page or slot. */
    int (*set_page_slot)(FH2Mod* mod, const char* page_key, int slot, int value);
    /* Moves the focus to the Button, Slider or SpinnerControl named `name`
       in the page's XAML, from on_button. Returns 1 if it did. */
    int (*focus_page_element)(FH2Mod* mod, const char* page_key, const char* name);
    /* A line of text on the tile your page has taken over, at its bottom
       left in the game's own style for a tile's count (a club tile's
       member count): a score, say. Plain ASCII, up to 40 characters; NULL
       or "" shows nothing. Call it from any thread, whenever it changes:
       the pause menu reads it each time it's shown. Returns 1, or 0 if you
       have no page called `page_key` or it isn't on a tile. */
    int (*set_page_tile_text)(FH2Mod* mod, const char* page_key, const char* text);
    /* That tile's colours, as 0xAARRGGBB: `background`, and `text` for its
       title and your line over it. 0 leaves the game's own (its light grey
       tile, dark title and orange line). The icon takes the background's
       colour, so it stays a shade of it. Like the text, the pause menu
       reads them each time it's shown. Returns 1, or 0 if you have no page
       called `page_key` or it isn't on a tile. */
    int (*set_page_tile_colors)(FH2Mod* mod, const char* page_key, uint32_t background, uint32_t text);

    /* ==== API version 9 (platform 2.8.0): check api_version >= 9 ======== */

    /* ---- Title-screen entries ----------------------------------------------
       A line of your own on the title screen, under "PRESS (A) TO START" and
       its notices, that runs your code when the player presses its button:
       "(Y) START ON STORM ISLAND". Each entry has a button of its own, Y, X, RB
       then LB in the order mods add them (or the one you ask for, if it's
       free): four at most, from every mod together. The lines show while the
       title screen waits for A, and go with "PRESS (A) TO START" once the
       game starts signing in. If the platform can't hook the title screen
       (the game's code isn't as expected), they don't show at all and the
       log says why. An entry can be a list instead (FH2TitleEntry.choices):
       its press shows the game's message box over the title screen with the
       choices and a Back, and the pick calls on_choose. A list of more than
       10 is shown 8 choices at a time, with Previous and Next.

       Add entries from fh2mod_init: the title screen is written when every
       mod's fh2mod_init has returned, so one added later only shows from the
       next launch. Returns 1 if the entry was added, 0 if not (a bad or
       repeated key, no label, no on_press, no button left, a list without
       on_choose or with more than FH2_TITLE_MAX_CHOICES; the log says
       why). */
    int (*add_title_entry)(FH2Mod* mod, const FH2TitleEntry* entry);
    /* The key of your entry that started the game (its on_press or
       on_choose returned FH2_TITLE_START), or NULL if the player started it with A, another
       mod's entry did, or it hasn't started yet. A later press of A on the
       title screen sets it back to NULL. Call it from any thread: from your
       hook on the world loading, say, to know whether this start is yours. */
    const char* (*title_started_by)(FH2Mod* mod);

    /* ---- Online play -------------------------------------------------------
       1 when the game was started from the FH2MP launcher (online play on the
       FH2MP servers), 0 otherwise. It's decided once, before any mod loads,
       and doesn't change during the session. Use it to turn off just the part
       of your mod that shouldn't run online; for the whole mod, set
       FH2ModInfo.offline_only instead. Call it from anywhere, fh2mod_init
       included. */
    int (*playing_online)(void);

    /* ---- Sessions ----------------------------------------------------------
       This run of the game's id, "20261021-203224-1234": when the platform
       started, and the process id (the game can start twice in one launch,
       and Restart now starts a new process). The same id is in
       platform.log's third line, in the copy of it the platform keeps
       (fh2-mods\platform-logs\platform-<id>.log, the newest 20 sessions'),
       and in fh2-mods\platform-session.json, which says for tools what's
       running (docs/api.md). Name your own logs with it, or let
       session_log_path do that. Call it from anywhere. */
    const char* (*session_id)(void);
    /* <azimuth_dir>\<folder>\logs\<session id>.log, with the folder made,
       where `folder` is your mod's own folder next to azimuth.exe (L"MyMod").
       The newest `keep` logs there are kept, counting this one (at least 1),
       and older *.log files in it are deleted. Open the file yourself; the
       path stays valid for the session. NULL if `folder` isn't a plain folder
       name or can't be made (the log says why). */
    const wchar_t* (*session_log_path)(FH2Mod* mod, const wchar_t* folder, uint32_t keep);

    /* ---- Save snapshots ----------------------------------------------------
       A copy of the saves folder (<azimuth_dir>\data\titles\1A96545B\saves)
       this session, before the game reads it: for a mod being tested, or one
       that changes what gets saved. Call it from fh2mod_init. Once every mod's
       fh2mod_init has returned, the platform copies the folder (once, however
       many mods ask) to fh2-mods\save-snapshots\<session id>\, keeps the
       newest `keep` copies (the most any mod asked for, at least 1), and says
       in the log which mods asked and why. The game reads the saves when A is
       pressed on the title screen, long after. The platform only reads the
       saves, never writes to them; to go back, copy a snapshot's contents
       over the saves folder with the game closed (all of it: Azimuth checks
       the saves' hashes). Returns 1 if a copy will be made, 0 if there's no
       saves folder or the call came after fh2mod_init (the log says why). */
    int (*request_save_snapshot)(FH2Mod* mod, const char* reason, uint32_t keep);

    /* ---- Crash dumps -------------------------------------------------------
       The minidump the platform writes with every crash report has the
       writable data (globals) of the DLLs it loaded, yours included. Memory
       behind your globals, on the heap (your tables, the state your hooks
       keep), only goes in if you add it here: `n` bytes at `p`, 1 byte to
       64 MB, in every dump from now on. Keep it to what you'd want to see
       after a crash; 64 ranges at most, from every mod together. The memory
       must stay allocated (or the range is skipped at the crash). Any
       thread. */
    void (*add_dump_memory)(FH2Mod* mod, const void* p, size_t n);

    /* ---- Hotkeys -----------------------------------------------------------
       A keyboard key (with FH2_KEY_ modifiers) and a controller chord that
       call your on_press, for a "mark this spot" key or a live switch while
       testing. Keys are only watched while the game's window is in front, and
       the game still gets every key and button: pick ones that do nothing
       harmful in game (F-keys; LB+RB with a face button). Each hotkey gets two
       rows on the Mods page, under your mod's: its key (Off, F1 to F12,
       Ctrl+F1 to F12, or yours if it's none of those) and its controller chord
       (Off, or LB+RB with Y, X, B, A or a d-pad direction). A change there
       takes effect straight away and calls on_rebind, so you can save it:
       the platform doesn't keep bindings, so pass the saved ones in next time.
       Add hotkeys from fh2mod_init, so their rows show. Returns 1 if added, 0
       if not (the log says why). See FH2Hotkey below. */
    int (*add_hotkey)(FH2Mod* mod, const FH2Hotkey* hotkey);

    /* ---- Commands ----------------------------------------------------------
       A command other programs can run while the game does, to flip a
       setting, ask for numbers or mark a spot without touching the game:
       `fh2mod MyMod shadows off` with fh2mod.exe (in the platform's zip), or
       one line of UTF-8, "<mod> <command> [args]", on the named pipe
       \\.\pipe\fh2mod-<process id> (platform-session.json has its name).
       "list" lists every mod's commands with their help. The pipe only
       starts once a mod adds a command, and only programs this Windows user
       runs on this PC can connect. `name` is letters, digits and '_', unique
       within your mod; `help` is a line for "list" (may be NULL). Commands
       run one at a time, on the platform's command thread (see
       FH2CommandFn). Returns 1 if added, 0 if not (the log says why). */
    int (*add_command)(FH2Mod* mod, const char* name, const char* help, FH2CommandFn fn, void* user);
} FH2ModApi;

/* Called on the game's UI thread when the player changes a row: keep it
   short (save the value, flip a flag), and don't call back into the game. */
typedef void (*FH2SettingChangedFn)(void* user, const char* key, int value);
/* The same for a slider (API 5). */
typedef void (*FH2SliderChangedFn)(void* user, const char* key, float value);

#define FH2_SETTING_ON_OFF 0
#define FH2_SETTING_SLIDER 1
#define FH2_SETTING_LINK   2   /* API 6 */
#define FH2_SETTING_CHOICE 3   /* API 9 */
#define FH2_SETTING_MAX_CHOICES 32

/* One row on the Mods page (API 4; sliders API 5). */
struct FH2Setting {
    uint32_t size;                 /* sizeof(FH2Setting) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Trees" */
    const char* label;             /* the row's label, UTF-8: "Breakable trees" */
    const char* description;       /* shown beside the list while the row is selected; may be NULL */
    int value;                     /* current value: 0 = Off, 1 = On */
    int needs_restart;             /* nonzero: a change only takes effect the next time the game starts */
    FH2SettingChangedFn on_change; /* may be NULL */
    void* user;                    /* passed to on_change and on_slide */

    /* ---- API 5 (platform 2.4.0): sliders. Zeroed (as by FH2Setting s{}),
       the row is On/Off and these are ignored. ---- */
    int kind;                      /* FH2_SETTING_ON_OFF or FH2_SETTING_SLIDER */
    float min, max;                /* the slider's range */
    float step;                    /* how far one press of left or right moves it; 0 = a hundredth of the range */
    float slider_value;            /* its current value (`value` is ignored) */
    float display_scale;           /* the number shown is the value times this, so 100 shows 0.12 as 12; 0 = 1 */
    int decimals;                  /* digits shown after the decimal point, 0 to 3 */
    FH2SliderChangedFn on_slide;   /* may be NULL */

    /* ---- API 6 (platform 2.5.0): links. With kind = FH2_SETTING_LINK the row
       opens a web page in the player's browser when they press A on it: your
       dashboard, your docs. It shows on your mod's own page under MODS >
       Installed mods (not on Settings > Mods), with the address under its
       description. value, needs_restart, on_change and the slider fields are
       ignored. On an older platform it would come out as an On/Off row, so
       only add one when api_version >= 6. ---- */
    const char* url;               /* "http://..." or "https://..." */

    /* ---- API 9 (platform 2.8.0): choices. With kind = FH2_SETTING_CHOICE the
       row shows one of `choices` by name, as the game's own option rows do
       ("Speedometer: Analogue / Digital"), and left and right step through
       them. `value` is the index of the one chosen (0 = the first), and
       on_change, get_setting and set_setting use that index. The names are
       copied. The slider fields are ignored. On an older platform it would
       come out as an On/Off row, so only add one when api_version >= 9. ---- */
    const char* const* choices;    /* UTF-8, shown as given: "LB", "RB", "A" ... */
    uint32_t choice_count;         /* 2 to FH2_SETTING_MAX_CHOICES */
};

/* Pages (API 8). Every page callback runs on the game's UI thread: keep them
   short, and don't call into the game from them. */
/* The page is about to be written to be shown: set its rows now if you
   haven't kept them up to date. Opening MODS > Installed mods writes every
   mod's pages, so it's called then too. */
typedef void (*FH2PageOpenFn)(void* user, const char* page_key);
/* The player pressed A on a row that has a key. */
typedef void (*FH2PageSelectFn)(void* user, const char* page_key, const char* row_key);
/* A page of your own design: a button was pressed (FH2_BUTTON_...). Return
   FH2_PAGE_IGNORED for what it usually does (B leaves the page, A does
   nothing, the others whatever the game does with them there), or one of
   the FH2_PAGE_HANDLED values. */
typedef int (*FH2PageButtonFn)(void* user, const char* page_key, int button);
/* A page of your own design: a binding changed slot `slot` to `value`. */
typedef void (*FH2PageSlotFn)(void* user, const char* page_key, int slot, int value);

#define FH2_BUTTON_A  1
#define FH2_BUTTON_B  2
#define FH2_BUTTON_X  3
#define FH2_BUTTON_Y  4
#define FH2_BUTTON_LB 5
#define FH2_BUTTON_RB 6
#define FH2_PAGE_IGNORED        0
#define FH2_PAGE_HANDLED        1   /* nothing else happens */
#define FH2_PAGE_HANDLED_SELECT 2   /* and the menus' select sound plays */
#define FH2_PAGE_HANDLED_BACK   3   /* and the menus' back sound plays */
#define FH2_PAGE_SLOTS 16

struct FH2Page {
    uint32_t size;                 /* sizeof(FH2Page) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "List" */
    const char* title;             /* the page's title, UTF-8: "Achievements" (shown in capitals) */
    const char* label;             /* the row on your mod's page that opens it; NULL = the title */
    const char* description;       /* that row's description; may be NULL */
    /* A pause-menu tile that opens this page instead of what it did on Xbox:
       "Achievements" (the tile on the pause menu and on its Storm Island
       tab). One mod per tile. NULL for none. */
    const char* tile;
    FH2PageOpenFn on_open;         /* may be NULL */
    FH2PageSelectFn on_select;     /* may be NULL: A then does nothing on the page */
    void* user;                    /* passed to every callback */
    /* Pages of your own design (set_page_xaml). Both may be NULL. */
    FH2PageButtonFn on_button;
    FH2PageSlotFn on_slot;
};

/* Title-screen entries (API 9). Called on the game's UI thread when the
   player presses the entry's button on the title screen. Keep it short (set
   a flag, start a thread of your own) and don't call into the game from it.
   Return FH2_TITLE_HANDLED to stay on the title screen, or FH2_TITLE_START
   for the platform to press A for the player straight after: the game then
   signs in, loads the save and loads the world as it always does, and
   title_started_by names this entry. */
typedef int (*FH2TitlePressFn)(void* user, const char* key);
/* The same for an entry with a list (FH2TitleEntry.choices): called when the
   player picks one, with its position in the list from 0. */
typedef int (*FH2TitleChooseFn)(void* user, const char* key, uint32_t choice);

#define FH2_TITLE_HANDLED 0
#define FH2_TITLE_START   1
#define FH2_TITLE_MAX_CHOICES 100

/* One title-screen entry (API 9). */
struct FH2TitleEntry {
    uint32_t size;                 /* sizeof(FH2TitleEntry) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Storm" */
    const char* label;             /* UTF-8, shown in capitals after its button: "Start on Storm Island"; about 24 characters fit */
    FH2TitlePressFn on_press;      /* may be NULL for an entry with choices (it isn't called then) */
    void* user;                    /* passed to on_press and on_choose */
    int button;                    /* FH2_BUTTON_Y, _X, _RB or _LB to ask for that one; 0 = the next free one */
    /* A list instead of an action: the press shows the game's message box
       over the title screen with these choices and a Back, and picking one
       calls on_choose. The choices are copied. */
    const char* const* choices;    /* UTF-8, shown as given; NULL = no list */
    uint32_t choice_count;         /* 1 to FH2_TITLE_MAX_CHOICES; over 10, shown in pages */
    FH2TitleChooseFn on_choose;
    const char* list_title;        /* the box's title (shown in capitals); NULL = the label */
    const char* list_message;      /* text over the choices; may be NULL */
};

/* Hotkeys (API 9). Modifiers for FH2Hotkey.modifiers: */
#define FH2_KEY_CTRL  1
#define FH2_KEY_SHIFT 2
#define FH2_KEY_ALT   4
/* Controller buttons for FH2Hotkey.pad_chord (XInput's bits): all of them held. */
#define FH2_PAD_DPAD_UP    0x0001
#define FH2_PAD_DPAD_DOWN  0x0002
#define FH2_PAD_DPAD_LEFT  0x0004
#define FH2_PAD_DPAD_RIGHT 0x0008
#define FH2_PAD_START      0x0010
#define FH2_PAD_BACK       0x0020
#define FH2_PAD_LTHUMB     0x0040
#define FH2_PAD_RTHUMB     0x0080
#define FH2_PAD_LB         0x0100
#define FH2_PAD_RB         0x0200
#define FH2_PAD_A          0x1000
#define FH2_PAD_B          0x2000
#define FH2_PAD_X          0x4000
#define FH2_PAD_Y          0x8000

/* Called on a platform thread (not the game's) when the key or chord is
   pressed, once per press. Keep it short, and hand game work to a hook. */
typedef void (*FH2HotkeyFn)(void* user, const char* key);
/* The player picked another binding on the Mods page: save it, and pass it
   in next time (`vk` 0 = no key, `pad_chord` 0 = no chord). On the UI
   thread. */
typedef void (*FH2HotkeyRebindFn)(void* user, const char* key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord);

/* One hotkey (API 9). */
struct FH2Hotkey {
    uint32_t size;                 /* sizeof(FH2Hotkey) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Mark" */
    const char* label;             /* UTF-8, for its rows on the Mods page: "Mark this spot" */
    uint32_t vk;                   /* the key, a Windows virtual-key code (VK_F9, 'M'); 0 = none */
    uint32_t modifiers;            /* FH2_KEY_CTRL | FH2_KEY_SHIFT | FH2_KEY_ALT, held with it */
    uint32_t pad_chord;            /* FH2_PAD_ bits, held together; 0 = none */
    FH2HotkeyFn on_press;
    FH2HotkeyRebindFn on_rebind;   /* may be NULL */
    void* user;                    /* passed to both */
};

#define FH2_ROW_NORMAL  0
#define FH2_ROW_DIM     1          /* label and value in grey: locked, not available yet */
#define FH2_ROW_HEADING 2          /* label in the game's orange: a heading over the rows after it */

/* One row of a page (API 8). */
struct FH2PageRow {
    uint32_t size;                 /* sizeof(FH2PageRow) */
    const char* key;               /* passed to on_select when A is pressed on it; NULL: A does nothing */
    const char* label;             /* left, UTF-8: "Island Explorer" */
    const char* value;             /* right: "12 / 50"; may be NULL */
    const char* description;       /* beside the list while the row is selected; may be NULL */
    const char* footer;            /* a line at the bottom of the description panel; may be NULL */
    int style;                     /* FH2_ROW_NORMAL, FH2_ROW_DIM or FH2_ROW_HEADING */
    int value_alert;               /* nonzero: the value in red */
};

/* What a mod exports. */
typedef int (*FH2ModInitFn)(const FH2ModApi* api);

#ifdef __cplusplus
}
#endif

#endif /* FH2MOD_H */
