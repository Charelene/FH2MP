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

#define FH2MOD_API_VERSION 10

#ifdef __cplusplus
#define FH2MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define FH2MOD_EXPORT __declspec(dllexport)
#endif

/* Opaque handle for one registered mod. */
typedef struct FH2Mod FH2Mod;
/* Opaque handle for one hook (API 3). */
typedef struct FH2Hook FH2Hook;
/* One row on your mod's page (API 4), defined below FH2ModApi. */
typedef struct FH2Setting FH2Setting;
/* A page of your own, and one of its rows (API 8), defined below FH2ModApi. */
typedef struct FH2Page FH2Page;
typedef struct FH2PageRow FH2PageRow;
/* A title-screen entry (API 9), defined below FH2ModApi. */
typedef struct FH2TitleEntry FH2TitleEntry;
typedef struct FH2Hotkey FH2Hotkey;
/* UI templates (API 10), defined below FH2ModApi. */
typedef struct FH2Prompt FH2Prompt;
typedef struct FH2Toast FH2Toast;
typedef struct FH2Tip FH2Tip;
typedef struct FH2MessageBox FH2MessageBox;

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
    /* API 6 (platform 2.5.0): what your mod adds, in a sentence or two, for your row on
       Pause > MODS > YOUR MODS and your page there. May be NULL. Leave it out (an older FH2ModInfo) on older platforms. */
    const char* description;
    /* API 9 (platform 2.8.0): nonzero for a mod that shouldn't run in online play, because it
       changes driving or what other players see (a nitro boost, say). When the game is started
       from the FH2MP launcher, register_mod then returns NULL, so return from fh2mod_init
       straight away (call register_mod first, before you do anything else); from the next
       launch the platform doesn't load the DLL at all online. Players can still choose for
       themselves on MODS > YOUR MODS ("Load when playing online"). Older platforms
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
       written to the game's override folder (mediaoverride, or from platform
       2.9.0 on Azimuth 1.0 or later without Extra Options, the Azimuth
       folder's own layer: docs/car-packs.md), merged with every other mod's
       edits to the same table. The game reads its
       string tables once at startup: edits made in fh2mod_init are always in
       time; later ones show from the next launch. See docs/string-tables.md
       for how to find keys. */

    /* Set the string with `key` in `table` ("Main.str") to `text`. */
    void (*set_string)(FH2Mod* mod, const char* table, uint16_t key, const wchar_t* text);
    /* Replace every string in `table` that is exactly `from` with `to`.
       Each language's table has its own text, so English `from` only
       matches the English tables (EN, GB, and CZ and HU, which are English
       too). For your text in every language, see set_string_tr (API 10). */
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
       Replace a file the game reads from game:\media, through the
       mediaoverride folder (from platform 2.9.0, on Azimuth 1.0 or later
       without Extra Options, the Azimuth folder's own layer, which the game
       reads first). `media_path` is relative to media, with a zip's
       name standing in for the zip: "drivatarglobalcache/DefaultCache.bin",
       or "UI/somefile.xml" for a file inside media\UI.zip. One mod per
       file: returns 1 if written (or already identical), 0 if another mod
       already replaces it or the write failed (the log says which). String
       tables and the game database have their own functions above and
       below, and can't be replaced whole. From platform 2.9.0 the
       platform's own UI files are refused too (A_Hud, A_Splash,
       A_PauseMenu, A_PauseOptions, A_ModOptions and A_ModPage_NN in
       UI\Scenes\Anthem, stateflow\pause.xml and ui_screens.xml; api.md
       lists them), and so is zipmanifest.xml while the platform writes its
       own, with car packs' zips added to the game's. */
    int (*set_file)(FH2Mod* mod, const char* media_path, const void* data, size_t size);
    /* Stop replacing a file (or all of yours, if NULL): the override is
       deleted and the game reads its own copy again. Files a mod no longer
       replaces at all (because it was removed) are cleaned up shortly after
       startup, from every folder the platform wrote them to (from platform
       2.9.0, mediaoverride and the layer; api.md says how what 2.8.0 left
       in mediaoverride is moved aside). */
    void (*clear_file)(FH2Mod* mod, const char* media_path);

    /* ---- Game database -----------------------------------------------------
       media\Stripped\gamedbRC.slt is a SQLite database with most of the
       game's data (cars, events, AI drivers). add_sql queues statements
       (UTF-8, several per call is fine) to run against a copy of it; every
       mod's statements run in load order, each mod's in a transaction of its
       own, and the copy goes to mediaoverride (from platform 2.9.0, on
       Azimuth 1.0 or later without Extra Options and without FH2MP, the
       Azimuth folder's own layer). If a mod's SQL fails, only
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
       On/Off rows in the game's own menus, on your mod's page under Pause >
       MODS > YOUR MODS (Pause > Settings > MODS opens the same list; up to
       2.8.0 it was a page shared by every mod), in the order you add them,
       so put your main switch first. Add your rows from fh2mod_init: the page is
       written when every mod's fh2mod_init has returned, and rows added
       after that only show from the next launch. (Their text can change
       later, in the game's language: see set_setting_text, API 10.)

       Your mod keeps owning the value: pass the current one in, and when the
       player changes it, on_change tells you the new one, so you can save it
       wherever you keep your settings (your ini) and use it. The platform
       doesn't store values. Rows marked needs_restart end in " *" and get a
       note in their description, and changing one brings up the game's own
       message box when the player leaves, offering to restart the game.
       Returns 1 if the row was added, 0 if not (the log says why). */
    int (*add_setting)(FH2Mod* mod, const FH2Setting* setting);
    /* The row's current value, or -1 if you have no row called `key`. */
    int (*get_setting)(FH2Mod* mod, const char* key);
    /* You changed the value yourself (a hotkey, say): the page shows it the
       next time it opens. on_change isn't called. */
    void (*set_setting)(FH2Mod* mod, const char* key, int value);

    /* ==== API version 5 (platform 2.4.0): check api_version >= 5 ======== */

    /* Sliders on your mod's page: an FH2Setting with kind = FH2_SETTING_SLIDER
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
       A page of your own in the game's menus, laid out like your mod's page: a
       list of rows, each with a label on the left and a value on the right,
       and the selected row's description beside the list. Your mod's page
       under MODS > YOUR MODS gets a row that opens it. A page can also
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
       XAML fragments, using the page's xmlns (local: is ForzaUI). They're
       written as they are, and the game reads them a byte a character: write
       a letter beyond ASCII as a hex reference with a lowercase x (&#xf3;
       for o-acute). A UTF-8 letter shows as two wrong ones, and a decimal
       reference (&#243;) as a space.
       Like rows, the XAML is copied, and the page is written from the last
       you set each time it opens. A page with XAML shows it on its tile;
       on your mod's page under MODS > YOUR MODS it shows its rows if it has
       any, and otherwise its row says which tile opens it. Returns 1, or 0 if you
       have no page called `page_key`, `content` has no FirstItem, or a
       fragment's tags don't balance (the page keeps the XAML it had).
       From platform 2.9.0 it also returns 0 when a resource key the XAML
       uses isn't one every page has or one of its own, or an image isn't
       one of the game's textures, which would crash the game as the page
       opens: the page keeps the XAML it had, or, if none has passed yet,
       shows "This page couldn't be shown" until XAML that passes is set.
       A page the game crashed opening is left out until the mod's DLL
       changes, or until the player presses A on the message its tile
       shows instead. A {StaticResource} of a key only the XAML defines
       has to come after that key (templates aside); scope and element
       types aren't checked. api.md has the rules.

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
       FH2MP servers), 0 otherwise. It's decided before any mod loads; from
       platform 2.9.0 it can turn from 0 to 1 later in the session (FH2MP's
       session found once the mods had started), never back, so check it
       when it matters rather than once. Use it to turn off just the part
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
       newest `keep` copies (the most any mod asked for, at least 1; from
       platform 2.9.0 the platform's own car-packs-<session id> copies aren't
       counted), and says
       in the log which mods asked and why. The game reads the saves when A is
       pressed on the title screen, long after. The platform only reads the
       saves, never writes to them; to go back, with the game closed, move
       the saves folder aside and copy a snapshot's contents into a new,
       empty one (not over the old: saves are numbered revisions, so newer
       ones would stay and be loaded, and Azimuth checks the saves' hashes).
       Returns 1 if a copy will be made, 0 if there's no
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
       rows on your mod's page, under "Keys and buttons": its key (Off, F1 to F12,
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

    /* ==== API version 10 (platform 2.9.0): check api_version >= 10 ======= */

    /* ---- Text in the game's language -------------------------------------
       The game picks its text language (EN, DE, CHT...) a few seconds after
       it starts, from its own option, well after fh2mod_init: rows added
       there can only be in a language you guess. game_language tells you
       the language once the game has it, and set_setting_text and
       set_mod_description change your text then. The MODS tab's pages
       (your mod's page among them) are written each time they're opened, so new
       text shows the next time the player opens them. Call them any time,
       from any thread.

       game_language: the game's text language, the code its string tables
       are named by: "EN", "GB", "DE", "FR", "ES", "MX", "IT", "BR", "NL",
       "PL", "RU", "JP", "KO", "CHT", "CZ", "HU" (Czech and Hungarian are the
       game's English text). Returns 1 with the code in `code`, NUL-
       terminated, or 0 before the game has a language, or with no game.
       Cheap enough to ask every frame.

       set_setting_text: a row's label, description and, for a choice row,
       the choices' names. NULL leaves that part as it was; `choices` must
       have as many names as the row has (`choice_count`). Link rows take a
       label and a description. Returns 1, or 0 if you have no row called
       `key` or the choices don't match (the log says why). The log notes
       the first change of each row in a session, not later ones.

       set_mod_description: your FH2ModInfo.description, shown on the YOUR
       MODS list and in "What it does" on your mod's page. */
    int (*game_language)(char* code, size_t size);
    int (*set_setting_text)(FH2Mod* mod, const char* key, const char* label, const char* description,
                            const char* const* choices, uint32_t choice_count);
    void (*set_mod_description)(FH2Mod* mod, const char* description);

    /* ---- Translations -------------------------------------------------------
       Your text in the player's language, from translations you build into
       your DLL as gettext PO files (docs/translating.md has the workflow and
       include/fh2mod_l10n.h the helpers). Each text has a key, the PO file's
       msgctxt ("row.Trees.label"), and its English, the msgid. Anything
       without a translation shows in English.

       Text you get from tr() and pass to the platform (a row's label,
       description or choices, a page's title and rows, a title-screen entry,
       a hotkey's label, set_notice, set_mod_description) follows the game's
       language by itself: the platform knows which key each text it handed
       out came from, and looks it up again in the game's language each time
       it writes a page. The game picks its language a few seconds after
       fh2mod_init, so add your rows there with tr() as usual. The title
       screen's entry lines are the exception: they're written once, as the
       game starts, so they follow its language from the next launch. Text
       you build from tr()'s (with a number filled in, say) is yours, and
       stays as it was: set it again when the page opens (FH2Page.on_open).

       language: the language text is looked up in, as a BCP-47 tag: "en",
       "en-GB", "pl", "pt-BR", "de", "fr", "es-ES", "es-MX", "it", "ja", "ko",
       "zh-Hant", "cs", "hu", "ru", "nl" (the game's language once it has one;
       before that, the one it had last time; the first time, the one the
       game picks for Windows' regional format), or "qps" for the
       test language (padded text in [brackets], turned on by test=1 in
       fh2-mods\platform-language.ini). Any thread, fh2mod_init included.

       add_translation: one language's PO file, held in memory (`size`
       bytes of UTF-8; your DLL's copy can be freed after the call). `lang` is
       its tag ("pl", "pt-BR"; "pt_BR" is read the same; "pl_PL", "ja_JP"
       and the like as the language alone, "zh_TW" as "zh-Hant"; one none
       of the game's languages looks for, such as "es_AR", adds nothing).
       Call it once per
       language, from fh2mod_init right after register_mod. If
       fh2-mods\lang\<your mod's name>\<lang>.po exists, that file is used
       instead (the log says so), so translators can try theirs without a
       build. Entries marked fuzzy are skipped, and one whose {placeholders}
       don't match the English, or whose text has a control character other
       than \n and \t, is left out with a line in the log (a plural
       form `lang` uses for 1 alone may leave the number out). A file that
       isn't UTF-8 isn't read at all. Returns how
       many entries it took (0: none; the log says why).

       tr: `english` in the language now, or `english` when there's no
       translation. `key` is the msgctxt (NULL: entries without one). A
       translation made from different English than you pass (the English
       changed since) isn't used, so it never shows with the wrong meaning.
       Looks in the language's catalogue, then pt-BR -> pt, en-GB -> en,
       es-MX -> es-ES -> es, then English. The text stays valid for the
       session: don't free it. Pass the same English each time: past 20000
       different texts from one mod, English not given before comes back
       as the pointer you passed, untranslated (the log says so), as on
       platforms before API 10.

       tr_plural: the same for text with a count: `one` and `other` are the
       English forms (msgid and msgid_plural), and the form for `n` is picked
       by the language's own rules (Polish has three). It returns the form
       with its {n} still in: fill it in yourself (fh2_fill in
       fh2mod_l10n.h), with format_number for the digits.

       set_string_tr: set_string with a key, for each language's string table
       in its own words: every table gets the text for its language, so the
       game shows the right one whatever language it picks. `msg_key` and
       `english` are looked up as in tr(). A translation whose braces don't
       fit the English's ({0}, {{token}}), which the game's own formatting
       could misread, goes in as the English (the log says so). Write it
       from fh2mod_init, as set_string.

       format_number: `v` with `decimals` digits after the point (0 to 6),
       the way the language writes numbers: 1,234.5 in English, 12 345,5 in
       Polish (a no-break space; 1234,5 with four digits, as CLDR has it),
       1.234,5 in Brazilian Portuguese and German.
       format_date: a date, 24.10.2026 in Polish and German, 24/10/2026 in
       Portuguese, French, Spanish and Italian, and 2026-10-24 in English.
       Both write UTF-8 to `out` and return its length, or 0 (with "" in
       `out`) when `n` bytes don't hold it or the value isn't usable.

       FH2ModInfo.description reaches register_mod before your translations
       do, so the platform looks it up itself under the key
       "mod.description". */
    const char* (*language)(void);
    int (*add_translation)(FH2Mod* mod, const char* lang, const char* po_utf8, size_t size);
    const char* (*tr)(FH2Mod* mod, const char* key, const char* english);
    const char* (*tr_plural)(FH2Mod* mod, const char* key, const char* one, const char* other, int64_t n);
    void (*set_string_tr)(FH2Mod* mod, const char* table, uint16_t key, const char* msg_key, const wchar_t* english);
    int (*format_number)(double v, int decimals, char* out, size_t n);
    int (*format_date)(int year, int month, int day, char* out, size_t n);

    /* ---- UI templates ------------------------------------------------------
       Ready-made pieces of the game's own UI that you fill with text: pop-ups
       on the game's own HUD notification, tips, the game's message box, and
       the help bar and hint line of your pages. The platform draws them in
       the game's look, decides when they show, and never takes XAML from you,
       so nothing you pass can break the game's screens.

       Every call is quick and can be made from any thread, fh2mod_init
       included, unless its comment says otherwise. Each copies every string
       and struct you pass before it returns, so tr() results and buffers on
       your stack are fine; only callbacks and their user pointers are kept.
       Text is UTF-8. Titles are shown in capitals made by the platform in the
       player's language, so pass sentence case. Limits are in characters,
       not bytes; longer text is cut with an ellipsis.

       Pop-ups and tips wait while the player is in the pause menu, photo
       mode, a replay, loading or a cutscene. Everything shown is logged, its
       text included (see FH2_TOAST_FLAG_PRIVATE). When your mod faults, the
       platform drops what you have waiting. Ask ui_status what works this
       session: a call that can't show returns 0, and the log says why.
       include/fh2mod_ui.h wraps every one of them so that it does nothing on
       an older platform. */

    /* What works now, as FH2_UI_ bits. Cheap. */
    uint32_t (*ui_status)(FH2Mod* mod);
    /* What you'll use, as FH2_UI_USES_ bits. From fh2mod_init only. With
       FH2_UI_USES_TOASTS the platform adds a "Pop-ups" row to your mod's
       page, unless you also pass FH2_UI_USES_OWN_SWITCH because you have a
       row of your own; FH2_UI_USES_TIPS starts the controller poll that tells
       which device the player used last. Declare pop-ups and tips here: the
       platform hooks the game's notification only in a session where a mod
       did (or with Developer=1 in fh2-mods\platform-popups.ini), and
       otherwise show_toast and show_tip return 0. Returns 1, or 0 after
       fh2mod_init. */
    int (*ui_uses)(FH2Mod* mod, uint32_t uses);
    /* Plays one of the game's UI sounds (FH2_SOUND_), wherever the player
       is. From one of your page, setting, title or box callbacks it plays at
       once; from anywhere else, at the game's next tick, or not at all if
       none comes within half a second. Each sound at most once per 100 ms
       from your mod. Never make a sound the only cue. Returns 1, or 0 for an
       unknown sound, a repeat within 100 ms, NOTIFY or a countdown beep
       while the driving view isn't on screen (they're silent under menus),
       before the game's sound is up, or when its sound code isn't as
       expected this session. */
    int (*play_ui_sound)(FH2Mod* mod, uint32_t sound);

    /* ---- Pop-ups -----------------------------------------------------------
       The game's own HUD notification: one of its nine pictures, a title and
       a line, bottom centre over the driving view, for about 5 seconds. One
       queue for every mod, spaced around the game's own notifications. One
       with the same key as one of yours that is waiting replaces it; one
       already on screen runs its 5 seconds. Up to 4 of yours wait at once;
       a 5th drops your oldest waiting one. Time spent waiting while the
       player isn't driving doesn't count toward expire_s, so a pop-up sent
       from fh2mod_init waits for the first drive. The player decides whether
       mod pop-ups show: the game's Minor notifications option, the
       platform's "Mod pop-ups" row and your mod's "Pop-ups" row
       (FH2_UI_TOASTS_OFF says they're off; turning one off also drops those
       waiting). Returns 1 if queued (it can still expire or be dropped), 0 if
       not (off, no title, a bad key; the log says which). */
    int (*show_toast)(FH2Mod* mod, const FH2Toast* toast);
    /* Takes down your waiting pop-up `key`, or all your waiting ones for
       NULL. One already on screen runs its 5 seconds. */
    void (*clear_toast)(FH2Mod* mod, const char* key);

    /* ---- Tips --------------------------------------------------------------
       A hint with a button in it, "Press {button} to keep it", shown once
       rather than every session: a pop-up with your mod's name as its title,
       after delay_s seconds of driving and at least 3 minutes after any other
       mod's tip. The button is written as the player last pressed (or as
       they chose in a HUD layer's Button prompts row): "(A)" on a pad,
       "[SPACE]" on the keyboard. The platform remembers which tips were seen
       (fh2-mods\platform-seen.txt); a tip counts as seen only once it was
       really drawn. The player can turn tips off, which also drops a waiting
       one (not seen). Returns 1 if
       queued, 0 if it was seen, tips or the game's Minor notifications are
       off, or a button is set but the text has no "{button}" (the log says
       which). */
    int (*show_tip)(FH2Mod* mod, const FH2Tip* tip);
    /* 1 if the tip with this key has been seen. */
    int (*tip_seen)(FH2Mod* mod, const char* key);
    /* Marks it seen (1), which also drops it if it's waiting: for tips that
       last until the player has done the thing (FH2_TIP_FLAG_UNTIL_DONE).
       Or not seen (0), to show it again. */
    void (*set_tip_seen)(FH2Mod* mod, const char* key, int seen);

    /* ---- Message boxes -----------------------------------------------------
       The game's own message box over the page the player is on: a title, a
       message, your choices and Back, which the platform adds last. Call it
       only from a callback of one of your pages, your settings or your title
       entries (on_open, on_select, on_button, on_slot, on_change, on_slide, a
       title entry's on_press or on_choose); from anywhere else, or from a
       box's own on_choose, it returns 0.
       One box at a time, and nothing is queued. on_choose gets the choice
       (from 0) or FH2_BOX_BACK (the Back item, or B), on the game's UI
       thread; it may never be called if the player leaves the box another
       way. Returns 1 if shown (from a tile page's on_open: as the page
       loads), 0 if not (not from a callback, on_open as YOUR MODS opens, a
       box already shown or still coming up, no title or no choices; the log
       says why).
       A title entry that shows a box can't also start the game. */
    int (*show_message_box)(FH2Mod* mod, const FH2MessageBox* box);

    /* ---- Pages: help bar and hint line ------------------------------------- */
    /* Your page's help bar: these prompts (copied), beside the platform's own
       (B) Back and, when the page has on_select, (A) Select, in the game's
       order. Only buttons a callback of the page can handle are accepted: A
       and B on a page of rows; A, B, X, Y, LB and RB on a page of your own
       design (on_button). Use FH2_WORD_ for the game's own words. Returns how
       many prompts were accepted (the log says why any weren't). */
    int (*set_page_help)(FH2Mod* mod, const char* page_key, const FH2Prompt* prompts, uint32_t count);
    /* The texts your page's hint line can show (copied; up to 8, each up to
       80 characters), written into the page each time it opens. From
       fh2mod_init or the page's on_open. Returns 1, or 0 for no such page. */
    int (*set_page_hints)(FH2Mod* mod, const char* page_key, const char* const* texts, uint32_t count);
    /* Shows hint `index` in `kind` (FH2_HINT_), or none for -1. From the
       page's on_open it shows as the page opens, and from its own callbacks
       while it's up (on_select, on_button, on_slot, or a box's on_choose
       over it) it changes on screen at once; at any other time it shows the
       next time the page opens. Returns 1, or 0 for no such page, hint or
       kind. */
    int (*show_page_hint)(FH2Mod* mod, const char* page_key, int index, uint32_t kind);
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

/* One row on your mod's page (API 4; sliders API 5). */
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
    int kind;                      /* FH2_SETTING_ON_OFF, _SLIDER, _LINK (API 6) or _CHOICE (API 9) */
    float min, max;                /* the slider's range */
    float step;                    /* how far one press of left or right moves it; 0 = a hundredth of the range */
    float slider_value;            /* its current value (`value` is ignored) */
    float display_scale;           /* the number shown is the value times this, so 100 shows 0.12 as 12; 0 = 1 */
    int decimals;                  /* digits shown after the decimal point, 0 to 3 */
    FH2SliderChangedFn on_slide;   /* may be NULL */

    /* ---- API 6 (platform 2.5.0): links. With kind = FH2_SETTING_LINK the row
       opens a web page in the player's browser when they press A on it: your
       dashboard, your docs. It shows on your mod's own page under MODS >
       YOUR MODS, with the value "Browser" and the address under its
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
   haven't kept them up to date. Opening MODS > YOUR MODS writes every
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
       "Achievements" (the tile on the pause menu's PROGRESS tab and on its
       PORSCHE tab). One mod per tile. NULL for none. */
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
/* The player picked another binding on your mod's page: save it, and pass it
   in next time (`vk` 0 = no key, `pad_chord` 0 = no chord). On the UI
   thread. */
typedef void (*FH2HotkeyRebindFn)(void* user, const char* key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord);

/* One hotkey (API 9). */
struct FH2Hotkey {
    uint32_t size;                 /* sizeof(FH2Hotkey) */
    const char* key;               /* unique within your mod: letters, digits and '_', like "Mark" */
    const char* label;             /* UTF-8, for its rows on your mod's page: "Mark this spot" */
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

/* ---- UI templates (API 10) ------------------------------------------------ */

/* ui_status bits (API 10). */
#define FH2_UI_TOASTS          0x0001  /* pop-ups and tips can show this session: a mod declared them (ui_uses)
                                          or Developer=1 is set, the game's notification code is as expected
                                          and the platform's hooks for them are in (without its watch on the
                                          game's own pop-ups, they're spaced 5.5 s apart) */
#define FH2_UI_TOASTS_OFF      0x0002  /* the player turned mod pop-ups off: the platform's row, your mod's
                                          row, or the game's Minor notifications option it follows (read
                                          only while a pop-up or tip waits on the driving view, so known to
                                          be off only once it has been read off) */
#define FH2_UI_TIPS_OFF        0x0004  /* the player turned tips off */
#define FH2_UI_ON_SCREEN       0x0008  /* the free-roam HUD page is on screen now */
#define FH2_UI_GAME_HUD_HIDDEN 0x0010  /* the game's HUD is suspended (a car meet, a Rivals event), or a mod's
                                          HUD layer holds it hidden (hold_game_hud): pop-ups and tips wait */
#define FH2_UI_KEYBOARD        0x0020  /* buttons are written as keys: the keyboard or mouse was used last, or
                                          the player chose Keyboard and mouse in a HUD layer's Button prompts row */

/* ui_uses bits (API 10). */
#define FH2_UI_USES_TOASTS     0x0001  /* pop-ups: the platform adds a "Pop-ups" row to your page */
#define FH2_UI_USES_TIPS       0x0002  /* tips: the controller poll for the device used last */
#define FH2_UI_USES_OWN_SWITCH 0x0004  /* you have a pop-ups row of your own: the platform adds none */

/* Sounds (play_ui_sound, FH2Toast.sound). 0 = none. All eleven were heard in game; NOTIFY and the three
   countdown beeps sound only over the driving view, so they're refused under menus. The HUD layer preview
   (fh2mod_hud.h) plays 5 to 11 with its own play_ui_sound. */
#define FH2_SOUND_SELECT   1
#define FH2_SOUND_BACK     2
#define FH2_SOUND_OPEN     3
#define FH2_SOUND_CLOSE    4
#define FH2_SOUND_ERROR    5   /* the menus' "can't do that" (a low thump over the driving view) */
#define FH2_SOUND_NOTIFY   6   /* the game's notification chime, for a milestone */
#define FH2_SOUND_INFO     7   /* a pop-up opening (very faint over the driving view) */
#define FH2_SOUND_TICK     8   /* the game's countdown beeps */
#define FH2_SOUND_TICK_LOW 9
#define FH2_SOUND_TICK_END 10
#define FH2_SOUND_SHUTTER  11  /* the photo shutter */

/* Icons (FH2Toast.icon): the game's own notification pictures, which always show. 10 to 18 are kept for a
   later API's. */
#define FH2_ICON_NONE      0
#define FH2_ICON_TROPHY    1
#define FH2_ICON_ROADS     2
#define FH2_ICON_PHOTO     3
#define FH2_ICON_BOARDS    4
#define FH2_ICON_BUCKET    5
#define FH2_ICON_CARMEET   6
#define FH2_ICON_CLUB      7
#define FH2_ICON_PAYOUT    8
#define FH2_ICON_BARNFIND  9

/* The game's own words, in the player's language (FH2Prompt.word). */
#define FH2_WORD_NONE     0
#define FH2_WORD_SELECT   1
#define FH2_WORD_BACK     2
#define FH2_WORD_ACCEPT   3
#define FH2_WORD_TOGGLE   4
#define FH2_WORD_ON       5
#define FH2_WORD_OFF      6
#define FH2_WORD_PREVIOUS 7
#define FH2_WORD_NEXT     8

/* Pad glyphs (FH2Prompt.glyph, and the HUD layer preview's FH2HudPrompt.glyph): the game's own. 1 to 6 are
   the same numbers as FH2_BUTTON_A to FH2_BUTTON_RB (API 8), so a page's button code is its glyph. */
#define FH2_GLYPH_NONE           0
#define FH2_GLYPH_A              1
#define FH2_GLYPH_B              2
#define FH2_GLYPH_X              3
#define FH2_GLYPH_Y              4
#define FH2_GLYPH_LB             5
#define FH2_GLYPH_RB             6
#define FH2_GLYPH_LT             7
#define FH2_GLYPH_RT             8
#define FH2_GLYPH_MENU           9
#define FH2_GLYPH_VIEW           10
#define FH2_GLYPH_DPAD_UP        11
#define FH2_GLYPH_DPAD_DOWN      12
#define FH2_GLYPH_DPAD_LEFT      13
#define FH2_GLYPH_DPAD_RIGHT     14
#define FH2_GLYPH_DPAD_UPDOWN    15
#define FH2_GLYPH_DPAD_LEFTRIGHT 16
#define FH2_GLYPH_L3             17
#define FH2_GLYPH_R3             18
#define FH2_GLYPH_LS             19   /* the left stick, and its directions below */
#define FH2_GLYPH_LS_UPDOWN      20
#define FH2_GLYPH_LS_LEFTRIGHT   21
#define FH2_GLYPH_RS             22
#define FH2_GLYPH_RS_UPDOWN      23
#define FH2_GLYPH_RS_LEFTRIGHT   24

/* One button prompt (API 10): a glyph when the pad was used last, a key when the keyboard was. Frozen: it's
   embedded in other structs and passed in arrays, so it never grows; `reserved` must be zero. */
struct FH2Prompt {
    uint32_t glyph;                /* FH2_GLYPH_ */
    uint32_t vk;                   /* the key, a Windows virtual-key code (VK_SPACE, 'R'); 0 = the glyph always */
    uint32_t modifiers;            /* FH2_KEY_CTRL | FH2_KEY_SHIFT | FH2_KEY_ALT (API 9), shown before it */
    uint32_t word;                 /* FH2_WORD_: the game's own word instead of `label` */
    const char* hotkey;            /* or the key of one of your hotkeys: shows its current binding (a chord as up to
                                      three glyphs), and glyph, vk and modifiers are ignored; NULL = not used */
    const char* label;             /* UTF-8: "Keep it"; ignored when `word` is set */
    int32_t hold;                  /* nonzero: "Hold" before it, with the hold fill (the HUD layer only) */
    uint32_t reserved[5];
};

/* Pop-ups (API 10). */
#define FH2_TOAST_FLAG_URGENT  0x01  /* a fault the player must know about and act on: first in the queue;
                                        2 a session per mod, later ones treated as normal */
#define FH2_TOAST_FLAG_REPLY   0x02  /* answers the player's own press: honoured within 1 s of your hotkey,
                                        command or callback being called; dropped if it can't show within 6 s */
#define FH2_TOAST_FLAG_PRIVATE 0x04  /* the text may name people: the log gets your mod and the key only */
#define FH2_TOAST_MAX_WAITING  4

struct FH2Toast {
    uint32_t size;                 /* sizeof(FH2Toast) */
    const char* key;               /* letters, digits and '_': replaces your waiting pop-up with the same key;
                                      NULL = never replaced */
    const char* title;             /* UTF-8, shown in capitals; 28 characters */
    const char* subtitle;          /* UTF-8, as given; 45 characters; may be NULL */
    uint32_t icon;                 /* FH2_ICON_: one of the game's nine, or none */
    uint32_t sound;                /* FH2_SOUND_ when it appears; 0 = none, as the game's own pop-ups */
    uint32_t flags;                /* FH2_TOAST_FLAG_ */
    uint32_t expire_s;             /* dropped once it could have shown for this long and didn't; 0 = 120; 1 to 600 */
};

/* Tips (API 10). */
#define FH2_TIP_FLAG_UNTIL_DONE 0x01 /* seen only when you call set_tip_seen, not when drawn */

struct FH2Tip {
    uint32_t size;                 /* sizeof(FH2Tip) */
    const char* key;               /* the seen key: once seen, show_tip with it returns 0; NULL = no seen state */
    const char* text;              /* UTF-8, with "{button}" where the button goes; 45 characters in all */
    FH2Prompt button;              /* glyph 0, vk 0 and no hotkey = no button */
    uint32_t delay_s;              /* seconds of driving, with nothing else showing, before it shows; 0 = 60 */
    uint32_t max_sessions;         /* FH2_TIP_FLAG_UNTIL_DONE only: shown in at most this many sessions; 0 = 1 */
    uint32_t flags;                /* FH2_TIP_FLAG_ */
};

/* Message boxes (API 10). Called on the game's UI thread when the player picks a choice (from 0) or the Back
   the platform adds (FH2_BOX_BACK). May never be called if the player leaves the box another way. Keep it
   short, and don't call into the game from it. */
typedef void (*FH2BoxChooseFn)(void* user, const char* key, uint32_t choice);
#define FH2_BOX_BACK 0xFFFFFFFFu

struct FH2MessageBox {
    uint32_t size;                 /* sizeof(FH2MessageBox) */
    const char* key;               /* passed to on_choose */
    const char* title;             /* UTF-8, shown in capitals; 30 characters */
    const char* message;           /* UTF-8, up to 200 characters, wrapped; may be NULL */
    const char* const* choices;    /* UTF-8, copied; put the safe one first; no Back of your own */
    uint32_t choice_count;         /* 1 to FH2_TITLE_MAX_CHOICES - 1 (the platform adds Back); over 10, 8 at a time */
    uint32_t focus;                /* the choice the focus starts on */
    FH2BoxChooseFn on_choose;      /* may be NULL */
    void* user;
};

/* Hint kinds (show_page_hint). */
#define FH2_HINT_INFO      0   /* in the colour of the platform's own hints */
#define FH2_HINT_ATTENTION 1   /* orange, with the game's info icon */
#define FH2_HINT_FAULT     2   /* red, with its alert icon */

/* The platform reads only `size` bytes of FH2Toast, FH2Tip and FH2MessageBox, so a mod built against a later
   header that adds members at their end still works, and one built against this header works on a later
   platform. FH2Prompt is frozen instead. */

/* Online play, for FH2MP (platform 2.9.0): the platform's own interface,
   there from every mod's fh2mod_init on:
       const FH2PlatformOnline* o = (const FH2PlatformOnline*)
           api->get_interface(FH2_ONLINE_INTERFACE, 1);
   NULL on an older platform. docs/api.md, "Online play", has the rest.

   playing_online: the same as FH2ModApi.playing_online (1 online, 0 not).

   going_online: call it before connecting. It reports before it changes
   anything. Returns how many mods that shouldn't run online have already
   run in this game (offline, before FH2MP's session was found): their hooks
   or byte patches went in, or their game text, SQL or files were written for
   the game, which the platform can't undo; a mod from before the platform
   that's loaded counts too. Their names go in `names` as "Nitro, Other",
   UTF-8, cut short between characters to fit `size` and NUL-terminated.
   `names` may be NULL (or `size` 0) to get only the count.
   0: the game counts as online from then on, and the mods that shouldn't run
   online but hadn't run yet are stopped until the game restarts, as at load.
   Connect.
   Over 0: nothing changed (no mod stopped, playing_online still 0 if it was):
   stay offline, and ask the player to start the game from the FH2MP
   launcher.
   Once the game counts as online (at load, or after a 0), it changes nothing
   and returns the mods that had run before the platform stopped them. Over 0
   then still means stay offline, and ask the player to restart the game from
   the FH2MP launcher: playing_online is already 1, and those mods stay
   stopped until the restart.
   It never rewrites the string tables or the game database. Call it from
   FH2MP's own thread, as often as you like. */
#define FH2_ONLINE_INTERFACE "FH2Platform.Online"

typedef struct FH2PlatformOnline {
    uint32_t size;                 /* sizeof(FH2PlatformOnline) */
    int (*playing_online)(void);
    int (*going_online)(char* names, size_t size);
} FH2PlatformOnline;

/* What a mod exports. */
typedef int (*FH2ModInitFn)(const FH2ModApi* api);

#ifdef __cplusplus
}
#endif

#endif /* FH2MOD_H */
