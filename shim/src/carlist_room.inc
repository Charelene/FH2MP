/* The car list room guard ("exception 0xc0000005 at game+0xf60526 reading 0x4c72", issue #486, report 404b9f2c749535ba, 2026-11-14, about a minute after a
 * Car Meet ended; no mod on the stack). The world a car drives in is a Simulation (vtable game+0x2e6c508, RTTI ".?AVSimulation@@", constructor
 * game+0xf8b770, which starts its car list empty at +0x2b0/+0x2b8/+0x2c0). Its car list is a std::vector of ref-counted ForzaCar pointers:
 *     game+0xf9ae00   lea rax,[rcx+0x2b0] ; ret                        the list's getter
 *     game+0xf8e820   Simulation vfunc 12: adds a car, push_back (game+0x308e20, called at +0xf8ea16)
 *     game+0x308e20   push_back: when size == capacity it first calls game+0x305a20(list, 1), which moves the whole list to a bigger buffer
 *     game+0xf6dc70   (car) walks the other cars of its Simulation on a worker thread: for each one game+0xf60520(&other->+0x4c58)
 * In the dump the walk's pointer is outside the list's current buffer (14 cars, capacity 19): a car was added on another thread while the worker walked
 * the list, the list moved, and the worker read the freed old buffer [C: dump and disassembly of 1.0.0.64; which thread adds the car is inferred: the
 * world filling up again after a Car Meet]. The guard hooks the constructor and, as soon as a Simulation is made (before any car is in it, so nothing walks
 * its list yet), asks the game's own reserve (game+0x305a20) for room for CRG_ROOM cars: push_back never has to move the list until that many cars are in
 * one world. It uses the game's allocator and changes nothing else; a removal can still leave a walker on a car that is going away, which this does not
 * cover. FH2MP_NO_CARROOM=1 switches it off. */
#include "carlist_room_rule.inc"

#define CRG_CTOR 0xf8b770u
#define CRG_RESERVE 0x305a20u
static const unsigned char g_crg_ctor[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70};
/* +0xf8b7ca: mov [rsi+0x2b0],rbp ; mov [rsi+0x2b8],rbp ; mov [rsi+0x2c0],rbp (the empty list) */
static const unsigned char g_crg_init[21] = {0x48, 0x89, 0xae, 0xb0, 0x02, 0x00, 0x00, 0x48, 0x89, 0xae, 0xb8, 0x02, 0x00, 0x00,
                                             0x48, 0x89, 0xae, 0xc0, 0x02, 0x00, 0x00};
static const unsigned char g_crg_getter[8] = {0x48, 0x8d, 0x81, 0xb0, 0x02, 0x00, 0x00, 0xc3};                       /* +0xf9ae00 */
static const unsigned char g_crg_reserve[28] = {0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x41, 0x08, 0x4c, 0x8b, 0xca, 0x48, 0x8b, 0x51,
                                                0x10, 0x48, 0x8b, 0xc2, 0x49, 0x2b, 0xc0, 0x48, 0xc1, 0xf8, 0x03, 0x49, 0x3b, 0xc1}; /* +0x305a20 */
static const unsigned char g_crg_push[7] = {0x48, 0x8d, 0x8d, 0xb0, 0x02, 0x00, 0x00};                               /* +0xf8ea0a: lea rcx,[rbp+0x2b0] */

typedef void *(*crg_ctor_fn)(void *sim, void *a, void *b);
typedef void (*crg_reserve_fn)(void *list, unsigned long long n);
static crg_ctor_fn g_crg_orig;
static crg_reserve_fn g_crg_reserve_fn;
static volatile LONG g_crg_made;

static void *crg_detour(void *sim, void *a, void *b)
{
    void *out = g_crg_orig(sim, a, b);
    unsigned long long *list = sim ? (unsigned long long *)((char *)sim + 0x2b0) : NULL;
    LONG n;

    if (!list || list[0] != list[1]) {
        return out;                     /* not the empty list the constructor makes: leave it */
    }
    g_crg_reserve_fn(list, CRG_ROOM);
    n = InterlockedIncrement(&g_crg_made);
    if (n <= 3 || n % 50 == 0) {
        log_begin();
        log_raw("carroom: a new world's car list has room for ");
        log_dec((unsigned long)((list[2] - list[0]) / 8));
        log_raw(" cars before it would move (the list moved under a worker thread in #486) (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
    return out;
}

static int patch_carlist_room(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("carroom", "the Simulation constructor", img, CRG_CTOR, g_crg_ctor, sizeof g_crg_ctor) ||
        !code_is("carroom", "its empty car list", img, 0xf8b7cau, g_crg_init, sizeof g_crg_init) ||
        !code_is("carroom", "the car list getter", img, 0xf9ae00u, g_crg_getter, sizeof g_crg_getter) ||
        !code_is("carroom", "the list's reserve", img, CRG_RESERVE, g_crg_reserve, sizeof g_crg_reserve) ||
        !code_is("carroom", "the add-car push", img, 0xf8ea0au, g_crg_push, sizeof g_crg_push)) {
        return 0;
    }
    g_crg_reserve_fn = (crg_reserve_fn)(void *)(img + CRG_RESERVE);
    if (!plat_hook("carroom: Simulation constructor", img + CRG_CTOR, (void *)crg_detour, (void **)&g_crg_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: carroom: a new world's car list gets room for 256 cars at once, so adding a car never moves it under a walk (game+0xf8b770)\n");
    return 1;
}
