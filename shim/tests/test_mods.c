/* Isolated tests of src/mods_wire.h, src/ug_status.h, src/ug_effects.h and src/mods_sync.inc (NitroSystem's online rule, Underglow's status through a
 * fake platform command pipe): no game is loaded, no network calls are made, NitroSystem is a recording fake. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "fh2mod.h"
#include "src/sha256.inc"
static unsigned char g_ticket_key[32];
static int g_ticket_ready;
static unsigned long long g_xuid;
#define FH2_SIZE_OF_IMAGE 0x3a18000u
static int (WINAPI *g_r_wsasendto)(UINT_PTR, void *, DWORD, DWORD *, DWORD, const void *, int, void *, void *);
static int read_game_memory(const void *p, void *out, SIZE_T n) { if (!p) return 0; memcpy(out, p, n); return 1; }
static void mem_copy(void *to, const void *from, SIZE_T n) { memcpy(to, from, n); }
static unsigned char *find_fh2_image(void) { return NULL; }
static int plat_hook(const char *name, void *target, void *detour, void **original) { (void)name; (void)target; (void)detour; (void)original; return 0; }
static char g_log[1 << 16];
static void log_begin(void) {}
/* the newest lines stay: a full log drops its older half (Wine writes more than a native run) */
static void log_raw(const char *s)
{
    size_t have = strlen(g_log), add = strlen(s);
    if (add >= sizeof g_log / 2) return;
    if (have + add >= sizeof g_log) {
        memmove(g_log, g_log + have - sizeof g_log / 4, sizeof g_log / 4 + 1);
        have = strlen(g_log);
    }
    memcpy(g_log + have, s, add + 1);
}
static void log_hex(unsigned long long value, int digits) { (void)value; (void)digits; }
static void log_hexbytes(const unsigned char *p, unsigned n) { (void)p; (void)n; }
static void log_dec(unsigned long value) { char b[24]; sprintf(b, "%lu", value); log_raw(b); }
#include "src/lighting_sync.inc"
/* the game state the rule reads (picker_pi.inc, autodrive_rule.inc in the shim) */
#define AD_FREE_ROAM 2
static int g_sess, g_ctx, g_rt;
static int ppi_in_session(const unsigned char *img) { (void)img; return g_sess; }
static int ad_context_type(const unsigned char *img) { (void)img; return g_ctx; }
static int ad_road_trip(const unsigned char *img) { (void)img; return g_rt; }
static const FH2ModApi *g_api;
static int g_ug_loaded;
#define UG_LOADED() g_ug_loaded
#include "src/mods_sync.inc"
#include "src/nos_flames.inc"
#include "src/ug_remote.inc"

/* ---- NitroSystem fake ---- */
static char g_who[64];
static int g_blocked = -1, g_block_calls, g_burning;
static void fake_set_blocked(const char *who, int blocked) { strcpy(g_who, who); g_blocked = blocked; g_block_calls++; }
static int fake_is_blocked(void) { return g_blocked == 1; }
static int fake_is_active(void) { return g_burning; }
static int fake_percent(void) { return 50; }
static const nos_control_v1 g_fake_nos = {sizeof(nos_control_v1), fake_set_blocked, fake_is_blocked, fake_is_active, fake_percent};
static int g_have_nos = 1;
static int fake_wait(uint32_t ms) { (void)ms; return 1; }
static const void *fake_iface(const char *name, uint32_t v) { return g_have_nos && !strcmp(name, "NitroSystem.Control") && v == 1 ? &g_fake_nos : NULL; }
static const char *fake_version(const char *name) { return !strcmp(name, "NitroSystem") ? "2.0.0" : !strcmp(name, "Underglow") ? "1.1.0" : NULL; }

/* ---- the platform's command pipe, answering one request with `g_answer` ---- */
static const char *g_answer;
static char g_asked[256];
static DWORD WINAPI pipe_server(void *unused)
{
    wchar_t name[48]; HANDLE p; char in[256]; DWORD n = 0, w;
    (void)unused;
    wsprintfW(name, L"\\\\.\\pipe\\fh2mod-%lu", GetCurrentProcessId());
    p = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 255, 0x4000, 0x1000, 0, NULL);
    assert(p != INVALID_HANDLE_VALUE);
    if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
        if (ReadFile(p, in, sizeof in - 1, &n, NULL)) { in[n] = 0; strcpy(g_asked, in); }
        WriteFile(p, g_answer, (DWORD)strlen(g_answer), &w, NULL);
        FlushFileBuffers(p);
        DisconnectNamedPipe(p);
    }
    CloseHandle(p);
    return 0;
}
static int ask_pipe(const char *answer, unsigned char ug[MODS_UG])
{
    HANDLE t; int r;
    g_answer = answer; g_asked[0] = 0;
    t = CreateThread(NULL, 0, pipe_server, NULL, 0, NULL);
    Sleep(100);
    r = ug_read(ug);
    WaitForSingleObject(t, 2000); CloseHandle(t);
    return r;
}

static const char STATUS[] =
    "neon on, primary color Cyan (0 1 1), secondary Pink, brightness 0.25, effects: front Rainbow Wave, right Comet, rear Police, left Static, at 0.3 a second; parts on: front right rear left\n"
    "shape: Side 10 Front 6 Rear 6 Height 0.01 FrontInset 0.5 RearInset 0.7 SideOffset -0.2 SideTrim 0.8 Range 1 Power 4 Inner 60 Outer 170\n"
    "car 2183: half width 0.95 (from its lights), front 2.10, rear -2.00 (from its lights)\n"
    "neon: 48 light(s), lit\n"
    "Underglow Kit on the driven car (garage id 32, car 2183): Front and Sides Lv2, required; colors and effects: this car's own (UnderglowGarage.json)\n"
    "radio: tapped; levels now: bass 0.10, low mids 0.20, high mids 0.30, treble 0.40\n";

static void status_variant(char *out, const char *from, const char *to)
{
    const char *at = strstr(STATUS, from);
    assert(at);
    memcpy(out, STATUS, (size_t)(at - STATUS)); out[at - STATUS] = 0;
    strcat(out, to); strcat(out, at + strlen(from));
}

static int hex_value(int c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; }
static unsigned read_hex(const char *path, unsigned char *out, unsigned max)
{
    FILE *f = fopen(path, "r"); char line[4096]; unsigned n = 0, i;
    assert(f && fgets(line, sizeof line, f)); fclose(f);
    for (i = 0; line[i] && line[i + 1] && line[i] != '\n' && n < max; i += 2) out[n++] = (unsigned char)(hex_value(line[i]) * 16 + hex_value(line[i + 1]));
    return n;
}
/* the server's vector underglow (native/internal/game/modsync_test.go modsVecUG) */
static void vec_ug(unsigned char ug[MODS_UG])
{
    static const unsigned char v[MODS_UG] = {1, 0x0b, 10, 6, 6, 2, 13, 7, 0, 0, 255, 255, 255, 25, 153, 16, 15, 0x52, 0x03, 0x40, 0x06, 0x88, 0xfa, 80, 1, 0, 0, 0};
    memcpy(ug, v, MODS_UG);
}

/* a failed assert says what the code logged last */
static void on_abort(int sig)
{
    size_t n = strlen(g_log);
    (void)sig;
    fprintf(stderr, "--- log tail ---\n%s\n", g_log + (n > 2000 ? n - 2000 : 0));
}

/* The rule runs at most every NOS_RULE_MS by GetTickCount64, which under a busy Wine can lag behind Sleep: wait until the tick count itself
 * has moved on, then tick. */
static void tick_later(void)
{
    ULONGLONG t0 = GetTickCount64();
    do Sleep(20); while (GetTickCount64() - t0 < NOS_RULE_MS + 10);
    nos_rule_tick();
}

int main(int argc, char **argv)
{
    unsigned char packet[MODS_LIMIT], epoch[16], ug[MODS_UG], server = 0;
    mods_record out[MODS_MAX];
    unsigned i, n, count = 0;
    const char *why;
    ug_customs cc;
    static FH2ModApi api;
    signal(SIGABRT, on_abort);
    for (i = 0; i < 32; i++) g_ticket_key[i] = (unsigned char)i;
    for (i = 0; i < 16; i++) epoch[i] = (unsigned char)(16 + i);
    g_xuid = 0x9000123456789ull;
    vec_ug(ug);
    assert(mods_ug_valid(ug));
    mods_request(packet, g_ticket_key, g_xuid, 9, epoch, 6, MODS_NOS_SWITCH | MODS_NOS_ALLOWED | MODS_NOS_BURNING, ug);
    if (argc > 1 && !strcmp(argv[1], "--vector")) { for (i = 0; i < MODS_REQUEST; i++) printf("%02x", packet[i]); puts(""); return 0; }
    if (argc > 2 && !strcmp(argv[1], "--reply")) {
        /* the server's reply to the request above (tests/vectors/mods_reply.hex): owner 10, car 5 burns NOS with the vector underglow, NOS ok here */
        unsigned char reply[MODS_LIMIT];
        n = read_hex(argv[2], reply, sizeof reply);
        assert(mods_response(reply, n, g_ticket_key, g_xuid, 9, 0, epoch, &server, out, &count));
        assert(server == MODS_SERVER_NOS_OK && count == 1 && out[0].owner == 10 && out[0].car == 5 && out[0].nos == (MODS_NOS_SWITCH | MODS_NOS_ALLOWED | MODS_NOS_BURNING));
        assert(!memcmp(out[0].ug, ug, MODS_UG));
        assert(!mods_response(reply, n, g_ticket_key, g_xuid, 9, 9, epoch, &server, out, &count));                 /* already received */
        assert(!mods_response(reply, n, g_ticket_key, g_xuid, 9 + MODS_SEQ_WINDOW + 1, 0, epoch, &server, out, &count));   /* too old */
        assert(!mods_response(reply, n, g_ticket_key, g_xuid + 1, 9, 0, epoch, &server, out, &count));
        epoch[0] ^= 1; assert(!mods_response(reply, n, g_ticket_key, g_xuid, 9, 0, epoch, &server, out, &count)); epoch[0] ^= 1;
        reply[n - 1] ^= 1; assert(!mods_response(reply, n, g_ticket_key, g_xuid, 9, 0, epoch, &server, out, &count)); reply[n - 1] ^= 1;
        reply[MODS_HEAD + 4 + 7] = MODS_UG_EFFECTS; hmac_sha256(g_ticket_key, 32, reply, n - 32, reply + n - 32);
        assert(!mods_response(reply, n, g_ticket_key, g_xuid, 9, 0, epoch, &server, out, &count));                 /* an effect past the list */
        puts("mods reply vector: ok");
        return 0;
    }

    /* the wire rules */
    {
        unsigned char bad[MODS_UG];
        memset(bad, 0, sizeof bad); assert(mods_ug_valid(bad));
        memcpy(bad, ug, MODS_UG); bad[2] = 30; bad[3] = 3; bad[4] = 2; assert(!mods_ug_valid(bad));
        memcpy(bad, ug, MODS_UG); bad[21] = 0x40; bad[22] = 0x06; bad[19] = 0x88; bad[20] = 0xfa; assert(!mods_ug_valid(bad));   /* front < rear */
        memcpy(bad, ug, MODS_UG); bad[27] = 1; assert(!mods_ug_valid(bad));
    }

    /* Underglow's status -> the wire state */
    memset(&cc, 0, sizeof cc);
    for (i = 0; i < UG_CUSTOMS; i++) ug_copy3(cc.rgb[i], ug_custom_defaults[i]);
    assert(ug_status_to_wire(STATUS, &cc, ug, &why) && !why);
    {
        static const unsigned char want[MODS_UG] = {1, 0x0b, 15, 9, 9, 2, 13, 7, 0, 0, 255, 255, 255, 26, 153, 16, 15, 0xee, 0x02, 0x40, 0x06, 0xec, 0xfa, 80, 1, 0, 0, 0};
        for (i = 0; i < MODS_UG; i++) if (ug[i] != want[i]) { printf("status byte %u: %u, want %u\n", i, ug[i], want[i]); return 1; }
        assert(mods_ug_valid(ug));
    }
    {
        char v[2048];
        status_variant(v, "neon: 48 light(s), lit", "neon: 48 light(s), dark");
        assert(ug_status_to_wire(v, &cc, ug, &why) && !ug[0] && !ug[2]);           /* dark: all zero */
        status_variant(v, "neon on,", "neon off,");
        assert(ug_status_to_wire(v, &cc, ug, &why) && !ug[0]);
        status_variant(v, "Front and Sides Lv2, required", "Stock, required");
        assert(ug_status_to_wire(v, &cc, ug, &why) && !ug[0]);                      /* a car on Stock has none */
        status_variant(v, "Front and Sides Lv2, required", "Front, Back and Sides Lv3, required");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[1] == 0x0f && ug[2] == 20 && ug[3] == 12 && ug[4] == 12);
        status_variant(v, "Front and Sides Lv2, required", "Back Lv1, not required (Require Underglow Kit is off): every part can light");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[1] == 0x0f && ug[2] == 10);   /* the kit decides nothing when it is not required */
        status_variant(v, "Underglow Kit on the driven car (garage id 32, car 2183): Front and Sides Lv2, required", "Underglow Kit: not in the garage (see platform.log)");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[1] == 0x0f && ug[2] == 10);
        status_variant(v, "parts on: front right rear left", "parts on: right left");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[1] == 0x0a);
        status_variant(v, "(from its lights)\nneon", "(from the game's data)\nneon");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[19] == 0x34 && ug[20] == 0x08 && ug[21] == 0x30 && ug[22] == 0xf8);   /* 2100, -2000: no insets */
        status_variant(v, "secondary Pink", "secondary Custom 2");
        assert(ug_status_to_wire(v, &cc, ug, &why) && ug[12] == 255 && ug[13] == 128 && ug[14] == 128);
        status_variant(v, "secondary Pink", "secondary Nobody");
        assert(!ug_status_to_wire(v, &cc, ug, &why) && why && !ug[0]);
        status_variant(v, "right Comet", "right Laser Show");
        assert(!ug_status_to_wire(v, &cc, ug, &why) && why);
        assert(!ug_status_to_wire("Underglow 2.0: on", &cc, ug, &why) && why);
        assert(!ug_status_to_wire(NULL, &cc, ug, &why) && why);
    }
    {
        float rgb[3];
        assert(ug_parse_color("#FF69B4", rgb) && rgb[0] == 1.0f && fabsf(rgb[1] - 105 / 255.0f) < 1e-6f);
        assert(ug_parse_color("255 105 180", rgb) && fabsf(rgb[2] - 180 / 255.0f) < 1e-6f);
        assert(ug_parse_color("0 1 0.5", rgb) && rgb[1] == 1.0f && rgb[2] == 0.5f);
        assert(!ug_parse_color("teal", rgb));
    }

    /* the effects: where the ring's lights sit, and a few colors (the rest match Underglow's own code; see the shim history for that check) */
    {
        ug_style s; ug_pixel px[UG_MAX_NEON]; ug_audio au; float rgb[3], level, p[3];
        unsigned char eff[4] = {UG_STATIC, UG_RAINBOW, UG_POLICE, UG_WAVE};
        memset(&au, 0, sizeof au);
        vec_ug(ug); ug_style_of(ug, &s);
        assert(s.side == 10 && s.front == 6 && fabsf(s.half_width - 0.85f) < 1e-6f && fabsf(s.zf - 1.6f) < 1e-6f && fabsf(s.zr + 1.4f) < 1e-6f);
        ug_edge_point(&s, 0, p);                   /* the front edge's first light: the car's left */
        assert(p[0] < -0.5f && fabsf(p[2] - 1.6f) < 1e-5f && fabsf(p[1] - 0.01f) < 1e-6f);
        ug_edge_point(&s, 6, p);                   /* the right side's first light, clear of the wheel arch */
        assert(fabsf(p[0] - 0.85f) < 1e-6f && p[2] < 1.6f - 0.8f + 1e-5f);
        ug_pixels(&s, eff, 0x0f, px, 32);
        assert(px[0].zone == UG_ZONE_FRONT && px[6].zone == UG_ZONE_RIGHT && px[16].zone == UG_ZONE_REAR && px[22].zone == UG_ZONE_LEFT);
        ug_effect_now(&s, &au, UG_STATIC, &px[0], 10.0, rgb, &level);
        assert(level == 1.0f && rgb[0] == 0.0f && rgb[1] == 1.0f && rgb[2] == 1.0f);
        ug_effect_now(&s, &au, UG_RAINBOW, &px[6], 0.0, rgb, &level);
        assert(level == 1.0f && rgb[0] == 1.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
        ug_effect_now(&s, &au, UG_POLICE, &px[22], 0.01, rgb, &level);   /* the left side: red, its turn */
        assert(rgb[0] == 1.0f && rgb[2] == 0.0f && level == 1.0f);
        ug_audio_frame(&au, s.speed, 3.0, 1000);
        for (i = 0; i < 4; i++) assert(au.lvl[i] >= 0.0f && au.lvl[i] <= 1.0f && au.peak[i] == au.lvl[i]);
    }

    /* NitroSystem's online rule */
    memset(&api, 0, sizeof api);
    api.size = sizeof api; api.wait_for_mods = fake_wait; api.get_interface = fake_iface; api.mod_version = fake_version;
    g_api = &api;
    nos_find();
    assert(g_nos_kind == NOS_SWITCH && g_blocked == 1 && !strcmp(g_who, "FH2MP") && !g_nos_allowed && strstr(g_log, "NitroSystem 2.0.0 found"));
    assert(nos_flags() == MODS_NOS_SWITCH);
    g_sess = 1; g_ctx = AD_FREE_ROAM; g_rt = 0;
    nos_rule_tick();
    assert(g_blocked == 1 && !g_nos_allowed);                                       /* the server has not said so */
    InterlockedExchange(&g_nos_server_ok, 1); InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64());
    tick_later();
    assert(g_blocked == 0 && g_nos_allowed && strstr(g_log, "nitro: NOS on (online Free Roam"));
    g_burning = 1; assert(nos_flags() == (MODS_NOS_SWITCH | MODS_NOS_ALLOWED | MODS_NOS_BURNING)); g_burning = 0;
    n = (unsigned)g_block_calls; tick_later(); assert(g_block_calls == (int)n);   /* nothing changed: not called again */
    g_ctx = 5; tick_later(); assert(g_blocked == 1 && strstr(g_log, "in a session, and the event context is not free roam"));
    g_ctx = 3; InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64()); tick_later();
    assert(g_blocked == 0 && strstr(g_log, "event context 3"));                     /* an online Free Roam lobby says 3: the server decides */
    g_ctx = AD_FREE_ROAM; InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64()); tick_later(); assert(g_blocked == 0);
    g_rt = 1; tick_later(); assert(g_blocked == 1);
    g_rt = -1; InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64()); tick_later(); assert(g_blocked == 1);   /* unreadable: off */
    g_rt = 0; InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64()); tick_later(); assert(g_blocked == 0);
    g_sess = 0; g_ctx = 3; tick_later(); assert(g_blocked == 1 && strstr(g_log, "solo, and the event context is not free roam"));   /* solo: the context alone */
    g_ctx = AD_FREE_ROAM; InterlockedExchange(&g_nos_server_ok, 0); tick_later();
    assert(g_blocked == 0 && g_nos_allowed && strstr(g_log, "solo Free Roam"));       /* solo Free Roam: no server needed */
    InterlockedExchange(&g_nos_server_ok, 1);
    g_sess = 1; InterlockedExchange(&g_nos_server_ok, 0); InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64()); tick_later();
    assert(g_blocked == 1);                                                         /* the server says event / Road Trip */
    InterlockedExchange(&g_nos_server_ok, 1); InterlockedExchange64(&g_nos_server_at, (LONG64)GetTickCount64() - NOS_SERVER_FRESH_MS - 10); tick_later();
    assert(g_blocked == 1);                                                         /* an old reply says nothing */
    /* no NitroSystem: nothing to switch, no flags */
    g_have_nos = 0; g_nos_set = NULL; g_nos_active = NULL; g_log[0] = 0;
    nos_find(); assert(g_nos_kind == NOS_ABSENT && nos_flags() == 0 && strstr(g_log, "not loaded"));

    /* Underglow's status through the platform's command pipe */
    g_ug_loaded = 1;
    assert(ask_pipe(STATUS, ug) && !strcmp(g_asked, "Underglow status") && ug[0] == 1 && ug[1] == 0x0b && ug[2] == 15);
    assert(ask_pipe("info: Underglow\nneon on, primary color Cyan (0 1 1), secondary Pink, brightness 0.25, effects: front Rainbow Wave, right Comet, rear Police, "
                    "left Static, at 0.3 a second; parts on: front right rear left\n"
                    "shape: Side 10 Front 6 Rear 6 Height 0.01 FrontInset 0.5 RearInset 0.7 SideOffset -0.2 SideTrim 0.8 Range 1 Power 4 Inner 60 Outer 170\n"
                    "car 2183: half width 0.95 (from its lights), front 2.10, rear -2.00 (from its lights)\nneon: 48 light(s), lit\n"
                    "Underglow Kit: not in the garage (see platform.log); every part can light\n", ug) && ug[0] == 1 && ug[1] == 0x0f);
    g_log[0] = 0;
    assert(!ask_pipe("error: Underglow's command status crashed\n", ug) && !ug[0] && strstr(g_log, "not shared"));
    {
        wchar_t dir[MAX_PATH]; char text[2048];
        GetTempPathW(MAX_PATH, dir);
        lstrcpyW(g_ug_ini, dir); lstrcatW(g_ug_ini, L"fh2mp_test_Underglow.ini");
        WritePrivateProfileStringW(L"CustomColors", L"Color3", L"#FF69B4", g_ug_ini);
        WritePrivateProfileStringW(L"CustomColors", L"Name3", L"Hot Pink", g_ug_ini);
        status_variant(text, "secondary Pink", "secondary Hot Pink");
        assert(ask_pipe(text, ug) && ug[12] == 255 && ug[13] == 105 && ug[14] == 180);
        DeleteFileW(g_ug_ini);
    }
    g_ug_loaded = 0;
    assert(!ug_read(ug) && !ug[0]);

    /* the other players' states as the renderers see them: fresh, then too old */
    {
        mods_record r[1], got[MODS_MAX];
        memset(r, 0, sizeof r); r[0].owner = 3; r[0].car = 4; r[0].nos = MODS_NOS_BURNING;
        mods_store(r, 1, MODS_SERVER_NOS_OK, GetTickCount64());
        assert(mods_snapshot(got, GetTickCount64()) == 1 && got[0].owner == 3 && g_nos_server_ok);
        assert(mods_snapshot(got, GetTickCount64() + MODS_FRESH_MS + 1) == 0);
    }
    puts("mods: ok");
    return 0;
}
