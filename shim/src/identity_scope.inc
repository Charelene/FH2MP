/* Azimuth runs several Forza games from one launcher window, all in the azimuth.exe process: the Mod Platform loads FH2MP when azimuth.exe
 * starts, before the player picks a game, and title_scope.inc can only go by the game data\azimuth.ini (or --game=) names at that moment. A
 * player whose last game was FH2 and who then picks Forza Horizon 3 or Forza Motorsport 5 in Azimuth's window used to get FH2MP's account id
 * in that game too: Azimuth's fixed XboxUserId (0x0009000000000000, a movabs r8 immediate) was patched at load, and Azimuth names each player's
 * save folder after it, so data\titles\<FH3 or FM5>\saves\player-1\User_<FH2MP id> appeared and that game opened an empty profile (reported
 * 2026-11-10).
 *
 * azimuth.exe 1.0.4 reads the immediate in exactly two functions [C: the bytes, every 8-byte copy of the constant in the file checked]:
 * 0x140042150, the XboxUserId getter (a vtable slot, 0x1406c0bc0: this, HSTRING* -> HRESULT), and 0x1404da770, which renames an earlier
 * Azimuth's save for this id (reached from the local save space, whose entry points are the vtable slots at 0x14070dcf0..0x14070dd00). Both
 * run only when the started game asks for its user or its saves, so the game is mapped by then.
 * So the two loads now read a slot (mov r8,[rip+slot]), and each of the two functions first settles what the slot holds, once: the account id
 * when FH2 1.0.0.64 is the game Azimuth started, Azimuth's own id when another game is. Every title's exe is mapped at 0x140000000 (FH2 Mod
 * Platform src/titles.cpp); the platform's game_base is NULL for any game but FH2 1.0.0.64. If they are asked before any game is mapped (not
 * seen), that request gets Azimuth's id and the next one decides.
 *
 * This file is the part without Windows calls: the decision (id_scope_settle) and the replacement instruction (id_scope_load). */
#ifndef ID_SCOPE_YIELD
#define ID_SCOPE_YIELD() ((void)0)
#endif

enum { ID_OPEN, ID_DECIDING, ID_DECIDED };

struct id_scope {
    int state;                          /* ID_*; changed only with __atomic */
    unsigned long long *slot;           /* what the patched loads read */
    unsigned long long account, azimuth; /* FH2MP's account id, Azimuth's own fixed id */
};

/* started(ctx): 1 = FH2 1.0.0.64 is the game Azimuth started, 0 = another game is, -1 = no game is mapped yet.
 * Sets the slot the first time a game is known and never again. -> 1 when this call looked (*game says what it found), 0 when it was
 * already settled or another thread was settling it (this call waited for that). */
static int id_scope_settle(struct id_scope *s, int (*started)(void *), void *ctx, int *game)
{
    int open = ID_OPEN;

    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == ID_DECIDED) {
        return 0;
    }
    if (!__atomic_compare_exchange_n(&s->state, &open, ID_DECIDING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == ID_DECIDING) {
            ID_SCOPE_YIELD();
        }
        return 0;
    }
    *game = started(ctx);
    __atomic_store_n(s->slot, *game > 0 ? s->account : s->azimuth, __ATOMIC_RELEASE);
    __atomic_store_n(&s->state, *game < 0 ? ID_OPEN : ID_DECIDED, __ATOMIC_RELEASE);
    return 1;
}

/* The ten bytes that replace `movabs r8, imm64` at `site`: mov r8, qword [rip+disp32] reading `slot`, then a three-byte nop.
 * -> 0 when the slot is out of rip-relative reach. */
static int id_scope_load(unsigned char out[10], unsigned long long site, unsigned long long slot)
{
    long long disp = (long long)(slot - (site + 7));
    unsigned d, i;

    if (disp < -0x80000000LL || disp > 0x7FFFFFFFLL) {
        return 0;
    }
    d = (unsigned)disp;
    out[0] = 0x4C;          /* REX.W + REX.R: r8 */
    out[1] = 0x8B;          /* mov r64, r/m64 */
    out[2] = 0x05;          /* [rip + disp32] */
    for (i = 0; i < 4; i++) {
        out[3 + i] = (unsigned char)(d >> (8 * i));
    }
    out[7] = 0x0F;          /* nop dword [rax] */
    out[8] = 0x1F;
    out[9] = 0x00;
    return 1;
}
