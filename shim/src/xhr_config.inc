/* Publish the complete immutable LSP endpoint before any request can use it. */
static wchar_t g_lsp_host[128] = L"127.0.0.1";
static unsigned g_lsp_port = 5080;
static int g_lsp_secure;
/* 0 = not started, 1 = reading, 2 = complete (including default/failure). */
static volatile LONG g_lsp_cfg_loaded;

static void xhr_read_cfg(void)
{
    wchar_t path[MAX_PATH];
    char raw[160];
    DWORD got = 0;
    HANDLE f;
    int i, colon = -1, n;

    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-lsp.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!ReadFile(f, raw, sizeof raw - 1, &got, NULL)) {
        got = 0;
    }
    CloseHandle(f);
    raw[got] = '\0';
    {
        const char *r = raw;
        if (r[0] == 'h' && r[1] == 't' && r[2] == 't' && r[3] == 'p') {
            if (r[4] == 's' && r[5] == ':' && r[6] == '/' && r[7] == '/') {
                g_lsp_secure = 1;
                g_lsp_port = 443;
                r += 8;
            } else if (r[4] == ':' && r[5] == '/' && r[6] == '/') {
                g_lsp_port = 80;
                r += 7;
            }
        }
        n = 0;
        while (r[n] && r[n] != '\r' && r[n] != '\n' && r[n] != ' ' && r[n] != '/') {
            if (r[n] == ':') {
                colon = n;
            }
            n++;
        }
        if (n == 0) {
            return;
        }
        for (i = 0; i < (colon >= 0 ? colon : n) && i < 120; i++) {
            g_lsp_host[i] = (wchar_t)(unsigned char)r[i];
        }
        g_lsp_host[i] = 0;
        if (colon >= 0) {
            unsigned v = 0;
            for (i = colon + 1; i < n; i++) {
                v = v * 10 + (unsigned)(r[i] - '0');
            }
            if (v) {
                g_lsp_port = v;
            }
        }
    }
}

static void xhr_load_cfg(void)
{
    if (InterlockedCompareExchange(&g_lsp_cfg_loaded, 1, 0) == 0) {
        xhr_read_cfg();
        InterlockedExchange(&g_lsp_cfg_loaded, 2);
    } else {
        while (InterlockedCompareExchange(&g_lsp_cfg_loaded, 0, 0) != 2) {
            Sleep(1);
        }
    }
}
