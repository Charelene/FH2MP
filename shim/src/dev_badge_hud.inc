/* FH2 1.0.0.64: the developer badge in the HUD's player list, Free Roam's Players panel (the page side is DEV_BADGE_HUD_* in dev_badge.inc).
 * That row's text is one string the game builds, so no rule on the page can find a name in it: the shim marks a developer's row instead.
 * The free roam row writer +785d30 (party_row in party_hud.inc calls it) makes the row's text with the gamertag formatter
 * +b2b520(out, name, role flags, mode, club tag, ...): name is the plain gamertag (UTF-16), role flags bit 0 and bit 1 add a club mod's or
 * leader's shield. Later it sets the row's TeamID (row+0x54, descriptor +75d880) to 0 with the game's change notice +74c6a0.
 * While party_row's call to the writer runs, the formatter's detour notes whether the name is a developer's and drops the shield (the badge
 * takes its place, as on the other pages); after the call the row's TeamID is set to DEV_BADGE_HUD_TEAM with the same notice. The template
 * never reads TeamID, and the writer puts it back to 0 when it next fills that row for someone else. Off with FH2MP_NO_DEVBADGE or
 * FH2MP_NO_PARTYHUD (party_row is the only way in). Not seen in a running game yet. */
typedef void *(*devhud_fmt_fn)(void *, const WCHAR *, unsigned char, int, void *, void *);
typedef void *(*devhud_desc_fn)(void);
typedef void (*devhud_notify_fn)(void *, void *);

static devhud_fmt_fn g_devhud_fmt_orig;
static devhud_desc_fn g_devhud_team_desc;
static devhud_notify_fn g_devhud_notify;
static volatile DWORD g_devhud_tid;    /* the thread inside party_row's call to the row writer, 0 when none */
static int g_devhud_hit;               /* that call formatted a developer's name */
static int g_devhud_on;

#define DEVHUD_NAME(name) name,
static int devhud_is_dev(const WCHAR *name)
{
    static const char *const names[] = {DEV_BADGE_NAMES(DEVHUD_NAME) NULL};
    unsigned i, j;

    for (i = 0; names[i]; i++) {
        for (j = 0; names[i][j] && name[j] == (WCHAR)(unsigned char)names[i][j]; j++) {
        }
        if (!names[i][j] && !name[j]) {
            return 1;
        }
    }
    return 0;
}

static void *devhud_fmt(void *out, const WCHAR *name, unsigned char flags, int mode, void *club, void *extra)
{
    if (g_devhud_tid && g_devhud_tid == GetCurrentThreadId() && name && devhud_is_dev(name)) {
        g_devhud_hit = 1;
        flags &= (unsigned char)~3u;
    }
    return g_devhud_fmt_orig(out, name, flags, mode, club, extra);
}

static void devhud_row_begin(void)
{
    if (g_devhud_on) {
        g_devhud_hit = 0;
        g_devhud_tid = GetCurrentThreadId();
    }
}

static void devhud_row_end(void **slot)
{
    unsigned char *row;
    int *team;

    if (!g_devhud_on) {
        return;
    }
    g_devhud_tid = 0;
    row = slot ? (unsigned char *)*slot : NULL;
    if (g_devhud_hit && row) {
        team = (int *)(row + 0x54);
        if (*team != DEV_BADGE_HUD_TEAM) {
            *team = DEV_BADGE_HUD_TEAM;
            g_devhud_notify(row, g_devhud_team_desc());
        }
    }
    g_devhud_hit = 0;
}

static int patch_dev_badge_hud(void)
{
    static const struct { unsigned rva; unsigned char bytes[16]; const char *what; } code[] = {
        {0xb2b520, {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48}, "gamertag formatter"},
        {0x75d880, {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48}, "TeamID descriptor"},
        {0x74c6a0, {0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48}, "change notice"},
        /* in the row writer: rbx = row; cmp dword [rbx+0x54], 0; je; mov [rbx+0x54], r12d; call the TeamID descriptor */
        {0x785f9b, {0x48, 0x8b, 0x1f, 0x83, 0x7b, 0x54, 0x00, 0x74, 0x14, 0x44, 0x89, 0x63, 0x54, 0xe8, 0xd3, 0x78}, "row writer's TeamID"},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;

    if (!img) {
        return 0;
    }
    for (i = 0; i < sizeof code / sizeof code[0]; ++i) {
        if (!code_is("devbadge", code[i].what, img, code[i].rva, code[i].bytes, sizeof code[i].bytes)) {
            return 0;
        }
    }
    g_devhud_team_desc = (devhud_desc_fn)(img + 0x75d880);
    g_devhud_notify = (devhud_notify_fn)(img + 0x74c6a0);
    if (!plat_hook("devbadge: gamertag formatter", img + 0xb2b520, (void *)devhud_fmt, (void **)&g_devhud_fmt_orig)) {
        return 0;
    }
    g_devhud_on = 1;
    log_begin();
    log_raw("devbadge: developers' rows in the HUD's player list are marked for the badge\n");
    return 1;
}
