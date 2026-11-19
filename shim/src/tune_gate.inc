/* FH2 1.0.0.64, online: a mod's press of the pause menu's Tune Car tile opens the tuning screen only when the game's own tile could be pressed
 * (reported 2026-11-10: FH2 ANNA's Tune Car opened it during an online Road Trip, where the game locks the Cars tab, so a player could load a
 * higher class tune into a B class Road Trip).
 *
 * Where [C: decompile, A_PauseMenu.xaml in media\UI.zip; FH2 ANNA 1.1.1 source, src/pause.cpp]. PauseMenuTiled::OnLoaded (+dc6e30) registers
 * +dbfd60 as PART_Button_TuneCar's click handler. It checks nothing itself: it takes the garage's current car and opens
 * ui_screens.tune_menu_without_upgrades through +ed07b0 (page, car by value, 1, two std::function by value; the callee releases the car, vtable
 * +0x18, and destroys both functions, vtable +0x20). The game locks a tile by its TileState ("Enabled", "Disabled", "Locked", ...; set through
 * +1424840, read by PauseMenuTiled::helper_140f500), and PauseMenuTiled::helper_dc17d0 (+dc17d0: page, element name -> 1 when that element is
 * Enabled) is the test other tiles' handlers make themselves. The Tune Car tile sits in PART_GROUP_Autoshow (the Cars tab, with Buy Car, My Cars
 * and Featured Cars), which the pause menu's mode setup (helper_ddac80, +ddac80) sets Disabled in its event modes. FH2 ANNA calls +dbfd60 with
 * the page from the page's own update, so neither state comes into it.
 * This points the call at +dbfdd6 (the one that opens the screen) here. The handler's first bytes stay as they are: ANNA compares them before it
 * hooks anything and turns its pause features off when they differ. A thunk notes who called the handler (its return address, 0x90 above the
 * call's: push rbx, sub rsp 0x80, call); when that is outside the game image and the game is in a network session (picker_pi.inc
 * ppi_in_session), the screen opens only if both PART_Button_TuneCar and PART_GROUP_Autoshow are Enabled. Otherwise the call's by-value arguments
 * are released as the callee would and nothing opens (the pause menu ANNA opened stays, Cars tab locked). The game's own presses and every call
 * outside a session pass unchanged. FH2MP_NO_TUNEGATE=1 turns it off.
 * Evidence: the handler, the arguments, the lock states and the group are confirmed from the decompile, the exe bytes and the XAML; that the
 * Cars tab is Disabled during an online Road Trip is inferred from helper_ddac80 and the report, NOT seen in a running game, so the decisions are
 * logged with both states. */
#include "tune_gate_rule.inc"

#define TG_HANDLER_RVA 0xdbfd60u
#define TG_OPEN_CALL_RVA 0xdbfdd6u
#define TG_ENABLED_RVA 0xdc17d0u

typedef unsigned char (*tg_open_fn)(void *page, void **car, int flag, unsigned char *fn4, unsigned char *fn5);
typedef char (*tg_enabled_fn)(void *page, const char *name);
static tg_open_fn g_tg_orig __attribute__((used));
static const unsigned char *g_tg_img;
static SIZE_T g_tg_img_size;
static const void *volatile g_tg_caller __attribute__((used));   /* who called the handler this time: the thunk's note, UI thread only */
static volatile LONG g_tg_refused, g_tg_allowed;

/* What +ed07b0 does with its by-value arguments before it returns. */
static void tg_release(void **car, unsigned char *fn4, unsigned char *fn5)
{
    unsigned char *fns[2] = {fn4, fn5};
    unsigned i;
    if (*car) {
        ((void (*)(void *))(*(void ***)*car)[3])(*car);
    }
    for (i = 0; i < 2; i++) {
        void *impl = *(void **)(fns[i] + 0x18);
        if (impl) {
            ((void (*)(void *, unsigned char))(*(void ***)impl)[4])(impl, impl != (void *)fns[i]);
            *(void **)(fns[i] + 0x18) = NULL;
        }
    }
}

static void tg_log(const char *what, int tile, int tab, volatile LONG *counter)
{
    LONG n = InterlockedIncrement(counter);
    if (n > 10 && n % 50) {
        return;
    }
    log_begin();
    log_raw("tunegate: ");
    log_raw(what);
    log_raw(" (Tune Car tile ");
    log_raw(tile ? "Enabled" : "not Enabled");
    log_raw(", Cars tab ");
    log_raw(tab ? "Enabled" : "not Enabled");
    log_raw(") (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

static unsigned char tg_open(void *page, void **car, int flag, unsigned char *fn4, unsigned char *fn5) __attribute__((used, noinline, noclone));
static unsigned char tg_open(void *page, void **car, int flag, unsigned char *fn4, unsigned char *fn5)
{
    const unsigned char *from = (const unsigned char *)g_tg_caller;
    g_tg_caller = NULL;
    if (page && tg_mod_press((ULONG_PTR)from, (ULONG_PTR)g_tg_img, g_tg_img_size) && ppi_in_session(g_tg_img)) {
        tg_enabled_fn enabled = (tg_enabled_fn)(g_tg_img + TG_ENABLED_RVA);
        int tile = enabled(page, "PART_Button_TuneCar") != 0, tab = enabled(page, "PART_GROUP_Autoshow") != 0;
        if (tg_refuses(tile, tab)) {
            tg_log("a mod's Tune Car press refused online: the game's own tile cannot be pressed now", tile, tab, &g_tg_refused);
            tg_release(car, fn4, fn5);
            return 0;
        }
        tg_log("a mod's Tune Car press opens the tuning screen online, as the game's tile would", tile, tab, &g_tg_allowed);
    }
    return g_tg_orig(page, car, flag, fn4, fn5);
}

/* The call at +dbfdd6 comes here: note the handler's caller (its return address, 0x90 above this call's), then go on into tg_open with the
 * registers and the stack as they came. */
__asm__(".text\n"
        ".p2align 4\n"
        "tg_open_thunk:\n"
        "movq 0x90(%rsp), %rax\n"
        "movq %rax, g_tg_caller(%rip)\n"
        "jmp tg_open\n");
extern unsigned char tg_open_thunk(void *page, void **car, int flag, unsigned char *fn4, unsigned char *fn5);

static int patch_tune_gate(void)
{
    /* mov r11,rsp; push rbx; sub rsp,0x80: the frame the thunk counts on (the first six are the bytes ANNA checks) */
    static const unsigned char prologue[11] = {0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00};
    /* mov rdx,rax; mov rcx,rbx; call +ed07b0 */
    static const unsigned char open_call[11] = {0x48, 0x8b, 0xd0, 0x48, 0x8b, 0xcb, 0xe8, 0xd5, 0x09, 0x11, 0x00};
    static const unsigned char enabled_head[6] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30};
    unsigned char *img = find_fh2_image();
    const IMAGE_NT_HEADERS64 *nt;

    if (!img || !code_is("tunegate", "Tune Car tile handler", img, TG_HANDLER_RVA, prologue, sizeof prologue) ||
        !code_is("tunegate", "its call that opens the tuning screen", img, TG_OPEN_CALL_RVA - 6, open_call, sizeof open_call) ||
        !code_is("tunegate", "PauseMenuTiled's tile state test", img, TG_ENABLED_RVA, enabled_head, sizeof enabled_head)) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS64 *)(img + ((const IMAGE_DOS_HEADER *)img)->e_lfanew);
    g_tg_img_size = nt->OptionalHeader.SizeOfImage;
    g_tg_img = img;
    if (!plat_hook_call("tunegate: Tune Car opens the tuning screen", img + TG_OPEN_CALL_RVA, (void *)tg_open_thunk, (void **)&g_tg_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: tunegate: online, a mod's Tune Car press opens the tuning screen only when the game's Tune Car tile and Cars tab are Enabled "
            "(game+0xdbfdd6)\n");
    return 1;
}
