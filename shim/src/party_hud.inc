/* FH2 1.0.0.64: the party list on the Free Roam HUD holds 12 members. HudPartyMembersComponent (constructor +7726c0) keeps its member rows as 12
 * MiniLeaderboardItem pointers at component+0x130, the vector that follows at +0x190 and the member count at +0x1ac are used by offset all over
 * the class, so the array cannot grow in place. Three places walk it by the count, `*(component+0x1ac)`, with no bound: the rebuild helper_78bc60
 * (the per-row write +785d30, and the call that hands the array to the mini leaderboard +abf2d0) and the per-frame helper_78bbd0 that
 * GameIndependentUpdate runs. A 13th member made all three read and write the vector's header as if it were a row, and every client in the lobby
 * crashed within a frame (game+0x144661d, issue #47; docs/re/second-pass.md).
 *
 * Rows 12 and up are rows of our own, made with the game's own factory (+7389d0), one table per component:
 *   - the call to +785d30 for row i >= 12 gets a pointer into our table instead of component+0x130+8*i (and no call at all past the maximum),
 *   - the call to +abf2d0 gets an array assembled from the 12 native rows and ours (the list collection is resized to the count it is given),
 *   - the per-frame helper sees the count held at 12 while it runs, so the extra rows are not refreshed every frame (they keep what the rebuild wrote).
 * Hooked at the two call sites, not at the functions, because +abf2d0 serves the other mini leaderboards too. With 12 members or fewer nothing is
 * changed. The maximum is 36 rows; env FH2MP_PARTY_MAX sets it (12 to 75), FH2MP_NO_PARTYHUD turns the patch off. Not verified in a running game: whether
 * the HUD template shows more than 12 rows, and what MemberDisplayCount (set from the member vectors, not from this count) does above 12. */
#define PARTY_NATIVE_ROWS 12
#define PARTY_ROWS_OFFSET 0x130
#define PARTY_COUNT_OFFSET 0x1ac
#define PARTY_EXTRA_MAX 63
#define PARTY_COMPONENTS 2

typedef void (*party_row_fn)(void *, void **, void *, char);
typedef void (*party_board_fn)(void *, void **, int);
typedef void (*party_frame_fn)(void *, void *, void *, void *);
typedef void **(*party_item_fn)(void **, void *, void *, void *);

static party_row_fn g_party_row_orig;
static party_board_fn g_party_board_orig;
static party_frame_fn g_party_frame_orig;
static party_item_fn g_party_new_item;
static int g_party_max = PARTY_NATIVE_ROWS + 24;
static struct { void *component; void *rows[PARTY_EXTRA_MAX]; } g_party_ext[PARTY_COMPONENTS];
static int g_party_next_table;
static volatile LONG g_party_logged;

/* The rows of one component; a new component takes a free table, or the oldest one (its rows are generic items, rebound at each rebuild). */
static void **party_extra_rows(void *component, int create)
{
    int i;

    for (i = 0; i < PARTY_COMPONENTS; i++) {
        if (g_party_ext[i].component == component) {
            return g_party_ext[i].rows;
        }
    }
    if (!create) {
        return NULL;
    }
    for (i = 0; i < PARTY_COMPONENTS && g_party_ext[i].component; i++) {
    }
    if (i == PARTY_COMPONENTS) {
        i = g_party_next_table++ % PARTY_COMPONENTS;
    }
    g_party_ext[i].component = component;
    return g_party_ext[i].rows;
}

static void party_row(void *component, void **slot, void *member, char flag)
{
    long long i = slot - (void **)((unsigned char *)component + PARTY_ROWS_OFFSET);
    void **rows, **row;

    if (i < PARTY_NATIVE_ROWS) {
        g_party_row_orig(component, slot, member, flag);
        return;
    }
    if (i >= g_party_max || !(rows = party_extra_rows(component, 1))) {
        return;
    }
    row = &rows[i - PARTY_NATIVE_ROWS];
    if (!*row) {
        g_party_new_item(row, NULL, NULL, NULL);
        if (*row && !InterlockedExchange(&g_party_logged, 1)) {
            log_begin();
            log_raw("partyhud: more than 12 members, rows from 13 on are made by FH2MP (up to ");
            log_dec((unsigned long)g_party_max);
            log_raw(")\n");
        }
    }
    if (*row) {
        g_party_row_orig(component, row, member, flag);
    }
}

static void party_board(void *mini, void **rows, int count)
{
    void *list[PARTY_NATIVE_ROWS + PARTY_EXTRA_MAX];
    void **extra;
    int i;

    if (count <= PARTY_NATIVE_ROWS) {
        g_party_board_orig(mini, rows, count);
        return;
    }
    extra = party_extra_rows((unsigned char *)rows - PARTY_ROWS_OFFSET, 0);
    if (count > g_party_max) {
        count = g_party_max;
    }
    if (!extra) {
        count = PARTY_NATIVE_ROWS;
    }
    for (i = 0; i < count; i++) {
        list[i] = i < PARTY_NATIVE_ROWS ? rows[i] : extra[i - PARTY_NATIVE_ROWS];
        if (i >= PARTY_NATIVE_ROWS && !list[i]) {      /* a row of ours that was never made: the list ends before it (the native rows are the game's to pass) */
            count = i;
            break;
        }
    }
    g_party_board_orig(mini, list, count);
}

static void party_frame(void *component, void *a, void *b, void *c)
{
    int *count = (int *)((unsigned char *)component + PARTY_COUNT_OFFSET);
    int members = *count;

    if (members <= PARTY_NATIVE_ROWS) {
        g_party_frame_orig(component, a, b, c);
        return;
    }
    *count = PARTY_NATIVE_ROWS;
    g_party_frame_orig(component, a, b, c);
    *count = members;
}

static void party_max_from_env(void)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA("FH2MP_PARTY_MAX", v, sizeof v);
    int total = 0, i;

    if (n == 0 || n >= sizeof v) {
        return;
    }
    for (i = 0; v[i]; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return;
        }
        total = total * 10 + (v[i] - '0');
    }
    if (total >= PARTY_NATIVE_ROWS && total <= PARTY_NATIVE_ROWS + PARTY_EXTRA_MAX) {
        g_party_max = total;
    }
}

static int patch_party_hud(void)
{
    static const struct { unsigned rva; unsigned char bytes[16]; const char *what; } natives[] = {
        {0x785d30, {0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44}, "per-row writer"},
        {0xabf2d0, {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b}, "mini leaderboard setter"},
        {0x7389d0, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48}, "row factory"},
        {0x78bbd0, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48}, "per-frame update"},
    };
    /* the two calls inside helper_78bc60 and the register setup in front of each: rcx = component, rdx = component+(i+0x26)*8, r8 = member;
     * r8d = count at +0x1ac, rcx = the mini leaderboard at +0x128, rdx = component+0x130 */
    static const struct { unsigned site, target; unsigned char before[14]; const char *what; } sites[] = {
        {0x78be88, 0x785d30, {0x48, 0x83, 0xc2, 0x26, 0x48, 0x8d, 0x14, 0xd7, 0x4c, 0x8b, 0x03, 0x48, 0x8b, 0xcf}, "per-row write"},
        {0x78beb0, 0xabf2d0, {0x44, 0x8b, 0x87, 0xac, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0x28, 0x01, 0x00, 0x00}, "mini leaderboard call"},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;
    int ok = 1, d32;

    if (!img) {
        return 0;
    }
    for (i = 0; i < sizeof natives / sizeof natives[0]; ++i) {
        if (!code_is("partyhud", natives[i].what, img, natives[i].rva, natives[i].bytes, sizeof natives[i].bytes)) {
            return 0;
        }
    }
    for (i = 0; i < sizeof sites / sizeof sites[0]; ++i) {
        mem_copy(&d32, img + sites[i].site + 1, 4);
        if (!code_is("partyhud", sites[i].what, img, sites[i].site - (unsigned)sizeof sites[i].before, sites[i].before, sizeof sites[i].before) ||
            img[sites[i].site] != 0xe8 || img + sites[i].site + 5 + d32 != img + sites[i].target) {
            log_begin();
            log_raw("partyhud: the ");
            log_raw(sites[i].what);
            log_raw(" call in the party HUD rebuild (game+0x78bc60) is not the one expected; not patched\n");
            return 0;
        }
    }
    party_max_from_env();
    g_party_new_item = (party_item_fn)(img + 0x7389d0);
    if (!plat_hook_call("partyhud: row writer", img + sites[0].site, (void *)party_row, (void **)&g_party_row_orig)) ok = 0;
    if (!plat_hook_call("partyhud: mini leaderboard rows", img + sites[1].site, (void *)party_board, (void **)&g_party_board_orig)) ok = 0;
    if (!plat_hook("partyhud: per-frame row update", img + 0x78bbd0, (void *)party_frame, (void **)&g_party_frame_orig)) ok = 0;
    log_begin();
    log_raw(ok ? "partyhud: party list guarded, up to " : "partyhud: incomplete, the hooks named above failed; up to ");
    log_dec((unsigned long)g_party_max);
    log_raw(" rows\n");
    return ok;
}
