/* FH2 1.0.0.64: the Free Roam invite toast for a race event ('[UL1] GokuBlack HAS STARTED A CLASS S2 RACE EVENT / Press (X) to join') names the
 * event's track, in bold, at the start of its second line:
 *     [UL1] GokuBlack HAS STARTED A CLASS S2 RACE EVENT
 *     Castelletto Meadow Circuit | Press (X) to join
 * (user 2026-11-05; the first version put it in front of the gamertag, the second at the end of line 1: both "wrong place" in the game). The name is
 * carried in the pre-gamertag string (empty for races), and the toast's layout (UI.zip Controls/NotificationPopupControl.xaml, patched through the
 * platform's set_file, see ui_race_toast_layout in fh2mp_mod.c) hides that string on line 1 when IsRaceEvent and shows it in the gamertag's bold
 * style in front of line 2's 'Press (X) to join'.
 *
 * Where the toast comes from [C: decompile]: NetworkRaceStartingPromptComponent::OnShowPrompt (+8193d0) reads the event options of the lobby
 * (CLobby +0xb8, a GameOptionsSynced; its game-type byte at vslot 0xd0 is 6 Infected, 7 King, else a race) and builds three strings from the
 * string table 'Lobby': helper_80cbb0 (+80cbb0) the text before the gamertag (IDS_EventInvite_Title_PreGamertag_Race: empty), helper_80c510
 * (+80c510) the text after it (' HAS STARTED A CLASS {0} RACE EVENT'), and the popup's template (Controls/NotificationPopupControl.xaml, in UI.zip)
 * lays them out as TitlePreGamertag + UserProfile.FormattedGamertag + TitlePostGamertag, the middle one in the bold gamertag style, with
 * 'Press (X) to join' on the second line. The pre string is the one place that can hold a name without changing the layout.
 *
 * This hooks the call to helper_80cbb0 inside OnShowPrompt (+819767). It lets the game build its string, and for a race event (not 6 or 7) looks
 * up the track the options hold (TrackId, u32 at options+0x128 [C: the GameOptionsSynced constructor +17d4630 puts the SyncedVar at +0x110 and its
 * value at +0x128]) with the game's own query, the one TrackLeaderboard::vfunc_178 (+e877b0) uses for its title but on the column DisplayName:
 * SELECT DisplayName FROM Tracks WHERE Id=<n>. The answer is the game's localized string object (it resolves in the player's language, nothing is
 * hard-coded), and the pre string becomes the game's own format of '{0} | ' with it (+123b660, the helper helper_80c510 uses for ' ...CLASS {0}...').
 * The format helper copies the text at once, as it does for the game's own toast.
 *
 * The layout: UI.zip's NotificationPopupControl.xaml has the three FZTextBlocks (Pre, UserProfile.FormattedGamertag, Post) in a horizontal
 * StackPanel for line 1 and a second horizontal StackPanel ('Press', the activate button, 'to join') for line 2; the patched copy hides the first
 * block when IsRaceEvent (Visibility with A_FZBooleanToVisibilityConverter_Inverse, the converter the file already uses) and adds a block bound to
 * TitlePreGamertag in front of 'Press', in A_Style_FZTextBlock_Notification_Gamertag (the bold style), visible when IsRaceEvent. If the platform
 * does not take the file, g_rt_layout stays 0 and the hook leaves the game's text.
 *
 * Safe by default: every byte checked before the hook goes in; at a toast, anything unexpected (no manager, no options, no track id, no row, an
 * empty name, an object outside the readable memory) leaves the game's own string, with one log line saying which. FH2MP_NO_RACETOAST=1 turns it off.
 * Not verified in a running game: that TrackId is already in the options when the toast is built, that the query's string object answers vslot
 * 0x38 like the Lobby strings, and how the toast looks (width of the longest names). */
#define RT_SITE_RVA 0x819767u        /* call helper_80cbb0 in OnShowPrompt */
#define RT_PRE_RVA 0x80cbb0u
#define RT_MGR_SLOT_RVA 0x34d6348u   /* g_NetworkGameManager (CLobby *) */
#define RT_MGR_LOAD_RVA 0x81972fu    /* mov rcx,[rip+g_NetworkGameManager] in OnShowPrompt */
#define RT_OPTS_RVA 0xa2ae90u        /* FUN_140a2ae90: return [rcx+0xb8] */
#define RT_MKSTR_RVA 0x323310u       /* FUN_140323310: narrow text -> the game's string object (FUN_140323310(&out, "[Unknown]", -1)) */
#define RT_FORMAT_RVA 0x123b660u     /* FUN_14123b660: (&out, &format string, &argument) */
#define RT_DB_RVA 0xced720u          /* FUN_140ced720: &(g_ServiceProvider+0x40), the game database interface */
#define RT_OPTS_OFFSET 0xb8u
#define RT_TRACK_OFFSET 0x128u
#define RT_ROUTE_OFFSET 0x148u
#define RT_TRACK_MAX 100000u
#define RT_LOG_MAX 20

typedef void **(*rt_pre_fn)(void **, char, unsigned, char);
typedef void **(*rt_mkstr_fn)(void **, const char *, int);
typedef void **(*rt_format_fn)(void **, void **, void **);
typedef void **(*rt_dbslot_fn)(void);
typedef void (*rt_query_fn)(void *, void **, const char *);
typedef void **(*rt_column_fn)(void *, void **, void *);
typedef const unsigned short *(*rt_text_fn)(void *);
typedef void (*rt_ref_fn)(void *);

/* 1 once the layout file shows the pre string on line 2 (rt_layout_ok, set by fh2mp_mod.c when the platform took the patched XAML). Without
 * that layout the pre string would show in front of the gamertag, so the game's own string is kept. */
static volatile LONG g_rt_layout;
static rt_pre_fn g_rt_orig;
static rt_mkstr_fn g_rt_mkstr;
static rt_format_fn g_rt_format;
static rt_dbslot_fn g_rt_dbslot;
static unsigned char *g_rt_img;
static volatile LONG g_rt_logged;

/* The game's std::string (MSVC layout) for a column name that fits the small buffer. */
struct rt_stdstring {
    char buf[16];
    unsigned long long size, capacity;
};

static int rt_readable(const void *p, unsigned n)
{
    return p && (ULONG_PTR)p > 0x10000 && !IsBadReadPtr(p, n);
}

/* Virtual function `off` bytes into the vtable of a game object, or NULL when the object or its table is not readable memory. */
static void *rt_vslot(void *obj, unsigned off)
{
    void **vt;

    if (!rt_readable(obj, 8)) {
        return NULL;
    }
    vt = *(void ***)obj;
    return rt_readable((unsigned char *)vt + off, 8) ? *(void **)((unsigned char *)vt + off) : NULL;
}

static void rt_addref(void *obj)
{
    rt_ref_fn f = (rt_ref_fn)rt_vslot(obj, 0x10);

    if (f) {
        f(obj);
    }
}

static void rt_release(void *obj)
{
    rt_ref_fn f;

    if (obj && (f = (rt_ref_fn)rt_vslot(obj, 0x18)) != NULL) {
        f(obj);
    }
}

static void rt_log(unsigned track, unsigned route, const char *what)
{
    if (InterlockedIncrement(&g_rt_logged) > RT_LOG_MAX) {
        return;
    }
    log_begin();
    log_raw("racetoast: ");
    log_raw(what);
    log_raw(" (TrackId ");
    log_dec(track);
    log_raw(", RouteId ");
    log_dec(route);
    log_raw(")\n");
}

/* The game's string object for the track's DisplayName, one reference held by the caller; NULL with *why set when there is none. */
static void *rt_track_name(unsigned track, const char **why)
{
    char sql[64];
    struct rt_stdstring col;
    void **dbslot, *db, *rs = NULL, *tmp = NULL, *name = NULL, **got;
    rt_query_fn query;
    rt_column_fn column;
    rt_text_fn text;
    const unsigned short *t;

    wsprintfA(sql, "SELECT DisplayName FROM Tracks WHERE Id=%u", track);
    dbslot = g_rt_dbslot();
    db = rt_readable(dbslot, 8) ? *dbslot : NULL;
    query = (rt_query_fn)rt_vslot(db, 0x40);
    if (!query) {
        *why = "the game database interface is not there";
        return NULL;
    }
    query(db, &rs, sql);
    column = (rt_column_fn)rt_vslot(rs, 0x100);
    if (!column) {
        *why = "the Tracks query gave no result set";
        rt_release(rs);
        return NULL;
    }
    memset(&col, 0, sizeof col);
    lstrcpyA(col.buf, "DisplayName");
    col.size = 11;
    col.capacity = 15;
    got = column(rs, &tmp, &col);
    if (rt_readable(got, 8) && *got) {
        name = *got;
        rt_addref(name);
    }
    rt_release(tmp);
    rt_release(rs);
    if (!name) {
        *why = "the Tracks row has no DisplayName";
        return NULL;
    }
    text = (rt_text_fn)rt_vslot(name, 0x38);
    t = text ? text(name) : NULL;
    if (!rt_readable(t, 2) || !t[0]) {
        rt_release(name);
        *why = "the track's name is empty";
        return NULL;
    }
    return name;
}

/* Replaces the pre-gamertag string *out (the game's, one reference) with '<track name> - '. */
static void rt_apply(void **out)
{
    unsigned char *mgr, *opts;
    unsigned track, route;
    const char *why = "";
    void *name, *fmt = NULL, *made = NULL, *arg, *old;

    mgr = rt_readable(g_rt_img + RT_MGR_SLOT_RVA, 8) ? *(unsigned char **)(g_rt_img + RT_MGR_SLOT_RVA) : NULL;
    opts = rt_readable(mgr + RT_OPTS_OFFSET, 8) ? *(unsigned char **)(mgr + RT_OPTS_OFFSET) : NULL;
    if (!rt_readable(opts, RT_ROUTE_OFFSET + 4)) {
        rt_log(0, 0, mgr ? "toast left as it is, the lobby has no event options" : "toast left as it is, there is no network manager");
        return;
    }
    track = *(unsigned *)(opts + RT_TRACK_OFFSET);
    route = *(unsigned *)(opts + RT_ROUTE_OFFSET);
    if (!g_rt_layout) {
        rt_log(track, route, "toast left as it is, its layout was not changed (see the racetoast layout line)");
        return;
    }
    if (track == 0 || track > RT_TRACK_MAX) {
        rt_log(track, route, "toast left as it is, the event options hold no usable track id");
        return;
    }
    name = rt_track_name(track, &why);
    if (!name) {
        rt_log(track, route, why);
        return;
    }
    g_rt_mkstr(&fmt, "{0} | ", -1);    /* the layout shows this string at the start of line 2, in front of 'Press (X) to join' */
    arg = name;
    if (fmt) {
        g_rt_format(&made, &fmt, &arg);
    }
    if (made) {
        old = *out;
        *out = made;
        rt_release(old);
        rt_log(track, route, "the race toast names the track at the start of line 2");
    } else {
        rt_log(track, route, "toast left as it is, the game could not format the name");
    }
    rt_release(fmt);
    rt_release(name);
}

static void **race_toast_pre(void **out, char type, unsigned cls, char flag)
{
    void **r = g_rt_orig(out, type, cls, flag);

    if (type != 6 && type != 7 && out) {
        rt_apply(out);
    }
    return r;
}

static int patch_race_toast(void)
{
    static const unsigned char pre_fn[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0xd9, 0x0f, 0xb6, 0xca};
    static const unsigned char before[] = {0x45, 0x0f, 0xb6, 0xcc, 0x44, 0x8b, 0xc0, 0x40, 0x0f, 0xb6, 0xd6, 0x48, 0x8d, 0x4c, 0x24, 0x30};
    static const unsigned char mkstr[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48};
    static const unsigned char format[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
    static const unsigned char opts_fn[] = {0x48, 0x8b, 0x81, 0xb8, 0x00, 0x00, 0x00, 0xc3};
    static const unsigned char db_head[] = {0x48, 0x8b, 0x05};
    static const unsigned char db_tail[] = {0x48, 0x83, 0xc0, 0x40, 0xc3};
    static const unsigned char mgr_head[] = {0x48, 0x8b, 0x0d};
    unsigned char *img = find_fh2_image();
    int d32, ok;

    if (!img) {
        return 0;
    }
    mem_copy(&d32, img + RT_SITE_RVA + 1, 4);
    ok = code_is("racetoast", "the pre-gamertag string builder", img, RT_PRE_RVA, pre_fn, sizeof pre_fn) &&
         code_is("racetoast", "the call to it in OnShowPrompt (register setup)", img, RT_SITE_RVA - (unsigned)sizeof before, before, sizeof before) &&
         code_is("racetoast", "the game's string maker", img, RT_MKSTR_RVA, mkstr, sizeof mkstr) &&
         code_is("racetoast", "the game's string formatter", img, RT_FORMAT_RVA, format, sizeof format) &&
         code_is("racetoast", "the lobby options accessor", img, RT_OPTS_RVA, opts_fn, sizeof opts_fn) &&
         code_is("racetoast", "the database accessor (mov rax,[rip+x])", img, RT_DB_RVA, db_head, sizeof db_head) &&
         code_is("racetoast", "the database accessor (add rax,0x40 / ret)", img, RT_DB_RVA + 7, db_tail, sizeof db_tail) &&
         code_is("racetoast", "the network manager load in OnShowPrompt", img, RT_MGR_LOAD_RVA, mgr_head, sizeof mgr_head);
    if (ok) {
        int mgr_disp;

        mem_copy(&mgr_disp, img + RT_MGR_LOAD_RVA + 3, 4);
        if (img[RT_SITE_RVA] != 0xe8 || img + RT_SITE_RVA + 5 + d32 != img + RT_PRE_RVA ||
            (long long)RT_MGR_LOAD_RVA + 7 + mgr_disp != (long long)RT_MGR_SLOT_RVA) {
            log_begin();
            log_raw("racetoast: the call in OnShowPrompt (game+0x819767) is not a call to game+0x80cbb0, or the network manager is not at game+0x34d6348; not patched\n");
            ok = 0;
        }
    }
    if (!ok) {
        log_begin();
        log_raw("racetoast: not installed, the race toast keeps the game's text\n");
        return 0;
    }
    g_rt_img = img;
    g_rt_mkstr = (rt_mkstr_fn)(img + RT_MKSTR_RVA);
    g_rt_format = (rt_format_fn)(img + RT_FORMAT_RVA);
    g_rt_dbslot = (rt_dbslot_fn)(img + RT_DB_RVA);
    if (!plat_hook_call("racetoast: pre-gamertag text of the race toast", img + RT_SITE_RVA, (void *)race_toast_pre, (void **)&g_rt_orig)) {
        return 0;
    }
    log_begin();
    log_raw("racetoast: race event toasts name the event's track at the start of line 2 (hook at game+0x819767)\n");
    return 1;
}
