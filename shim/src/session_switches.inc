/* Kill switches from the launcher: `switches=NO_HONK,NO_PARTYHUD` in fh2mp-session.txt, written from the signed hotfix document the launcher
 * fetches (launcher/fh2mp_launcher/hotfix.py). Each name becomes FH2MP_<name>=1 in this process before late_init reads any FH2MP_NO_* variable,
 * so a patch that crashes players can be switched off without a new shim. Only NO_ names of capitals, digits and underscores are taken: the
 * list can turn a patch off, never set a token, a server address or a debug option. */
#define SWITCH_NAME_MAX 32
#define SWITCHES_MAX 32

/* The next name of a comma-separated list. -> 1 with `name` filled (valid or not, see *ok), 0 at the end. */
static int switch_next(const char **p, char *name, int *ok)
{
    const char *s = *p;
    int n = 0, valid = 1;
    while (*s == ',' || *s == ' ') {
        s++;
    }
    if (!*s) {
        *p = s;
        return 0;
    }
    while (*s && *s != ',') {
        char c = *s++;
        if (c == ' ') {
            continue;
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') || n >= SWITCH_NAME_MAX) {
            valid = 0;
        }
        if (n < SWITCH_NAME_MAX) {
            name[n++] = c;
        }
    }
    name[n] = 0;
    *ok = valid && n > 3 && name[0] == 'N' && name[1] == 'O' && name[2] == '_';
    *p = s;
    return 1;
}

#ifdef _WIN32
static int g_switches_set;

static void session_switches(const char *list)
{
    char name[SWITCH_NAME_MAX + 1], var[SWITCH_NAME_MAX + 8];
    int ok;
    while (switch_next(&list, name, &ok)) {
        log_begin();
        if (!ok || g_switches_set >= SWITCHES_MAX) {
            log_raw("session: switch ignored '");
            log_raw(name);
            log_raw(ok ? "' (more than 32 switches)\n" : "' (only NO_ names of A-Z, 0-9 and _ are taken)\n");
            continue;
        }
        lstrcpyA(var, "FH2MP_");
        lstrcatA(var, name);
        SetEnvironmentVariableA(var, "1");
        g_switches_set++;
        log_raw("session: switch ");
        log_raw(var);
        log_raw(" set by the launcher's hotfix list\n");
    }
}
#endif
