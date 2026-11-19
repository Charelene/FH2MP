/* The drivetrain converter guard ("exception 0xc0000005 at game+0xb99cd3 reading 0x1", issue #412, 12+ reports).
 * An AVUI binding refresh (the InheritablePropertyChange walker -> BindingExpression) converts a car card's car value through the
 * DriveTrainConverter vfunc game+0xb99be0(this, out, value, target type, parameter). For a boxed int (or int64) it takes the low 32 bits of
 * [value+0x28] as a car ordinal and, for each of its three target types, looks the car up and reads through the result:
 *     140b99cb9  mov  rcx,[rip+0x2911da0]   ; the service provider (game+0x34aba60)
 *     140b99cc4  call 0x14021dec0           ; its car table (+0x3a0)
 *     140b99cce  call 0x14026f900           ; table entry of the ordinal, NULL past the end
 *     140b99cd3  movsx r8d,byte [rax+1]     <- rax = 0
 * (the other two branches, +0xb99d2c and +0xb99d8d, read [rax+0x30..0x48] the same way). In both dumps of #412 the value was
 * 0x7FFFFFFFFFFFFFFF (a car card with no car) and the lookup of 0xFFFFFFFF returned NULL [confirmed: dumps 5a11efa21fa89348,
 * 1e01bcdf2442954f, disassembly]. When no branch fits, the converter's answer is the unset value of game+0x160c340 with a reference taken
 * (+0xb99e4a); that is also what it answers for a value that is not an int.
 * The guard hooks the converter: when the value is an int whose car is not in the table it gives that same unset answer and does not call
 * the game's code; every other value goes to the game's code unchanged. FH2MP_NO_DTCONVGUARD=1 switches it off. */
#include "dtconv_rule.inc"

#define DTC_FN 0xb99be0u
typedef void **(*dtc_fn)(void *, void **, void *, void *, void *);
static dtc_fn g_dtc_orig;
static dtc_game g_dtc_game;
static void *(*g_dtc_unset)(void);
static volatile LONG g_dtc_hits;

/* mov rax,rsp ; push rdi ; push r14 ; push r15 ; sub rsp,0x60 ; mov qword [rax-0x28],-2 */
static const unsigned char g_dtc_head[16] = {0x48, 0x8b, 0xc4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x40, 0xd8};
/* +0xb99cb9: mov rcx,[rip+0x2911da0] ; test eax,eax ; jne +0x5e ; call 21dec0 ; mov rcx,rax ; mov edx,ebx ; call 26f900 ;
 * movsx r8d,byte [rax+1] */
static const unsigned char g_dtc_site[31] = {0x48, 0x8b, 0x0d, 0xa0, 0x1d, 0x91, 0x02, 0x85, 0xc0, 0x75, 0x5e, 0xe8, 0xf7, 0x41, 0x68, 0xff,
                                             0x48, 0x8b, 0xc8, 0x8b, 0xd3, 0xe8, 0x2d, 0x5c, 0x6d, 0xff, 0x44, 0x0f, 0xbe, 0x40, 0x01};
/* +0xb99e4a: call 0x14160c340 ; test rax,rax ; mov [rsi],rax ; je ; lock inc dword [rax+0x10] */
static const unsigned char g_dtc_unset_site[16] = {0xe8, 0xf1, 0x24, 0xa7, 0x00, 0x48, 0x85, 0xc0, 0x48, 0x89, 0x06, 0x74, 0x04, 0xf0, 0xff, 0x40};
/* game+0x26f900: mov rax,[rcx+0x70] ; mov r8d,edx ; sub rax,[rcx+0x68] ; sar rax,3 ; cmp r8,rax */
static const unsigned char g_dtc_car[18] = {0x48, 0x8b, 0x41, 0x70, 0x44, 0x8b, 0xc2, 0x48, 0x2b, 0x41, 0x68, 0x48, 0xc1, 0xf8, 0x03, 0x4c, 0x3b, 0xc0};
/* game+0x160c340: lea rax,[rip+0x20ae1b1] ; ret */
static const unsigned char g_dtc_unset_fn[8] = {0x48, 0x8d, 0x05, 0xb1, 0xe1, 0x0a, 0x02, 0xc3};

static void **dtc_detour(void *self, void **out, void *value, void *target, void *parameter)
{
    void *unset;
    LONG n;
    if (!dtc_car_missing(&g_dtc_game, value)) {
        return g_dtc_orig(self, out, value, target, parameter);
    }
    unset = g_dtc_unset();
    *out = unset;
    if (unset) {
        InterlockedIncrement((volatile LONG *)((char *)unset + 0x10));
    }
    n = InterlockedIncrement(&g_dtc_hits);
    if (n <= 5 || n % 50 == 0) {
        log_begin();
        log_raw("dtconv: a car binding names car ordinal ");
        log_dec((unsigned long)*(const unsigned *)((const char *)value + 0x28));
        log_raw(", which is not in the car table; the drivetrain converter answers no value instead of reading NULL at game+0xb99cd3 (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
    return out;
}

static int patch_dtconv_guard(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("dtconv", "the drivetrain converter", img, DTC_FN, g_dtc_head, sizeof g_dtc_head) ||
        !code_is("dtconv", "the converter's car lookup", img, 0xb99cb9u, g_dtc_site, sizeof g_dtc_site) ||
        !code_is("dtconv", "the converter's unset answer", img, 0xb99e4au, g_dtc_unset_site, sizeof g_dtc_unset_site) ||
        !code_is("dtconv", "the car table lookup", img, 0x26f900u, g_dtc_car, sizeof g_dtc_car) ||
        !code_is("dtconv", "the unset value", img, 0x160c340u, g_dtc_unset_fn, sizeof g_dtc_unset_fn)) {
        return 0;
    }
    g_dtc_game.int_type = (void *(*)(void *))(img + 0x15c8b20u);
    g_dtc_game.int64_type = (void *(*)(void *))(img + 0x15c94b0u);
    g_dtc_game.is_a = (unsigned char (*)(void *, void *))(img + 0x14530e0u);
    g_dtc_game.car = (void *(*)(void *, unsigned))(img + 0x26f900u);
    g_dtc_game.service = (void *const *)(img + 0x34aba60u);   /* rip of +0xb99cc0 + 0x2911da0, checked in g_dtc_site */
    g_dtc_unset = (void *(*)(void))(img + 0x160c340u);
    if (!plat_hook("dtconv: drivetrain converter", img + DTC_FN, (void *)dtc_detour, (void **)&g_dtc_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: dtconv: a car binding whose car is not in the car table gets no value from the drivetrain converter (game+0xb99be0)\n");
    return 1;
}
