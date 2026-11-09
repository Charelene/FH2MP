/* FH2MP.Online: what FH2MP offers other FH2 Mod Platform mods (a car pack, a garage or tuning mod) so they can keep custom cars and parts out of
 * online events themselves, before the server refuses them.
 *
 * The rule (the FH2MP game server enforces it; this only lets a mod follow it in its own menus):
 *   - In online Free Roam every car may drive. A car the others' game does not have is shown to them as a stand-in car.
 *   - Online events and Road Trip take only cars from the game. A custom car is refused when the event starts. A car from the game with parts from
 *     outside the game races with its stock parts.
 *   - A game that keeps its own car database ("Keep my car packs" in the FH2MP launcher) posts no scores to the leaderboards.
 *
 * Use (platform API 2 or newer, after wait_for_mods):
 *     const FH2MPOnline *mp = (const FH2MPOnline *)api->get_interface(FH2MP_ONLINE_NAME, 1);
 *     if (mp && mp->car_allowed_in_events(car_id) == 0) { ...leave the car out of the online event's car list... }
 * NULL means FH2MP is not installed (or older): there is nothing online to follow.
 *
 * Compatibility: like the platform's API, members are only ever added at the end, and `version` grows with them. Check `version` (or `size`) before
 * using a member added after version 1. Every function may be called from any thread and never blocks. */
#ifndef FH2MP_ONLINE_H
#define FH2MP_ONLINE_H

#include <stdint.h>

#define FH2MP_ONLINE_NAME "FH2MP.Online"
#define FH2MP_ONLINE_VERSION 1

typedef struct FH2MPOnline {
    uint32_t size;                  /* sizeof(FH2MPOnline) of the FH2MP that published it */
    uint32_t version;               /* FH2MP_ONLINE_VERSION of the FH2MP that published it */

    /* ==== version 1 ==== */
    /* 1 when this game was started signed in to the FH2MP services (online), 0 when offline. */
    int (*is_online)(void);
    /* 1 when this game reads its own car database (the player keeps their car packs), 0 when it reads the server's. */
    int (*keeps_own_cars)(void);
    /* 1 when the car (Data_Car id) may race in online events and Road Trip, 0 when it may not (a custom car), -1 when FH2MP cannot tell (a game
       that keeps its own car database but has no list of the server's cars yet). Offline every car may race: 1. */
    int (*car_allowed_in_events)(uint32_t car_id);
    /* 1 when the part (List_Upgrade<table> row id, e.g. table "Engine") is one of the game's, 0 when it is not (it is put back to stock in online
       events and Road Trip), -1 when FH2MP cannot tell. `table` is the name after "List_Upgrade". Offline: 1. */
    int (*part_allowed_in_events)(const char *table, uint32_t part_id);
} FH2MPOnline;

#endif
