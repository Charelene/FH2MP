/* FH2MP.Online (fh2mp_online.h): what other FH2 Mod Platform mods may ask FH2MP, so a car pack or tuning mod can keep custom cars and parts out of
 * online events in its own menus. Answers come from the session (g_token: online; g_gamedb_path: the server's database is put in place) and,
 * for a game that keeps its own car packs, from the server's part list (picker_pi.inc, fh2mp\base-parts.txt). The game server enforces the rule
 * either way. */
#include "fh2mp_online.h"

static int ona_is_online(void) { return g_token[0] != 0; }

static int ona_keeps_own_cars(void) { return g_token[0] != 0 && !g_gamedb_path[0]; }

static int ona_car_allowed_in_events(uint32_t car_id)
{
    unsigned klass;
    float pi;
    if (!ona_keeps_own_cars()) {
        return 1;           /* offline, or the server's database: every car the game has is the server's */
    }
    return g_ppi_base_ready ? ppi_base_stock(NULL, car_id, &klass, &pi) : -1;
}

static int ona_part_allowed_in_events(const char *table, uint32_t part_id)
{
    if (!ona_keeps_own_cars()) {
        return 1;
    }
    if (!table || !g_ppi_base_ready || ppi_table_index(table, (unsigned)lstrlenA(table)) < 0) {
        return -1;          /* no list, or a table the list does not carry */
    }
    return ppi_base_exists(NULL, table, part_id);
}

static const FH2MPOnline g_online_api = {sizeof(FH2MPOnline), FH2MP_ONLINE_VERSION, ona_is_online, ona_keeps_own_cars, ona_car_allowed_in_events,
                                         ona_part_allowed_in_events};

/* After the session is read: a game that keeps its own car packs loads the server's part list, then the interface is published. */
static void online_api_start(void)
{
    if (ona_keeps_own_cars()) {
        wchar_t path[MAX_PATH];
        lstrcpynW(path, g_exe_dir, MAX_PATH - 32);
        lstrcatW(path, L"\\fh2mp\\base-parts.txt");
        ppi_base_load(path);
    }
    if (g_api->api_version < 2 || !g_api->publish_interface) {
        return;
    }
    g_api->publish_interface(g_mod, FH2MP_ONLINE_NAME, FH2MP_ONLINE_VERSION, &g_online_api);
    log_begin();
    log_raw("online-api: published " FH2MP_ONLINE_NAME " version ");
    log_dec(FH2MP_ONLINE_VERSION);
    log_raw(ona_keeps_own_cars() ? " (this game keeps its own car packs: cars and parts are checked against the server's list)\n"
                                 : (g_token[0] ? " (the server's car database: every car may race)\n" : " (offline: every car may race)\n"));
}
