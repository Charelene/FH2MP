/* The garage rating guard (crash family "heap-corruption-game-12e0de0": game+0x12e0de0 / +0x12e0df5 or a 0xc0000374 exit 5-30 s after
 * every start; report 2cb2ec5004c023d3, #62, #63, #444).
 * While the save loads, every garage car is rated: CGarage::LoadCarById game+0x2dac40 -> CCarInGarage ctor +0x2be790 -> +0x2f5120 (sets
 * the car's parts) -> CCarInGarage::UpdatePerformanceColumns game+0x2cfa50(this):
 *     1402cfaca  lea  r8,[rcx+0x48]          ; the car's installed CParts (180 bytes)
 *     1402cface  mov  edx,[rcx+0x40]         ; the car's ordinal (the Data_Car Id)
 *     1402cfad8  call 0x1402d0160            ; CarPerformance_BuildSnapshotForGarageCar
 * +0x2d0160 builds a car info with use-stock 0 (info+0x168, read by +0x1056210), so the installed parts are resolved, then
 * CarPerformance_SimulateAndComputeRatings +0x1044da0 builds the car's physics; a torque curve with no samples underflows the loop of
 * the curve constructor +0x12e0a50 [confirmed: October dumps #106/#128 for the constructor, the mod-crash stack of 2cb2ec5004c023d3 for
 * this route, disassembly]. +0x2cfa50 then copies the ratings into this+0x260..0x2b8 and the torque samples (count this+0xaa4) and, for
 * a car with a save row (this+0x2bc), writes them to the save's table [confirmed: decompile]. The torque load guard does not see this
 * route: it guards the asynchronous preloader +0xfcfe70.
 * The guard hooks +0x2cfa50: it checks the car's ordinal and installed parts with torque_parts_valid(), the resolution +0x1056210 makes.
 * For a car whose engine data is unusable it does not rate the car: the rating fields and the sample count are left at 0 (the CCarInGarage
 * comes from the game's heap, not zeroed), ClassID (+0x2cc, from the save row) and the save are not touched. The car stays in the garage
 * as it is; driving it goes through the torque load guard's placeholder. A car whose data cannot be read is rated by the game as before.
 * FH2MP_NO_GARAGERATING=1 switches it off. Online runs only (FH2MP_OFFLINE skips the shim's guards). */
#include "garage_rating_rule.inc"

#define GR_FN 0x2cfa50u
typedef void (*gr_fn)(unsigned char *);
static gr_fn g_gr_orig;
static volatile LONG g_gr_hits;

/* push rbp ; push rbx ; push rsi ; push rdi ; push r12 ; push r14 ; push r15 ; lea rbp,[rsp-0x3de0] */
static const unsigned char g_gr_head[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x20};
/* +0x2cfaca: lea r8,[rcx+0x48] ; mov edx,[rcx+0x40] ; lea rcx,[rbp+0x470] ; call 0x1402d0160 */
static const unsigned char g_gr_site[19] = {0x4c, 0x8d, 0x41, 0x48, 0x8b, 0x51, 0x40, 0x48, 0x8d, 0x8d, 0x70, 0x04, 0x00, 0x00,
                                            0xe8, 0x83, 0x06, 0x00, 0x00};

static void gr_detour(unsigned char *car_in_garage)
{
    unsigned car, parts[45], row;
    void *singleton = NULL, *db = NULL;
    torque_fault fault = {0, 0};
    LONG n;
    if (!car_in_garage || !read_game_memory(g_torque_load_img + 0x354f4f8, &singleton, sizeof singleton) || !singleton ||
        !read_game_memory((unsigned char *)singleton + 0x40, &db, sizeof db) || !db) {
        g_gr_orig(car_in_garage);                                 /* no game database to check with: the game's code as before */
        return;
    }
    memcpy(&car, car_in_garage + 0x40, sizeof car);
    memcpy(parts, car_in_garage + 0x48, sizeof parts);
    if (torque_parts_valid(db, car, parts, &fault)) {
        g_gr_orig(car_in_garage);
        return;
    }
    gr_clear(car_in_garage);
    memcpy(&row, car_in_garage + 0x2bc, sizeof row);
    n = InterlockedIncrement(&g_gr_hits);
    if (n <= 20 || n % 50 == 0) {
        char line[400];
        wsprintfA(line, "garagerating: garage car %u (save row %u, engine part %u, camshaft part %u) is not rated (#%ld): %s, value %u; "
                  "it stays in the garage with empty performance numbers instead of the torque curve constructor running past its samples "
                  "(game+0x12e0a50)", car, row, parts[2], parts[14], (long)n, fault.why, fault.detail);
        log_begin();
        log_raw(line);
        log_raw("\n");
    }
}

static int patch_garage_rating_guard(void)
{
    if (!torque_bind()) {
        return 0;
    }
    if (!code_is("garagerating", "UpdatePerformanceColumns", g_torque_load_img, GR_FN, g_gr_head, sizeof g_gr_head) ||
        !code_is("garagerating", "the garage snapshot call", g_torque_load_img, 0x2cfacau, g_gr_site, sizeof g_gr_site)) {
        return 0;
    }
    if (!plat_hook("garagerating: garage car rating", g_torque_load_img + GR_FN, (void *)gr_detour, (void **)&g_gr_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: garagerating: a garage car whose engine data is unusable is not rated while the save loads (game+0x2cfa50)\n");
    return 1;
}
