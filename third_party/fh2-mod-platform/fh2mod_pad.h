/*
 * fh2mod_pad.h -- the pad layer preview of the FH2 Mod Platform (MIT licensed, see LICENSE).
 *
 * A preview: not part of any API version yet. Platform 2.9.0 is the first to offer it, as a
 * preview interface (version 1), so a mod that uses it still loads on a platform without it
 * (2.8.0 and older):
 *
 *     const FH2PadInterface* pad = (const FH2PadInterface*)api->get_interface(FH2_PAD_INTERFACE, 1);
 *     if (!pad) ...   (no pad layers on this platform: say so in your log, carry on without)
 *
 * The interface is there from your fh2mod_init on. Its shape can still change before it
 * becomes an API version, so never make it the only way your mod can be used.
 *
 * A pad layer lets a mod use some controller buttons while the player holds a modifier,
 * without the game also acting on them. While the modifier is held, the game doesn't see the
 * layer's buttons (the d-pad doesn't change the radio station, say), and the platform calls
 * on_press once for each one pressed. When it's let go, the game sees everything again; a
 * button that was held through the release only reaches the game once it's pressed again.
 *
 * The modifier itself always reaches the game: pick one that does nothing harmful while held,
 * and let the player change it (set_modifier).
 *
 * A layer is on in every game state: the title screen, the pause menu and the MODS tab, the
 * map and photo mode as well as driving. There, too, the game doesn't see the layer's buttons
 * while the modifier is held, and on_press still comes. LB is the menus' tab-left, so a player
 * holding it in a menu and pressing the d-pad would send your action from the menu. If your
 * layer is for driving, ignore presses while a menu is open: FH2_HUD_ON_SCREEN in hud_status
 * (with a HUD layer of your own, fh2mod_hud.h), or FH2_UI_ON_SCREEN in ui_status (API 10, in a
 * session with pop-ups or tips: declare them with ui_uses), is set while the free-roam HUD page
 * is on screen. Without FH2_HUD_AVAILABLE, or FH2_UI_TOASTS, the platform may not be following
 * that page and the bit may never come, so then let presses through rather than drop them all.
 * A flag for layers that are only on in the world is planned for 2.9.1, in the reserved
 * `flags`.
 */
#ifndef FH2MOD_PAD_H
#define FH2MOD_PAD_H

#include "fh2mod.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FH2_PAD_INTERFACE "FH2Platform.PadLayer"
#define FH2_PAD_INTERFACE_VERSION 1

/* Called on a platform thread (not the game's) once per press of one of the layer's buttons
   while its modifier is held. `button` is one FH2_PAD_ bit. Keep it short. */
typedef void (*FH2PadPressFn)(void* user, const char* key, uint32_t button);

/* One pad layer. */
typedef struct FH2PadLayer {
    uint32_t size;          /* sizeof(FH2PadLayer) */
    const char* key;        /* unique within your mod: letters, digits and '_', like "QuickChat" */
    uint32_t modifier;      /* FH2_PAD_ bits, all held to open the layer, like FH2_PAD_LB; 0 = off for now */
    uint32_t buttons;       /* FH2_PAD_ bits the layer takes while open, like the four d-pad bits */
    uint32_t flags;         /* 0 (reserved: a flag for a layer only on in the world is planned) */
    FH2PadPressFn on_press;
    void* user;             /* passed to on_press */
} FH2PadLayer;

typedef struct FH2PadInterface {
    uint32_t size;          /* sizeof(FH2PadInterface) as the platform knows it */
    uint32_t version;       /* FH2_PAD_INTERFACE_VERSION */

    /* Adds a layer. Only from fh2mod_init. Returns 1 if added, 0 if not (the log says why):
       a bad key or bits, the modifier and buttons overlap, or another layer with the same
       modifier already takes one of the buttons. */
    int (*add_layer)(FH2Mod* mod, const FH2PadLayer* layer);

    /* Changes a layer's modifier (the player picked another one in your settings). Any
       time, from any thread; 0 turns the layer off. Returns 1, or 0 for an unknown layer,
       bad bits or a clash with another layer (then it keeps the modifier it had). */
    int (*set_modifier)(FH2Mod* mod, const char* key, uint32_t modifier);

    /* The buttons held on the controller the game is using now, as FH2_PAD_ bits, read
       from the game's own reading of it. Returns 1, or 0 when there's none yet (in the
       title screen's first seconds, say). Any thread. */
    int (*buttons)(uint32_t* held);
} FH2PadInterface;

#ifdef __cplusplus
}
#endif

#endif
