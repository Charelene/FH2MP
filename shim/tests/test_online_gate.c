/* The online gate (src/online_gate.inc) against a stand-in for the platform's FH2Platform.Online: what going_online's answer does to the game
 * (online, held offline), that it is asked once and only with a session, and an older platform without the interface. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"

static char g_raw[2048];
static int g_failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)
static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void log_dec(unsigned long v) { char b[16]; sprintf(b, "%lu", v); log_raw(b); }

static char g_token[96];
static FH2ModApi g_api_obj;
static const FH2ModApi *g_api = &g_api_obj;

static int g_answer, g_asked, g_online_now;
static const char *g_names;
static int fake_playing(void) { return g_online_now; }
static int fake_going(char *names, size_t size)
{
    g_asked++;
    if (names && size) lstrcpynA(names, g_names ? g_names : "", (int)size);
    if (g_answer == 0) g_online_now = 1;
    return g_answer;
}
static FH2PlatformOnline g_iface = {sizeof(FH2PlatformOnline), fake_playing, fake_going};
static int g_have_iface;
static const void *fake_get_interface(const char *name, uint32_t min_version)
{
    return g_have_iface && !strcmp(name, FH2_ONLINE_INTERFACE) && min_version <= 1 ? &g_iface : NULL;
}

#include "src/online_gate.inc"

static void reset(int token, int iface, int answer, const char *names)
{
    INIT_ONCE fresh = INIT_ONCE_STATIC_INIT;
    g_gate_once = fresh;
    g_net_held = 0;
    g_held_names[0] = 0;
    g_held_count = 0;
    strcpy(g_token, token ? "fh2.test" : "");
    g_have_iface = iface;
    g_answer = answer;
    g_names = names;
    g_asked = g_online_now = 0;
    g_raw[0] = 0;
}

int main(void)
{
    g_api_obj.api_version = 10;
    g_api_obj.get_interface = fake_get_interface;

    reset(1, 1, 0, "");                                   /* the usual Play: nothing ran first, the game goes online */
    CHECK(online_allowed() && online_allowed());
    CHECK(g_asked == 1);                                   /* asked once */
    CHECK(strstr(g_raw, "the platform counts it as online") != NULL);

    reset(1, 1, 2, "Nitro, Other");                       /* the session came late, and two offline-only mods had run */
    CHECK(!online_allowed() && !online_allowed() && g_asked == 1);
    CHECK(g_net_held == 1 && g_held_count == 2 && !strcmp(g_held_names, "Nitro, Other"));
    CHECK(strstr(g_raw, "2 mods that shouldn't run online have already run in this game (Nitro, Other)") != NULL);

    reset(1, 1, 1, "Car packs");                          /* fh2-cars read through Extra Options */
    CHECK(!online_allowed());
    CHECK(strstr(g_raw, "1 mod that shouldn't run online has already run in this game (Car packs)") != NULL);

    reset(0, 1, 2, "Nitro");                              /* no session: nothing to ask, the platform counts it offline too */
    CHECK(online_allowed() && g_asked == 0 && g_raw[0] == 0);

    reset(1, 0, 2, "Nitro");                              /* platform 2.8.0: no interface, online as before */
    CHECK(online_allowed() && g_asked == 0);
    CHECK(strstr(g_raw, "older than 2.9.0") != NULL);

    reset(1, 1, 0, "");                                   /* an interface smaller than this build's: not used */
    g_iface.size = 8;
    CHECK(online_allowed() && g_asked == 0);
    g_iface.size = sizeof g_iface;

    if (!g_failed) puts("online gate: all checks pass");
    return g_failed != 0;
}
