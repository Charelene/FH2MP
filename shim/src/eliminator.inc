/* FH2MP Eliminator — client groundwork (DRAFT). See docs/eliminator/design.md.
 *
 * This file is the client half of the Eliminator battle-royale mode and is a *scaffold*: it wires up
 * the three pillars the mode needs (custom UI, camera movements, car drops) with the parts that are
 * already known to work, and leaves clearly-marked TODO-RE stubs for the two pieces that are net-new
 * reverse engineering (driving the camera, spawning/collecting car drops). It follows the house
 * pattern of the other synced features (honk_sync.inc / carsel_sync.inc / mods_sync.inc).
 *
 *   - Custom UI      : the platform's HUD layer (FH2HudInterface) for the chips, panel and tip card,
 *                      the pad layer (FH2PadInterface) for button chords (join, challenge, accept,
 *                      decline), and a menu page (FH2Page) to join/leave. These are real, shipped
 *                      platform 2.9.0 preview APIs, obtained here via get_interface. The HUD and
 *                      the chords are on only while the server answers, which it does only in a
 *                      world that hosts the mode.
 *   - Camera movements: nothing yet. The known anchor (game+0x1031e40, CCamAnimPlayer) is the
 *                      function camanim_guard.inc hooks; see the camera section below.
 *   - Car drops       : holds the server's drop list (el_response). Detecting a pickup (the local
 *                      player's position, TODO-RE address) and spawning the pickup car in the world
 *                      are TODO-RE; EL_CMD_DROP_CLAIM already round-trips.
 *
 * Two entry points, matching the shim's init split:
 *   - eliminator_ui_init()   : called from fh2mod_init (the HUD/pad/page must be registered there).
 *   - patch_eliminator()     : called from late_init (starts the worker; no game hooks yet).
 * FH2MP_NO_ELIMINATOR=1 switches both off.
 */
#include "fh2mod_hud.h"
#include "fh2mod_pad.h"
#include "eliminator_wire.h"

/* The button pictures of FH2HudPrompt.glyph are named in fh2mod.h from platform 2.9.0 on. While the
 * vendored fh2mod.h is older, these are the same numbers; the HUD layer itself only exists on a
 * 2.9.0 platform, so on an older one nothing here is shown. */
#ifndef FH2_GLYPH_Y
#define FH2_GLYPH_A 1
#define FH2_GLYPH_X 3
#define FH2_GLYPH_Y 4
#endif

/* Off switch, like every other feature: FH2MP_NO_ELIMINATOR=1. */
#define EL_ENV L"FH2MP_NO_ELIMINATOR"

/* Page and layer keys (unique within the FH2MP mod). */
#define EL_PAGE_KEY "eliminator"
#define EL_PAD_KEY  "Eliminator"

/* Pad chords: hold LB and press a face button. */
#define EL_MODIFIER FH2_PAD_LB
#define EL_BTN_CHALLENGE FH2_PAD_Y
#define EL_BTN_ACCEPT    FH2_PAD_A
#define EL_BTN_DECLINE   FH2_PAD_B
#define EL_BTN_JOIN      FH2_PAD_X

/* ---- state ---------------------------------------------------------------------------------- */

static const FH2HudInterface *g_el_hud;        /* NULL: the platform offers no HUD layer */
static const FH2PadInterface *g_el_pad;        /* NULL: the platform offers no pad layer */
static SRWLOCK g_el_lock = SRWLOCK_INIT;       /* guards g_el_* shared between the worker and the game thread */
static volatile LONG g_el_enabled;             /* 1 once patch_eliminator ran (online only) */
static unsigned g_el_parts;                    /* the HUD parts shown now; the worker's only */

/* The server answers only in a world that hosts the mode. While its replies come, the HUD and the pad
 * chords are on and the worker polls every EL_POLL_MS; without one for EL_LIVE_MS they go off and it
 * polls every EL_IDLE_MS, so a player anywhere else sees and pays next to nothing. */
#define EL_POLL_MS 250u
#define EL_IDLE_MS 2000u
#define EL_LIVE_MS 3000u

/* The latest reply, mirrored from the server. */
static struct {
    unsigned char phase, players, area, flags, tier, challenger;
    unsigned count;
    el_drop drops[EL_DROP_MAX];
    ULONGLONG response_at;
} g_el;

/* A command the player asked for (pad/page/game hook) that the worker sends on its next poll. */
static struct { unsigned char cmd; unsigned arg0, arg1; volatile LONG pending; } g_el_cmd;

/* ---- a command, requested from anywhere (pad/page/game thread) ------------------------------ */

static void eliminator_command(unsigned char cmd, unsigned arg0, unsigned arg1)
{
    AcquireSRWLockExclusive(&g_el_lock);
    g_el_cmd.cmd = cmd; g_el_cmd.arg0 = arg0; g_el_cmd.arg1 = arg1;
    g_el_cmd.pending = 1;
    ReleaseSRWLockExclusive(&g_el_lock);
}

/* ---- custom UI: the HUD layer --------------------------------------------------------------- */

static const char *const g_el_tiers[] = {"D", "C", "B", "A", "S1", "S2"};

/* v (0..999) as decimal text. */
static const char *el_dec(char out[4], unsigned v)
{
    char *p = out;
    if (v >= 100) *p++ = (char)('0' + v / 100 % 10);
    if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    *p = 0;
    return out;
}

/* Builds the HUD from the latest reply and shows it. The worker's only; the platform copies the
 * texts and drops a state that changes nothing. */
static void eliminator_hud_update(void)
{
    FH2HudState st;
    char players[4], area[4];
    unsigned char phase, flags, tier, challenger;
    unsigned parts = FH2_HUD_PART_CHIPS | FH2_HUD_PART_PANEL;

    if (!g_el_hud) return;
    memset(&st, 0, sizeof st);                 /* every reserved field must be zero */
    st.size = sizeof st;

    AcquireSRWLockShared(&g_el_lock);
    phase = g_el.phase; flags = g_el.flags; tier = g_el.tier; challenger = g_el.challenger;
    el_dec(players, g_el.players); el_dec(area, g_el.area);
    ReleaseSRWLockShared(&g_el_lock);

    st.corner.text = (flags & EL_FLAG_ELIMINATED) ? "ELIMINATED" : "ELIMINATOR";
    st.rows[0].label = "Car class";    st.rows[0].value = tier < 6 ? g_el_tiers[tier] : "-";
    st.rows[1].label = "Players left"; st.rows[1].value = players;
    st.rows[2].label = "Area shrunk";  st.rows[2].value = area;      /* the shrink level, 0 = the whole area */
    st.rows[3].label = "Phase";
    switch (phase) {
    case EL_PHASE_WAITING:     st.rows[3].value = "Waiting";      break;
    case EL_PHASE_DROPPING:    st.rows[3].value = "Drop";         break;
    case EL_PHASE_ACTIVE:      st.rows[3].value = "Active";       break;
    case EL_PHASE_HEADTOHEAD:  st.rows[3].value = "Head-to-head"; break;
    case EL_PHASE_FINISHED:    st.rows[3].value = "Finished";     break;
    default:                   st.rows[3].value = "-";            break;
    }
    st.row_count = 4; st.row_selected = 4;     /* 4 = no row filled in orange */

    /* The tip card says what the pad chords do now (the bar, where prompts live, is not one of this
     * layer's parts). Text, one button, text. */
    if (!(flags & EL_FLAG_JOINED)) {
        if (phase == EL_PHASE_WAITING) {
            st.tip_before = "Hold LB and press"; st.tip_button.glyph = FH2_GLYPH_X; st.tip_after = "to join";
        }
    } else if (!(flags & EL_FLAG_ELIMINATED) && phase == EL_PHASE_ACTIVE) {
        if (challenger) {
            st.tip_before = "Challenged! Hold LB and press"; st.tip_button.glyph = FH2_GLYPH_A; st.tip_after = "to race, B to decline";
        } else {
            st.tip_before = "Hold LB and press"; st.tip_button.glyph = FH2_GLYPH_Y; st.tip_after = "to challenge the nearest player";
        }
    }
    if (st.tip_before) parts |= FH2_HUD_PART_TIP;

    if (g_el_parts & ~parts) g_el_hud->show_hud_layer(g_mod, parts);   /* a part going out: before the state (fh2mod_hud.h) */
    g_el_hud->set_hud_state(g_mod, &st);
    g_el_hud->show_hud_layer(g_mod, parts);
    g_el_parts = parts;
}

/* ---- custom UI: the pad layer --------------------------------------------------------------- */

static void eliminator_pad_press(void *user, const char *key, uint32_t button)
{
    (void)user; (void)key;
    if (button & EL_BTN_CHALLENGE) eliminator_command(EL_CMD_CHALLENGE, 0, 0);
    else if (button & EL_BTN_ACCEPT) eliminator_command(EL_CMD_ACCEPT, 0, 0);
    else if (button & EL_BTN_DECLINE) eliminator_command(EL_CMD_DECLINE, 0, 0);
    else if (button & EL_BTN_JOIN) eliminator_command(EL_CMD_JOIN, 0, 0);
}

/* ---- custom UI: the menu page (join / leave / status) ---------------------------------------- */

static void eliminator_page_select(void *user, const char *page_key, const char *row_key)
{
    (void)user; (void)page_key;
    if (!row_key) return;
    if (lstrcmpA(row_key, "join") == 0) eliminator_command(EL_CMD_JOIN, 0, 0);
    else if (lstrcmpA(row_key, "leave") == 0) eliminator_command(EL_CMD_LEAVE, 0, 0);
}

static int eliminator_page_button(void *user, const char *page_key, int button)
{
    (void)user; (void)page_key; (void)button;
    return FH2_PAGE_IGNORED;
}

/* Registers the HUD layer, the pad layer and the menu page. Called from fh2mod_init (all three
 * are "fh2mod_init only"). Nothing here fails the shim: each missing piece just logs and is off. */
static void eliminator_ui_init(void)
{
    FH2HudLayer layer;
    FH2PadLayer pad;
    FH2Page page;
    FH2PageRow rows[2];
    wchar_t env[8];

    if (GetEnvironmentVariableW(EL_ENV, env, 8) != 0) return;    /* the off switch covers the UI too */
    if (g_api->api_version >= 2) {
        g_el_hud = (const FH2HudInterface *)g_api->get_interface(FH2_HUD_INTERFACE, FH2_HUD_INTERFACE_VERSION);
        if (g_el_hud && g_el_hud->size < sizeof *g_el_hud) g_el_hud = NULL;
        g_el_pad = (const FH2PadInterface *)g_api->get_interface(FH2_PAD_INTERFACE, FH2_PAD_INTERFACE_VERSION);
        if (g_el_pad && g_el_pad->size < sizeof *g_el_pad) g_el_pad = NULL;
    }

    if (g_el_hud) {
        memset(&layer, 0, sizeof layer);
        layer.size = sizeof layer;
        layer.parts = FH2_HUD_PART_CHIPS | FH2_HUD_PART_PANEL | FH2_HUD_PART_COUNTDOWN | FH2_HUD_PART_TIP;
        layer.lease_ms = 5000;
        if (!g_el_hud->add_hud_layer(g_mod, &layer)) g_el_hud = NULL;
    }
    if (g_el_pad) {
        memset(&pad, 0, sizeof pad);
        pad.size = sizeof pad;
        pad.key = EL_PAD_KEY;
        pad.modifier = 0;    /* off: the worker sets EL_MODIFIER while the server's replies come, or LB + A/B/X/Y
                                would be taken from the game in every online session */
        pad.buttons = EL_BTN_CHALLENGE | EL_BTN_ACCEPT | EL_BTN_DECLINE | EL_BTN_JOIN;
        pad.on_press = eliminator_pad_press;
        if (!g_el_pad->add_layer(g_mod, &pad)) g_el_pad = NULL;
    }

    memset(&page, 0, sizeof page);
    page.size = sizeof page;
    page.key = EL_PAGE_KEY;
    page.title = "The Eliminator";
    page.label = "The Eliminator";
    page.description = "Last-car-standing battle royale in Online Free Roam (groundwork).";
    page.on_select = eliminator_page_select;
    page.on_button = eliminator_page_button;
    if (g_api->api_version >= 8) {
        /* A row on FH2MP's mod page. No pause-menu tile: there is one mod per tile, and the
         * Achievements tile belongs to the Achievements mod. */
        g_api->add_page(g_mod, &page);
        memset(rows, 0, sizeof rows);
        rows[0].size = sizeof rows[0]; rows[0].key = "join";  rows[0].label = "Join Eliminator";
        rows[1].size = sizeof rows[1]; rows[1].key = "leave"; rows[1].label = "Leave Eliminator";
        g_api->set_page_rows(g_mod, EL_PAGE_KEY, rows, 2);
    }
    if (!g_el_hud && !g_el_pad) {
        g_api->log(g_mod, "eliminator: no HUD/pad layer on this platform; the mode's custom UI is off");
    }
}

/* Replies started or stopped: the pad chords and the HUD follow. The worker's only (and
 * g_el.response_at is written by the worker alone). */
static void eliminator_live(int *live, ULONGLONG now)
{
    int now_live = g_el.response_at && now - g_el.response_at < EL_LIVE_MS;
    if (now_live == *live) return;
    *live = now_live;
    if (g_el_pad) g_el_pad->set_modifier(g_mod, EL_PAD_KEY, now_live ? EL_MODIFIER : 0);
    if (!now_live && g_el_hud) { g_el_hud->show_hud_layer(g_mod, 0); g_el_parts = 0; }
}

/* ---- the worker: polls the lighting ticket, sends the Eliminator request, drains replies ------
 * Same shape as the lighting/honk workers: own UDP socket, address/epoch/car copied from g_light
 * under g_light_lock, reopen on generation change, send with g_r_wsasendto. */
static DWORD WINAPI eliminator_worker(void *unused)
{
    HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
    SOCKET (WSAAPI *make_socket)(int, int, int);
    int (WSAAPI *option)(SOCKET, int, int, const char *, int);
    int (WSAAPI *nonblocking)(SOCKET, long, u_long *);
    int (WSAAPI *receive)(SOCKET, char *, int, int, struct sockaddr *, int *);
    int (WSAAPI *close_socket)(SOCKET);
    SOCKET s = INVALID_SOCKET;
    ULONGLONG seq = 0, received = 0, generation = 0;
    unsigned char packet[EL_LIMIT];
    int live = 0;                              /* the server's replies are coming: this world hosts the mode */

    (void)unused;
    if (!ws) return 0;
    make_socket = (void *)GetProcAddress(ws, "socket");
    option = (void *)GetProcAddress(ws, "setsockopt");
    nonblocking = (void *)GetProcAddress(ws, "ioctlsocket");
    receive = (void *)GetProcAddress(ws, "recvfrom");
    close_socket = (void *)GetProcAddress(ws, "closesocket");
    if (!make_socket || !option || !nonblocking || !receive || !close_socket) return 0;

    for (;;) {
        int length, n, drained = 0;
        ULONGLONG current_generation, now = GetTickCount64();
        unsigned char address[28], epoch[16], car = 0, phase, players, area, flags, tier, challenger;
        unsigned cmd, arg0, arg1, count;
        el_drop drops[EL_DROP_MAX];
        int want_cmd;

        if (!g_el_enabled) break;
        eliminator_live(&live, now);
        AcquireSRWLockShared(&g_light_lock);
        length = g_light.address_len;
        car = g_light.car;
        current_generation = g_light.generation;
        { unsigned i; for (i = 0; i < 28 && i < (unsigned)length; i++) address[i] = g_light.address[i]; }
        { unsigned i; for (i = 0; i < 16; i++) epoch[i] = g_light.epoch[i]; }
        ReleaseSRWLockShared(&g_light_lock);

        if (generation != current_generation) {
            if (s != INVALID_SOCKET) close_socket(s);
            s = make_socket(23 /*AF_INET6*/, 2 /*SOCK_DGRAM*/, 17 /*IPPROTO_UDP*/);
            { int one = 1; option(s, 41 /*IPPROTO_IPV6*/, 27 /*IPV6_V6ONLY*/, (const char *)&one, 4); }
            { u_long nb = 1; nonblocking(s, 0x8004667e /*FIONBIO*/, &nb); }
            received = 0;
            generation = current_generation;
        }
        if (!length || !g_ticket_ready || !g_r_wsasendto) { Sleep(100); continue; }

        /* drain replies (best effort; a few datagrams a second is plenty) */
        for (;;) {
            int from = sizeof(struct sockaddr_storage);
            struct sockaddr_storage src;
            n = receive(s, (char *)packet, sizeof packet, 0, (struct sockaddr *)&src, &from);
            if (n <= 0) break;
            drained++;
            if (el_response(packet, (unsigned)n, g_ticket_key, g_xuid, seq, received, epoch,
                            &phase, &players, &area, &flags, &tier, &challenger, drops, &count)) {
                received = seq;
                AcquireSRWLockExclusive(&g_el_lock);
                g_el.phase = phase; g_el.players = players; g_el.area = area; g_el.flags = flags;
                g_el.tier = tier; g_el.challenger = challenger;
                g_el.count = count;
                { unsigned i; for (i = 0; i < count && i < EL_DROP_MAX; i++) g_el.drops[i] = drops[i]; }
                g_el.response_at = now;
                ReleaseSRWLockExclusive(&g_el_lock);
                eliminator_hud_update();
            }
            if (drained >= 16) break;
        }
        eliminator_live(&live, now);

        /* pick up a pending command (pad/page/game hook) and send it */
        AcquireSRWLockExclusive(&g_el_lock);
        want_cmd = g_el_cmd.pending; cmd = g_el_cmd.cmd; arg0 = g_el_cmd.arg0; arg1 = g_el_cmd.arg1;
        g_el_cmd.pending = 0;
        ReleaseSRWLockExclusive(&g_el_lock);

        el_request(packet, g_ticket_key, g_xuid, ++seq, epoch, car, want_cmd ? (unsigned char)cmd : EL_CMD_READY, arg0, arg1);
        {
            WSABUF b; DWORD sent;
            b.buf = (char *)packet; b.len = EL_REQUEST;
            g_r_wsasendto(s, &b, 1, &sent, 0, (const struct sockaddr *)address, length, NULL, NULL);
        }
        Sleep(live ? EL_POLL_MS : EL_IDLE_MS);
    }
    if (s != INVALID_SOCKET) close_socket(s);
    return 0;
}

/* ---- camera movements (TODO-RE, no hook yet) ---------------------------------------------------
 * The known anchor is the camera-anim transform game+0x1031e40 (CCamAnimPlayer, mode camera+0x188,
 * space provider camera+8). camanim_guard.inc already hooks it for a crash fix, so there is no
 * second hook here: when there is a drop-in/spectate provider to write, it goes into that guard's
 * detour (cag_detour), one hook for both. TODO-RE: the camera manager, how CCamAnimPlayer instances
 * are created/positioned/released, and a spectate follow provider. */

/* ---- car drops: pickup detection (groundwork) -------------------------------------------------
 * TODO-RE: the local player's car + position address; the drop list already arrives in g_el. When
 * the position is known, poll it here (read_game_memory is thread-safe) and, on entering a drop's
 * radius, call eliminator_command(EL_CMD_DROP_CLAIM, drop->id, 0). Spawning the pickup in the world
 * is the other net-new piece (server + shim object path). */

/* ---- install --------------------------------------------------------------------------------- */

static int patch_eliminator(void)
{
    g_el_enabled = 1;
    {
        HANDLE t = CreateThread(NULL, 0, eliminator_worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    log_begin(); log_raw("eliminator: client scaffold on (HUD and pad chords only in a world that hosts the mode; no camera or car-drop code yet)\n");
    return 1;
}
