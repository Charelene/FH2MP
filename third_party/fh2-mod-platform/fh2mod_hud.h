/*
 * fh2mod_hud.h -- the HUD layer, a preview (MIT licensed, see LICENSE).
 *
 * PREVIEW. This isn't part of the API in fh2mod.h: it's an interface the
 * platform publishes, and platform 2.9.0 publishes it as a preview, version
 * 2 (with the feed). Version 1 was only in a test build before 2.9.0, and
 * released 2.8.0 and older don't offer it. When the layer moves into the
 * API, its names, limits and behaviour may all change, and this interface
 * may go. Never make it the only way your mod works.
 *
 * The layer is a heads-up display of your own, drawn by the game's own UI
 * inside its free-roam HUD page, in the game's look and fonts:
 *   - the bar along the bottom of the screen: tabs on the left between LB and
 *     RB, two status lines in the middle and up to five button prompts on the
 *     right, with a strip of marks (a timeline with a playhead, or dots) on
 *     a row of its own just above the bar;
 *   - chips: up to four small labels at the top left, and one at the top
 *     right;
 *   - a tip card, centred just above the bar;
 *   - a panel of up to eight rows, over the left of the bar;
 *   - a countdown: a big number in a ring, in the middle of the screen;
 *   - a shutter: a short white blink across the whole picture.
 * You fill the parts with text and numbers. The platform owns the layout, so
 * nothing you set can break the game's HUD page. Prompts follow the device
 * the player used last: the game's own controller pictures, or key names on
 * key caps. The player can fix either with the "Button prompts" row on the
 * first layer mod's page under MODS > YOUR MODS.
 *
 * Getting it, in fh2mod_init after register_mod. The platform offers it
 * before any mod starts, so unlike other mods' interfaces it's there in
 * fh2mod_init:
 *
 *     #include "fh2mod_hud.h"
 *     const FH2HudInterface* hud = NULL;
 *     if (api->api_version >= 2)
 *         hud = (const FH2HudInterface*)api->get_interface(
 *             FH2_HUD_INTERFACE, FH2_HUD_INTERFACE_VERSION);
 *     if (hud && hud->size < sizeof(FH2HudInterface))
 *         hud = NULL;
 *     if (!hud)
 *         api->log(mod, "no HUD layer on this platform; carrying on without the bar");
 *
 * get_interface came with API 2, hence the version check. Platform 2.8.0
 * and older return NULL there.
 *
 * Where it shows. Only while the game's free-roam HUD page is on screen.
 * In testing, the pause menu, the map, photo mode, the rewind (with its
 * loading screen) and the telemetry HUD each put another page in its place,
 * and the layer went with it; replays, event intros and the game's other
 * cutscenes haven't been checked. FH2_HUD_ON_SCREEN says when the layer can
 * be seen. A frame that must be clean of it (a screenshot, say) is yours to
 * clear: show_hud_layer(mod, FH2_HUD_PART_INSTANT), then wait for
 * FH2_HUD_CLEAN.
 *
 * The game's own HUD. The layer draws over it. The bar covers the bottom of
 * the screen, where the game puts its speedometer, map and notifications, so
 * show the bar with the game's HUD held hidden (hold_game_hud). While it's
 * held, the game's small notifications and its big banners wait until it's
 * let go (one waited over two minutes in testing), the save spinner waits, a
 * prompt the car passes in the world ("Press A to start an event") is lost
 * rather than kept for later, and Drivatar name tags stay. Hold it while your
 * bar is up, and no longer.
 *
 * Threads. Every call is quick and can be made from any thread, apart from
 * add_hud_layer (fh2mod_init only), and copies what it's given. Nothing
 * reaches the game before the platform's next frame: once a frame, on the
 * game's UI thread, the platform takes the newest of what you set and tells
 * the HUD page what changed. A big change reaches the page over a few frames
 * (24 values a frame), so it can take a few hundredths of a second; hiding is
 * never held back. on_status runs on that thread too.
 *
 * Cost. Values that don't change cost nothing. A frame that changes anything
 * costs the game's UI about 0.15 ms, plus about 0.04 ms for each value; a
 * playhead or a hold ring moving every frame costs about 0.2 ms a frame. Set
 * text only when it changes: the platform drops sets that change nothing, and
 * passes one text on at most 10 times a second.
 *
 * Letters. In testing, the HUD's fonts showed English, the Western European
 * letters (Latin-1: e acute, u umlaut, sharp s, o slash and the rest), S and
 * Z with caron, Polish and Danish. Cyrillic, Japanese and some other Latin
 * letters (g circumflex, h stroke, k cedilla, r acute, t cedilla) came out
 * as boxes. Czech, Slovak, Hungarian and Romanian letters (r and u with caron
 * or ring, o with a double acute, s comma) and Greek haven't been checked:
 * expect boxes. The log says once when a text has letters outside the set
 * that was seen working. Line breaks, tabs and other control characters
 * become spaces.
 *
 * Room. The bar's tabs and prompts take the width they need, and the middle
 * gets what's left (1704 pixels of the 1920-wide picture, less the tabs and
 * prompts), trimming its text with an ellipsis. Five short tabs and five
 * prompts leave it about 300 pixels with key caps; keep tabs and labels
 * short, or the middle has little room.
 */
#ifndef FH2MOD_HUD_H
#define FH2MOD_HUD_H

#include "fh2mod.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FH2_HUD_INTERFACE         "FH2Platform.HudLayer"
#define FH2_HUD_INTERFACE_VERSION 2   /* 2: post_hud_feed and FH2_HUD_PART_FEED (2.9.0) */

/* Limits. Text longer than its limit (given with each field below, in
   UTF-16 characters) is cut by the platform; the HUD also trims text that
   doesn't fit its place, with an ellipsis. */
#define FH2_HUD_MAX_LAYERS    4    /* mods with a layer; one is on screen at a time */
#define FH2_HUD_MAX_TABS      8
#define FH2_HUD_MAX_PROMPTS   5
#define FH2_HUD_MAX_MARKS     16   /* on the strip's row */
#define FH2_HUD_MAX_DOTS      8    /* at the end of line 2: the first marks */
#define FH2_HUD_MAX_CHIPS     4    /* at the top left; FH2HudState.corner is the fifth, at the top right */
#define FH2_HUD_MAX_CHIP_DOTS 5
#define FH2_HUD_MAX_ROWS      8
#define FH2_HUD_MAX_FEED      5    /* feed lines on screen at once, from every mod together */

/* Parts (FH2HudLayer.parts, show_hud_layer). Only the parts a layer has are
   written into the HUD page. */
#define FH2_HUD_PART_BAR       0x01u        /* the bar: tabs, status lines, the strip's row, prompts */
#define FH2_HUD_PART_CHIPS     0x02u        /* the chips at the top left and the one at the top right */
#define FH2_HUD_PART_TIP       0x04u        /* the tip card, just above the bar */
#define FH2_HUD_PART_PANEL     0x08u        /* the rows, over the left of the bar */
#define FH2_HUD_PART_COUNTDOWN 0x10u        /* the number in a ring, in the middle */
#define FH2_HUD_PART_ALL       0x1Fu        /* the parts show_hud_layer shows: every one but the feed */
/* The feed (interface version 2): a few lines of text at the left, above the
   game's minimap, newest at the bottom, like a chat log. Unlike the other
   parts it's shared and never shown with show_hud_layer: every mod whose
   layer has it posts lines (post_hud_feed), and the feed is on screen while
   it has lines, whichever layer owns the screen. A layer can have only the
   feed: then it never takes the screen, has no lease, and gets no Button
   prompts row. The feed stays out of the way: it's hidden while a layer shows
   its bar or its panel, while the game's own HUD is held hidden, and from a
   show_hud_layer with FH2_HUD_PART_INSTANT (a clean frame) until the next
   show without it. */
#define FH2_HUD_PART_FEED      0x20u
#define FH2_HUD_PART_INSTANT   0x80000000u  /* show_hud_layer: parts going out go at once instead of fading */

/* FH2HudFeedLine.style: the colour of the line's text (its name is always
   white). */
#define FH2_HUD_FEED_PLAIN  0               /* white */
#define FH2_HUD_FEED_GREEN  1               /* the HUD's green, as for a message from a player */
#define FH2_HUD_FEED_ORANGE 2               /* the menus' orange */
#define FH2_HUD_FEED_YELLOW 3               /* the HUD's yellow, for a warning */

/* set_hud_value. */
#define FH2_HUD_PLAYHEAD 1                  /* 0 to 1 along the timeline */
#define FH2_HUD_HOLD     2                  /* 0 to 1: the held prompt's ring (or the bar under its key caps) */

/* pulse_hud. (2 is kept for a later "nudge".) */
#define FH2_HUD_PULSE_SHUTTER 1             /* a white blink across the picture, a tenth of a second */

/* hud_status bits. */
#define FH2_HUD_AVAILABLE     0x001u  /* your layer is in this session's HUD page and the platform's hooks for it are on */
#define FH2_HUD_SHOWN         0x002u  /* some of your parts are shown (show_hud_layer) and the screen is yours */
#define FH2_HUD_ON_SCREEN     0x004u  /* the free-roam HUD page is on screen, so a shown layer is seen */
#define FH2_HUD_CLEAN         0x008u  /* nothing of the HUD layer has been drawn for 3 frames: a capture is clean of it */
#define FH2_HUD_KEYBOARD      0x010u  /* the prompts show key caps */
#define FH2_HUD_GAME_HIDDEN   0x020u  /* the game's own HUD is held hidden now, by any mod */
#define FH2_HUD_TAKEN         0x040u  /* another mod's layer took the screen: show yours again to take it back */
#define FH2_HUD_DEVICE_FORCED 0x080u  /* the player chose Controller or Keyboard and mouse for the prompts */
#define FH2_HUD_LAPSED        0x100u  /* your lease ran out: the layer was hidden and your hold let go
                                         (until your next show_hud_layer with parts, or hold_game_hud(1)) */

/* Controller pictures (FH2HudPrompt.glyph): the game's own, FH2_GLYPH_NONE
   to FH2_GLYPH_RS_LEFTRIGHT (0 to 24), defined once in fh2mod.h (API 10),
   where the UI templates' FH2Prompt uses the same numbers. 1 to 6 are the
   same numbers as FH2_BUTTON_A to FH2_BUTTON_RB. */

/* Colours and kinds. */
#define FH2_HUD_TEXT_DIM      0       /* line1_after in light grey */
#define FH2_HUD_TEXT_WARN     1       /* in the HUD's yellow */
#define FH2_HUD_TEXT_ACCENT   2       /* in the menus' orange */
#define FH2_HUD_CENTRE_NORMAL 0
#define FH2_HUD_CENTRE_ALERT  1       /* the middle of the bar in amber, for a warning */
#define FH2_HUD_STRIP_NONE    0
#define FH2_HUD_STRIP_DOTS    1       /* dots: at the end of line 2, or with strip_row on the strip's row, centred on the screen */
#define FH2_HUD_STRIP_RAIL    2       /* a timeline on the strip's row: marks at their `at`, labels at its ends */
#define FH2_HUD_MARK_DOT      0
#define FH2_HUD_MARK_DIAMOND  1
#define FH2_HUD_MARK_NORMAL   0       /* white */
#define FH2_HUD_MARK_CURRENT  1       /* orange */
#define FH2_HUD_MARK_DIM      2       /* faded white */

/* Sounds (play_ui_sound): the game's own, FH2_SOUND_ in fh2mod.h (API 10),
   shared with the UI templates, whose api->play_ui_sound plays all eleven
   with no layer (see play_ui_sound below). This preview plays only 5 to 11;
   1 to 4 are the menus' own sounds, which it doesn't play:
     FH2_SOUND_ERROR    the menus' "can't do that"; quiet in the world
     FH2_SOUND_NOTIFY   the game's notification chime; in the world only
     FH2_SOUND_INFO     a pop-up opening; faint in the world
     FH2_SOUND_TICK     the race countdown's beep; in the world only
     FH2_SOUND_TICK_LOW its lower beep; in the world only
     FH2_SOUND_TICK_END its last beep; in the world only
     FH2_SOUND_SHUTTER  the photo shutter */

/* Called on the game's UI thread, at the platform's frame, when your
   layer's FH2_HUD_ bits change. Keep it short (store the bits, set a flag)
   and don't call into the game from it; this interface's calls are fine.
   If it crashes, it isn't called again, your layer is hidden and your hold
   let go. */
typedef void (*FH2HudStatusFn)(void* user, uint32_t status);

/* Your layer (add_hud_layer). */
typedef struct FH2HudLayer {
    uint32_t size;               /* sizeof(FH2HudLayer) */
    uint32_t parts;              /* the FH2_HUD_PART_ bits you'll use */
    uint32_t lease_ms;           /* 250 to 60000, or 0 for the default of 5000. Every layer has a lease:
                                    when none of your calls (hud_status included) comes for this long while
                                    your layer is shown or your hold is on, the platform hides the layer,
                                    lets go of your hold and sets FH2_HUD_LAPSED, so code of yours that
                                    stopped can't leave the player without the game's HUD. Time with the
                                    HUD page off screen (a menu, the map) doesn't count. */
    FH2HudStatusFn on_status;    /* may be NULL */
    void* user;                  /* passed to on_status */
} FH2HudLayer;

/* One button prompt, or the tip card's button. It's embedded in FH2HudState
   and frozen: it never grows, and `reserved` must be zero. */
typedef struct FH2HudPrompt {
    uint32_t glyph;              /* FH2_GLYPH_: the controller picture; 0 = none (the key caps always) */
    uint32_t chord;              /* nonzero: shown as LB + RB + the glyph, like the platform's hotkey chords */
    const char* key;             /* UTF-8, on key caps when the keyboard or mouse was used last: "Space",
                                    "Ctrl+Z" (two caps), "Left/Right" (two arrows). "Up", "Down", "Left" and
                                    "Right" are drawn as arrows; the rest in capitals, 12 each. NULL or "" =
                                    the glyph always */
    const char* label;           /* UTF-8, after it: "Place", 24. Ignored for the tip's button */
    int32_t hold;                /* nonzero: "HOLD" before it, and FH2_HUD_HOLD fills a ring round its glyph (a
                                    bar under its key caps). Ignored for the tip's button */
    uint32_t reserved[3];
} FH2HudPrompt;

/* One mark on the strip. Frozen: `reserved` must be zero. */
typedef struct FH2HudMark {
    float at;                    /* 0 to 1 along the timeline; ignored for dots */
    uint8_t shape;               /* FH2_HUD_MARK_DOT or _DIAMOND (dots at the end of line 2 are always dots) */
    uint8_t style;               /* FH2_HUD_MARK_NORMAL, _CURRENT or _DIM */
    uint16_t number;             /* under it, on the strip's row: 1 to 999; 0 = none */
    uint32_t reserved;
} FH2HudMark;

/* One chip. Frozen: `reserved` must be zero. */
typedef struct FH2HudChip {
    const char* text;            /* UTF-8, shown in capitals: "EDITING", 24. NULL or "" with no dots = no chip.
                                    At the top left about 15 capitals show before the ellipsis (the four
                                    there share the top with the corner chip); in the corner, all 24 */
    uint8_t dots;                /* small dots after the text, 0 to FH2_HUD_MAX_CHIP_DOTS */
    uint8_t dots_filled;         /* how many of them are filled, from the left */
    uint8_t reserved[6];
} FH2HudChip;

/* One row of the panel. Frozen: `reserved` must be zero. */
typedef struct FH2HudRow {
    const char* label;           /* UTF-8, on the left, in capitals: "SURFACE", 32 */
    const char* value;           /* UTF-8, on the right, between < and >: "Tarmac", 32; NULL or "" = none */
    int32_t style;               /* FH2_ROW_NORMAL, FH2_ROW_DIM or FH2_ROW_HEADING (fh2mod.h) */
    uint32_t reserved[3];
} FH2HudRow;

/* What your layer shows (set_hud_state). Zero it, set size, and fill what
   you use: NULL or "" leaves a text out. Text is UTF-8; the number after
   each field is the most characters shown. */
typedef struct FH2HudState {
    uint32_t size;                             /* sizeof(FH2HudState) */
    /* the tabs, at the bar's left between LB and RB */
    const char* tabs[FH2_HUD_MAX_TABS];        /* in capitals, 16 each; the first NULL or "" ends them */
    uint32_t tab;                              /* the selected one */
    const char* tab_keys[2];                   /* key caps for LB and RB with the keyboard: "Shift+Tab", "Tab",
                                                  12 each, one cap each; give both or neither: with
                                                  either NULL or "", the LB and RB pictures always */
    /* the middle */
    const char* line1;                         /* in capitals, 40: "CHECKPOINT 2 OF 3" */
    const char* line1_after;                   /* after it, in line1_after_style, 24: "MOVED" */
    int32_t line1_after_style;                 /* FH2_HUD_TEXT_ */
    const char* line2;                         /* smaller, under line 1, 64 */
    int32_t centre;                            /* FH2_HUD_CENTRE_ */
    /* the strip */
    int32_t strip;                             /* FH2_HUD_STRIP_ */
    int32_t strip_row;                         /* nonzero: dots on the strip's own row (a timeline always is) */
    const char* strip_left;                    /* at the timeline's start, 12: "0:00" */
    const char* strip_right;                   /* at its end, 12: "1:30" */
    const char* strip_after;                   /* at the row's right end, or after the dots on line 2, 48 */
    FH2HudMark marks[FH2_HUD_MAX_MARKS];
    uint32_t mark_count;
    int32_t playhead;                          /* nonzero: the timeline's playhead, at FH2_HUD_PLAYHEAD */
    /* the prompts, from the bar's right end leftwards, as in the game's own help bars */
    FH2HudPrompt prompts[FH2_HUD_MAX_PROMPTS];
    uint32_t prompt_count;
    /* the chips */
    FH2HudChip chips[FH2_HUD_MAX_CHIPS];       /* at the top left, in a row */
    FH2HudChip corner;                         /* at the top right */
    /* the tip card: text, one button, text */
    const char* tip_before;                    /* "Press", 48 */
    FH2HudPrompt tip_button;                   /* its glyph, chord and key; glyph 0 and no key = no button */
    const char* tip_after;                     /* "to drop a checkpoint", 48 */
    /* the panel */
    FH2HudRow rows[FH2_HUD_MAX_ROWS];
    uint32_t row_count;
    uint32_t row_selected;                     /* filled in orange; row_count or more = none; a heading never is */
    const char* panel_footer;                  /* a line under the rows, 96 */
    /* the countdown */
    uint32_t countdown;                        /* the number, 1 to 999 (two digits fit inside the ring), popping in
                                                  each time it changes; 0 = none */
    const char* countdown_label;               /* under the ring, 48: "TEST RUN" */
} FH2HudState;

/* A feed line (post_hud_feed, version 2). */
typedef struct FH2HudFeedLine {
    uint32_t size;               /* sizeof(FH2HudFeedLine) */
    const char* name;            /* who: "Trinculo54"; NULL or "" for none */
    const char* text;            /* what: "Nice race!" */
    uint32_t style;              /* FH2_HUD_FEED_ */
    uint32_t ms;                 /* how long it stays: 0 for 8000 */
} FH2HudFeedLine;

/* The interface: get_interface(FH2_HUD_INTERFACE, FH2_HUD_INTERFACE_VERSION). */
typedef struct FH2HudInterface {
    uint32_t size;               /* sizeof(FH2HudInterface) as the platform knows it */
    uint32_t version;            /* FH2_HUD_INTERFACE_VERSION it offers */

    /* Adds your layer, with the parts you'll use. From fh2mod_init only:
       the HUD page is written once every mod's fh2mod_init has returned, so
       a layer can't be added later (it returns 0). One layer per mod,
       FH2_HUD_MAX_LAYERS from every mod together. The first mod to add one
       also gets the "Button prompts" row on its page under MODS > YOUR MODS
       (Automatic, Controller, Keyboard and mouse). Returns 1 if added, 0 if
       not (the log says why). Even then, check hud_status for
       FH2_HUD_AVAILABLE before relying on the layer: the platform leaves it
       out when the game's code or UI files aren't as expected, for one
       session after the last one crashed between start-up and the HUD
       page's first 10 s on screen (whatever the crash was), or when the
       game didn't read the platform's HUD page (the log says which). */
    int (*add_hud_layer)(FH2Mod* mod, const FH2HudLayer* layer);

    /* Everything your layer shows apart from the two per-frame numbers,
       copied (a struct on your stack is fine). Each call replaces the whole
       state, so pass all of it every time, and only when something changed;
       only what changed reaches the game. A part that isn't shown keeps what
       you set for when it is, and a part going out keeps showing what it had
       until it has faded. Mind the order: when parts go out, call
       show_hud_layer first and set_hud_state after it, or the new state can
       empty them before they fade; when parts come in, set the state first.
       Any thread. Returns 1, or 0 if you have no layer or state->size is
       less than sizeof(FH2HudState) (logged once). */
    int (*set_hud_state)(FH2Mod* mod, const FH2HudState* state);

    /* A number that may change every frame, 0 to 1: FH2_HUD_PLAYHEAD (the
       playhead's place along the timeline, with FH2HudState.playhead set)
       or FH2_HUD_HOLD (how full a held prompt's ring is). Any thread; the
       newest value each frame wins, and one that doesn't change costs
       nothing. Returns 1, or 0 for no layer or an unknown `which`. */
    int (*set_hud_value)(FH2Mod* mod, int which, float value);

    /* Which of your parts are on screen (FH2_HUD_PART_ bits, of the ones you
       added); 0 hides the whole layer. Parts fade in and out over 0.15 s, and
       keep their contents while they fade out; add FH2_HUD_PART_INSTANT to
       take parts away at once (for a clean frame: then wait for
       FH2_HUD_CLEAN). Parts coming in always fade in. Call it before
       set_hud_state when parts go out, and after it when parts come in (see
       set_hud_state). Showing yours hides another mod's layer (it gets
       FH2_HUD_TAKEN), and clears your FH2_HUD_TAKEN and FH2_HUD_LAPSED. Any
       thread. Returns 1, or 0 if you have no layer or it isn't available
       this session. */
    int (*show_hud_layer)(FH2Mod* mod, uint32_t parts);

    /* A one-off effect while your layer is shown: FH2_HUD_PULSE_SHUTTER, a
       white blink across the whole picture, to confirm a change (a
       checkpoint placed, say; play FH2_SOUND_SHUTTER with it for the
       sound). Right after
       show_hud_layer is fine: "shown" means your latest show_hud_layer had
       parts, no other mod's came after it, and the platform hasn't hidden
       it since (your lease running out, or on_status crashing). A pulse
       sent while the HUD page is off screen (a menu, the map) is dropped if
       the page isn't back within 0.4 s. Any thread. Returns 1, or 0 if you
       have no layer, it isn't shown, or the pulse is unknown. */
    int (*pulse_hud)(FH2Mod* mod, int pulse);

    /* Your layer's FH2_HUD_ bits now. Any thread, cheap. Call it from the
       code that drives your layer: it also keeps your lease. */
    uint32_t (*hud_status)(FH2Mod* mod);

    /* Hides the game's own HUD while you hold it (1): its speedometer, map,
       prompts and notifications, but not your layer. Holds are counted
       across mods: the game's HUD comes back when the last one lets go (0),
       or when your lease runs out. The game shows its HUD again by itself at
       times (after a menu, as driving starts), so while it's held the
       platform hides it again each frame. Read "The game's own HUD" at the
       top for what waits while it's held. Your hold stays while another
       mod's layer has the screen (FH2_HUD_TAKEN): show yours again or let
       go, or the player has neither HUD. hold_game_hud(mod, 1) also clears
       FH2_HUD_LAPSED. Any thread; it takes effect at the next frame
       (FH2_HUD_GAME_HIDDEN says when). Returns 1, or 0 if you have no layer,
       the layer is off this session, the game's HUD code isn't as expected,
       or hiding or showing the game's HUD crashed earlier this session (then
       the game's HUD stays; logged once). */
    int (*hold_game_hud)(FH2Mod* mod, int hold);

    /* Plays one of the game's UI sounds (FH2_SOUND_) once, at the next
       frame. New code should use the API's play_ui_sound (api->play_ui_sound,
       API 10, in fh2mod.h) instead: it needs no layer and plays all eleven.
       This one has its own gate and its own repeat rule. It needs the MODS
       tab's hooks, so in a session where they didn't go in it plays nothing
       while the API's still plays; and its 100 ms between repeats is counted
       apart from the API's, so a sound sent through both can play twice in
       one frame. Use one of the two for a sound, not both.
       NOTIFY and the three TICKs only sound in the world, so they're
       refused while the free-roam HUD page isn't on screen (a menu, the map,
       photo mode); the others play anywhere. The same sound at most once per
       100 ms from your mod. Never make a sound the only cue. Any thread.
       Returns 1 if it will play, 0 if not: no layer, the layer off this
       session, an unknown sound, a repeat, or one that can't play where the
       player is (each logged once, apart from a repeat). */
    int (*play_ui_sound)(FH2Mod* mod, uint32_t sound);

    /* ---- version 2 (2.9.0): check version >= 2 (or size) before using these. ---- */

    /* Adds a line to the feed, at the bottom; the oldest of FH2_HUD_MAX_FEED
       lines makes room. `name` (24, may be NULL) shows in white with a colon
       after it, `text` (64) in `style`'s colour, on one line trimmed at 720
       pixels. A line stays for `ms` (0: 8000; 2000 to 30000) of time with the
       HUD page on screen, so a line posted in a menu waits for the HUD. Needs a
       layer with FH2_HUD_PART_FEED. Any thread. Returns 1, or 0 if you have no
       feed, the text is empty, or the layer is off this session (logged once). */
    int (*post_hud_feed)(FH2Mod* mod, const FH2HudFeedLine* line);
} FH2HudInterface;

#ifdef __cplusplus
}
#endif

#endif /* FH2MOD_HUD_H */
