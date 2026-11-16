/* The zip manifest check (crash #99, "0xc0000005 at game+0xfd1927 reading 0x0"): zipmanifest_rule.inc says why the game stops there.
 *
 * A guard in game code can't be in place in time: the game reads its manifest ~90 ms after Extra Options has patched it (crash dump
 * 20261115T175053-029-8700), and FH2MP's first game hook lands 100-440 ms after that point (platform logs of win1/win2, 2026-11-12/13).
 * So the files are checked instead, from fh2mod_init, seconds before Azimuth maps the game: each of <Mount>\mediaoverride, \mediapc and
 * \media's zipmanifest.xml, and a line whose zip is in none of the folders the game can read it from is taken out, the file before kept
 * next to it as zipmanifest.xml.fh2mp-<date>.bak. Those folders: <Mount>\media, \mediaoverride and \mediapc (the zip, or a folder of its
 * name: Extra Options' in-zip overrides), every Extra Options mod's media folder (turned on or not) and every car pack's in fh2-cars.
 *
 * Why it only crashes with FH2MP: the launcher turns Extra Options' media overrides on for online play (gamedb.ensure_media_overrides), and
 * from then on Extra Options reads mediaoverride and mediapc, where a car mod's manifest stayed when its zip was removed. Both folders are
 * shared by every Azimuth folder on the same game folder.
 *
 * The platform's own merged manifests (car packs, data\titles\1A96545B\tmp\fh2-platform) are built from these before any mod starts: one
 * built from a manifest changed here keeps the line until the next start, when it's built again. FH2MP_NO_ZIPGUARD=1 switches the check off. */
#include "zipmanifest_rule.inc"

#define ZG_MAX_DIRS 96
#define ZG_MAX_FILE (8u << 20)
#define ZG_MAX_NAMED 6                                   /* zips named in the platform log's line for one file */

static WCHAR g_zg_dirs[ZG_MAX_DIRS][MAX_PATH];           /* where game:\media\<rel> can be; no trailing backslash */
static int g_zg_folder_form[ZG_MAX_DIRS];                /* a folder of the zip's name counts there too */
static int g_zg_dirs_n;

struct zg_file {
    const WCHAR *path;
    unsigned long left;
    char named[ZG_MAX_NAMED * (ZM_REL_MAX + 2) + 8];     /* "cars\a.zip, cars\b.zip" for the platform log */
    int named_n;
};

static void zg_cat(char *buf, unsigned long cap, const char *s)
{
    unsigned long n = 0;
    while (n < cap && buf[n]) {
        n++;
    }
    while (*s && n + 1 < cap) {
        buf[n++] = *s++;
    }
    if (n < cap) {
        buf[n] = 0;
    }
}

static void zg_cat_w(char *buf, unsigned long cap, const WCHAR *w)
{
    char u[3 * MAX_PATH + 1];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL) > 0) {
        zg_cat(buf, cap, u);
    } else {
        zg_cat(buf, cap, "?");
    }
}

static void zg_cat_dec(char *buf, unsigned long cap, unsigned long v)
{
    char d[12];
    int i = 11;
    d[i] = 0;
    do {
        d[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    zg_cat(buf, cap, d + i);
}

/* <a><b><c> as one more place to look, when it is a folder. */
static void zg_add_dir(const WCHAR *a, const WCHAR *b, const WCHAR *c, int folder_form)
{
    DWORD at;
    WCHAR *d;
    if (g_zg_dirs_n >= ZG_MAX_DIRS || lstrlenW(a) + lstrlenW(b) + lstrlenW(c) + 2 >= MAX_PATH) {
        return;
    }
    d = g_zg_dirs[g_zg_dirs_n];
    lstrcpyW(d, a);
    lstrcatW(d, b);
    lstrcatW(d, c);
    at = GetFileAttributesW(d);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }
    g_zg_folder_form[g_zg_dirs_n++] = folder_form;
}

/* Every <parent>\<name>\media folder (Extra Options' mods, car packs). */
static void zg_add_subdirs(const WCHAR *parent)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (lstrlenW(parent) + 3 >= MAX_PATH) {
        return;
    }
    lstrcpyW(pat, parent);
    lstrcatW(pat, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.cFileName[0] == L'.' && (!fd.cFileName[1] ||
            (fd.cFileName[1] == L'.' && !fd.cFileName[2])))) {
            continue;
        }
        if (lstrlenW(parent) + lstrlenW(fd.cFileName) + 2 >= MAX_PATH) {
            continue;
        }
        lstrcpyW(sub, parent);
        lstrcatW(sub, L"\\");
        lstrcatW(sub, fd.cFileName);
        zg_add_dir(sub, L"\\media", L"", 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 1 when the game can open game:\media\<rel> from one of the folders, and when that can't be told (a name that doesn't convert, a path
 * too long, an error other than "not there"): only a zip that is plainly nowhere counts as missing. */
static int zg_present(const char *rel, void *user)
{
    WCHAR w[ZM_REL_MAX + 1], p[MAX_PATH];
    int i, n;
    (void)user;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rel, -1, w, ZM_REL_MAX + 1);
    if (n <= 1) {
        return 1;
    }
    for (i = 0; i < g_zg_dirs_n; i++) {
        DWORD at, err;
        if (lstrlenW(g_zg_dirs[i]) + 1 + lstrlenW(w) >= MAX_PATH) {
            return 1;
        }
        lstrcpyW(p, g_zg_dirs[i]);
        lstrcatW(p, L"\\");
        lstrcatW(p, w);
        at = GetFileAttributesW(p);
        if (at != INVALID_FILE_ATTRIBUTES) {
            if (!(at & FILE_ATTRIBUTE_DIRECTORY)) {
                return 1;
            }
        } else if ((err = GetLastError()) != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            return 1;
        }
        n = lstrlenW(p);
        if (g_zg_folder_form[i] && n > 4 && p[n - 4] == L'.' && (p[n - 3] | 0x20) == L'z' && (p[n - 2] | 0x20) == L'i' &&
            (p[n - 1] | 0x20) == L'p') {
            p[n - 4] = 0;
            at = GetFileAttributesW(p);
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                return 1;
            }
        }
    }
    return 0;
}

static void zg_dropped(const char *rel, void *user)
{
    struct zg_file *f = (struct zg_file *)user;
    f->left++;
    if (f->left <= 20) {
        log_begin();
        log_raw("zipguard: ");
        log_wide(f->path);
        log_raw(" lists game:\\media\\");
        log_raw(rel);
        log_raw(", which is in none of the folders the game reads zips from; opening it the game would read NULL at game+0xfd1927\n");
    }
    if (f->named_n < ZG_MAX_NAMED) {
        if (f->named_n++) {
            zg_cat(f->named, sizeof f->named, ", ");
        }
        zg_cat(f->named, sizeof f->named, rel);
    } else if (f->named_n++ == ZG_MAX_NAMED) {
        zg_cat(f->named, sizeof f->named, ", ...");
    }
}

static void zg_put2(WCHAR *w, unsigned v)
{
    w[0] = (WCHAR)(L'0' + v / 10 % 10);
    w[1] = (WCHAR)(L'0' + v % 10);
}

/* Writes `data` over `file`, keeping the file before as <file>.fh2mp-YYYYMMDD-HHMMSS.bak (named into `bak`): 0, or the Windows error
 * (then `file` is as it was). By way of a temporary file of this process, so the game, the platform and another Azimuth folder's FH2MP
 * never read it half written. */
static DWORD zg_replace(const WCHAR *file, const char *data, unsigned long n, WCHAR *bak)
{
    WCHAR tmp[MAX_PATH];
    SYSTEMTIME t;
    HANDLE h;
    DWORD put = 0, err, pid = GetCurrentProcessId();
    int len = lstrlenW(file), i;
    if (len + 40 >= MAX_PATH) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    GetLocalTime(&t);
    lstrcpyW(bak, file);
    lstrcatW(bak, L".fh2mp-00000000-000000.bak");
    zg_put2(bak + len + 7, t.wYear / 100);
    zg_put2(bak + len + 9, t.wYear % 100);
    zg_put2(bak + len + 11, t.wMonth);
    zg_put2(bak + len + 13, t.wDay);
    zg_put2(bak + len + 16, t.wHour);
    zg_put2(bak + len + 18, t.wMinute);
    zg_put2(bak + len + 20, t.wSecond);
    if (!CopyFileW(file, bak, TRUE)) {
        return GetLastError();
    }
    lstrcpyW(tmp, file);
    lstrcatW(tmp, L".fh2mp-tmp-");
    i = lstrlenW(tmp);
    do {
        tmp[i++] = (WCHAR)(L'0' + pid % 10);
        pid /= 10;
    } while (pid && i < MAX_PATH - 1);
    tmp[i] = 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (!WriteFile(h, data, n, &put, NULL) || put != n || !FlushFileBuffers(h)) {
        err = GetLastError();
        CloseHandle(h);
        DeleteFileW(tmp);
        return err ? err : ERROR_WRITE_FAULT;
    }
    CloseHandle(h);
    SetFileAttributesW(file, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
        DeleteFileW(tmp);
        return err;
    }
    return 0;
}

/* One manifest: read, filtered, written back when a few lines name zips that are nowhere. Quiet when there's nothing to take out. */
static void zg_check_file(const WCHAR *path, char *in, char *out)
{
    struct zg_file f;
    HANDLE h;
    LARGE_INTEGER size;
    DWORD got = 0, err;
    unsigned long out_len = 0, zips = 0, left;
    WCHAR bak[MAX_PATH];
    char line[1400];
    DWORD t0 = GetTickCount();

    size.QuadPart = 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            log_begin();
            log_raw("zipguard: could not open ");
            log_wide(path);
            log_raw(" to check it (Windows error ");
            log_dec(err);
            log_raw("); if a line in it names a zip that is not there, the game stops at game+0xfd1927\n");
        }
        return;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart > ZG_MAX_FILE || !ReadFile(h, in, (DWORD)size.QuadPart, &got, NULL) ||
        got != (DWORD)size.QuadPart) {
        err = GetLastError();
        CloseHandle(h);
        log_begin();
        log_raw("zipguard: could not read ");
        log_wide(path);
        log_raw(size.QuadPart > ZG_MAX_FILE ? " (larger than 8 MB), so it was not checked\n" : " to check it (Windows error ");
        if (size.QuadPart <= ZG_MAX_FILE) {
            log_dec(err);
            log_raw(")\n");
        }
        return;
    }
    CloseHandle(h);
    f.path = path;
    f.left = 0;
    f.named[0] = 0;
    f.named_n = 0;
    left = zm_filter(in, got, out, &out_len, &zips, zg_present, zg_dropped, &f);
    if (!left) {
        return;
    }
    if (!zm_should_write(left, zips)) {
        log_begin();
        log_raw("zipguard: ");
        log_wide(path);
        log_raw(" lists ");
        log_dec(left);
        log_raw(" of its ");
        log_dec(zips);
        log_raw(" zips in none of the game's folders; that many means these folders aren't where the game reads from (game folder ");
        log_wide(g_api->game_dir);
        log_raw("), so the file was left as it is\n");
        return;
    }
    err = zg_replace(path, out, out_len, bak);
    log_begin();
    if (err) {
        log_raw("zipguard: could not take the ");
        log_dec(left);
        log_raw(" line(s) out of ");
        log_wide(path);
        log_raw(" (Windows error ");
        log_dec(err);
        log_raw(", while keeping a copy or writing); the game stops at game+0xfd1927 opening the first of those zips\n");
    } else {
        log_raw("zipguard: took ");
        log_dec(left);
        log_raw(" of ");
        log_dec(zips);
        log_raw(" zip line(s) out of ");
        log_wide(path);
        log_raw("; the file before is ");
        log_wide(bak);
        log_raw(" (");
        log_dec(GetTickCount() - t0);
        log_raw(" ms)\n");
    }
    line[0] = 0;
    if (err) {
        zg_cat(line, sizeof line, "zipguard: could not take ");
        zg_cat(line, sizeof line, f.named);
        zg_cat(line, sizeof line, " out of ");
        zg_cat_w(line, sizeof line, path);
        zg_cat(line, sizeof line, " (Windows error ");
        zg_cat_dec(line, sizeof line, err);
        zg_cat(line, sizeof line, "): no such zip is in the game's folders, and the game stops at game+0xfd1927 opening it");
    } else {
        zg_cat(line, sizeof line, "zipguard: took ");
        zg_cat(line, sizeof line, f.named);
        zg_cat(line, sizeof line, " out of ");
        zg_cat_w(line, sizeof line, path);
        zg_cat(line, sizeof line, ": no such zip is in the game's folders, and the game would stop at game+0xfd1927 opening it (the file before: ");
        zg_cat_w(line, sizeof line, bak + lstrlenW(path) - 15);
        zg_cat(line, sizeof line, ")");
    }
    g_api->log(g_mod, line);
}

static void zipmanifest_check(void)
{
    static const WCHAR *const roots[3] = {L"\\mediaoverride", L"\\mediapc", L"\\media"};
    WCHAR file[MAX_PATH], dir[MAX_PATH];
    char *in, *out;
    int i;

    if (!g_api->game_dir[0] || lstrlenW(g_api->game_dir) + 40 >= MAX_PATH || lstrlenW(g_api->azimuth_dir) + 40 >= MAX_PATH) {
        return;
    }
    g_zg_dirs_n = 0;
    zg_add_dir(g_api->game_dir, L"\\media", L"", 0);
    if (!g_zg_dirs_n) {
        log_begin();
        log_raw("zipguard: the game folder ");
        log_wide(g_api->game_dir);
        log_raw(" has no media folder, so its zip manifests were not checked\n");
        return;
    }
    zg_add_dir(g_api->game_dir, L"\\mediaoverride", L"", 1);
    zg_add_dir(g_api->game_dir, L"\\mediapc", L"", 1);
    lstrcpyW(dir, g_api->azimuth_dir);
    lstrcatW(dir, L"\\mods\\Forza Horizon 2");
    zg_add_subdirs(dir);
    lstrcpyW(dir, g_api->azimuth_dir);
    lstrcatW(dir, L"\\mods");
    zg_add_subdirs(dir);
    lstrcpyW(dir, g_api->azimuth_dir);
    lstrcatW(dir, L"\\fh2-cars");
    zg_add_subdirs(dir);
    in = (char *)VirtualAlloc(NULL, 2 * ZG_MAX_FILE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!in) {
        log_begin();
        log_raw("zipguard: could not allocate 16 MB to read the zip manifests (Windows error ");
        log_dec(GetLastError());
        log_raw("), so they were not checked\n");
        return;
    }
    out = in + ZG_MAX_FILE;
    for (i = 0; i < 3; i++) {
        lstrcpyW(file, g_api->game_dir);
        lstrcatW(file, roots[i]);
        lstrcatW(file, L"\\zipmanifest.xml");
        zg_check_file(file, in, out);
    }
    VirtualFree(in, 0, MEM_RELEASE);
}
