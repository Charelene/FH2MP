/*
 * Keeping the car database another mod had in mediaoverride while the server's copy is there, and putting it back for offline play.
 * A traffic car mod's database, or a car pack's put in mediaoverride, would otherwise be lost after one online session. These are the
 * decisions only, on what the caller found out about the files, with no library calls (the shim links KERNEL32 only); gamedb_place and
 * gamedb_put_back in fh2mp_mod.c do the file work. Checked by shim/test_gamedb_keep.sh.
 *
 *   F   <Azimuth>\fh2mp\gamedbRC.slt, the launcher's copy of the server's database
 *   T   <Mount>\mediaoverride\Stripped\gamedbRC.slt, what the game reads
 *   K   <Azimuth>\fh2mp\gamedb-kept.slt, the database another mod had at T, moved aside for online play
 *   K'  <Azimuth>\fh2mp\gamedb-kept.previous.slt, the one before it: a newer one replaced it, or it was removed from T
 *   B   <Azimuth>\fh2mp\gamedb-put-back.txt, K's "size date" while K is back at T (FH2MP put it back, or found it there offline)
 *   R   "size date" of the last copies FH2MP put at T, newest last: <Azimuth>\fh2mp\gamedb-placed.txt (the launcher adds F's when it
 *       replaces F), and <Mount>\mediaoverride\Stripped\platform-db-fh2mp.txt for every Azimuth folder using that game folder (the FH2
 *       Mod Platform's list of FH2MP's copies from 2.7.2 on, like its fh2-mods\platform-db-fh2mp.txt, which is only read). CopyFileW
 *       keeps F's date, so an older copy of FH2MP's is still known once the server's database has changed.
 *   S   <Azimuth>\fh2-mods\platform-db.txt, the platform's record of its copy at T
 */
typedef struct {
    unsigned long long size, time;  /* time: last written, in 100 ns ticks (FILETIME) */
} gk_stamp;

#define GK_PLACED_MAX 4
/* A date up to 2 s off is the same when T's is whole 10 ms: FAT32 keeps dates to 2 s, exFAT to 10 ms, NTFS to 100 ns. */
#define GK_NEAR (2 * 10000000ULL)
#define GK_COARSE 100000ULL
/* The FH2 Mod Platform up to 2.7.1 writes fh2-mods\platform-db.txt 4-7 ms after its copy, and marks nothing: a T dated from 10 s
 * before that file to 2 s after is the platform's. */
#define GK_OLD_BEFORE (10 * 10000000ULL)
#define GK_OLD_AFTER (2 * 10000000ULL)
/* SQLite application_id (big-endian, header byte 68) of every copy the platform builds from 2.7.2 on, and of FH2ML's: "FH2D" */
#define GK_MARK 0x46483244u
/* The bytes of T read for gk_classify: SQLite's header */
#define GK_HEAD 100

/* What T is: FH2MP's copy, not a whole database (a copy cut short), a platform build, or another mod's database */
enum { GK_FH2MP, GK_BROKEN, GK_PLATFORM, GK_OTHER };

typedef struct {
    gk_stamp t;
    int same_as_server;             /* T has F's bytes */
    const gk_stamp *placed;         /* every R list, oldest first in each */
    int nplaced;
    const unsigned char *head;      /* T's first bytes, GK_HEAD of them unless T is shorter */
    int nhead;
    int have_state;                 /* S is there and not empty */
    unsigned long long state_time;  /* its date */
} gk_facts;

/* `t` is `s`: the same size, and the same date, or one at most 2 s off when t's is whole 10 ms (the platform does the same). */
static int gk_nearly(const gk_stamp *s, const gk_stamp *t)
{
    unsigned long long d = s->time > t->time ? s->time - t->time : t->time - s->time;
    return s->size == t->size && (d == 0 || (t->time % GK_COARSE == 0 && d <= GK_NEAR));
}

static unsigned long gk_be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static int gk_sqlite(const unsigned char *h, int n)
{
    static const char magic[16] = "SQLite format 3";
    int i;

    if (n < 72) {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        if (h[i] != (unsigned char)magic[i]) {
            return 0;
        }
    }
    return 1;
}

/* A SQLite database with the platform's mark */
static int gk_marked(const unsigned char *h, int n)
{
    return gk_sqlite(h, n) && gk_be32(h + 68) == GK_MARK;
}

/* A whole SQLite database, `size` bytes, by its header: one cut short (a copy stopped half way) has fewer pages than the header says.
 * The page count (byte 28) counts only when it was written with the change counter (byte 24 equals byte 92); SQLite before 3.7.0,
 * the game's own among them, doesn't keep it. */
static int gk_whole(const unsigned char *h, int n, unsigned long long size)
{
    unsigned long page, pages;

    if (n < GK_HEAD || !gk_sqlite(h, n)) {
        return 0;
    }
    page = (unsigned long)h[16] << 8 | h[17];
    page = page == 1 ? 65536 : page;
    if (page < 512 || page > 65536 || (page & (page - 1))) {
        return 0;
    }
    pages = gk_be32(h + 28);
    return !pages || gk_be32(h + 24) != gk_be32(h + 92) || (unsigned long long)pages * page <= size;
}

/* 1. FH2MP's copy: F's bytes, or a date an R list has. 2. Not a whole database: nothing worth keeping. 3. A platform build: marked, or
 * dated just before an older platform's S; the platform builds it again, and from 2.7.2 on keeps what it built on. 4. Another mod's
 * database. */
static int gk_classify(const gk_facts *f)
{
    int i;

    if (f->same_as_server) {
        return GK_FH2MP;
    }
    for (i = 0; i < f->nplaced; i++) {
        if (gk_nearly(&f->placed[i], &f->t)) {
            return GK_FH2MP;
        }
    }
    if (!gk_whole(f->head, f->nhead, f->t.size)) {
        return GK_BROKEN;
    }
    if (gk_marked(f->head, f->nhead) ||
        (f->have_state && f->t.time + GK_OLD_BEFORE >= f->state_time && f->t.time <= f->state_time + GK_OLD_AFTER)) {
        return GK_PLATFORM;
    }
    return GK_OTHER;
}

/* Online, before the server's copy goes over a T that isn't it (what: gk_classify): keep T as K first? K becomes K' only once T is
 * copied, so a copy that fails moves nothing. */
enum { GK_NOTHING, GK_KEEP, GK_KEEP_REPLACING /* K becomes K' */ };

static int gk_online(int what, int have_kept, int same_as_kept)
{
    if (what != GK_OTHER || (have_kept && same_as_kept)) {
        return GK_NOTHING;
    }
    return have_kept ? GK_KEEP_REPLACING : GK_KEEP;
}

/* The FH2 Mod Platform from 2.7.2 on (`v`: its version, "2.7.2") keeps another mod's database it builds on, and puts it back itself:
 * once K is back at T, what then happens to T is the platform's business. Up to 2.7.1 it builds from the game's own database and keeps
 * nothing. */
static int gk_platform_keeps(const char *v)
{
    unsigned long part[3] = {0, 0, 0};
    int i;

    for (i = 0; i < 3 && v; i++) {
        while (*v >= '0' && *v <= '9' && part[i] < 100000) {
            part[i] = part[i] * 10 + (unsigned long)(*v++ - '0');
        }
        v = *v == '.' ? v + 1 : 0;
    }
    return part[0] != 2 ? part[0] > 2 : part[1] != 7 ? part[1] > 7 : part[2] >= 2;
}

/* T is missing, with K there. FH2MP's copy sat where K had been, or an older platform built over K from the game's own database, and that
 * was removed: K goes back. But when K was back at T (B), and no older platform has built over it since (`built`: S is there; FH2MP
 * removes it when K goes back; `keeps`: gk_platform_keeps), the other mod's database itself was removed, so that mod is gone: K becomes
 * K', and isn't put back (online or offline). */
static int gk_removed(int back, int built, int keeps)
{
    return back && (keeps || !built);
}

/* Offline (no server database in the session), with K there, before the platform's database step:
 *   T missing                       put K back, unless gk_removed: then K becomes K'
 *   FH2MP's copy, or not whole      put K back (FH2MP undoes what it did: its copy sat where K had been)
 *   T has K's bytes                 K is back: keep K, as an older platform can still build over T or remove it in this launch
 *   T is a platform build           leave both (the platform builds again, and from 2.7.2 on keeps what it built on); an older platform
 *                                   built it over K, so K isn't back at T any more
 *   T is another mod's, not K       a newer database replaced it: K becomes K'
 * Once K is back (put back, or found there), B is written and S removed, so an older platform doesn't take K for its copy. */
enum { GK_LEAVE, GK_NOT_BACK, GK_PUT_BACK, GK_IS_BACK, GK_RETIRE_KEPT };

static int gk_offline(int have_t, int what, int same_as_kept, int back, int built, int keeps)
{
    if (!have_t) {
        return gk_removed(back, built, keeps) ? GK_RETIRE_KEPT : GK_PUT_BACK;
    }
    if (what == GK_FH2MP || what == GK_BROKEN) {
        return GK_PUT_BACK;
    }
    if (same_as_kept) {
        return GK_IS_BACK;
    }
    if (what == GK_PLATFORM) {
        return keeps ? GK_LEAVE : GK_NOT_BACK;
    }
    return GK_RETIRE_KEPT;
}

/* R's lines, "size date" in decimal: the last GK_PLACED_MAX that read, oldest first. Anything else is skipped. -> how many. */
static int gk_placed_parse(const char *s, int n, gk_stamp *list)
{
    int i = 0, count = 0;

    while (i < n) {
        unsigned long long v[2] = {0, 0};
        int k, ok = 1;
        for (k = 0; k < 2 && ok; k++) {
            int digits = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9' && digits < 19) {
                v[k] = v[k] * 10 + (unsigned)(s[i++] - '0');
                digits++;
            }
            if (!digits) {
                ok = 0;
            } else if (k == 0) {
                ok = i < n && s[i] == ' ';
                i += ok;
            } else {
                ok = i == n || s[i] == '\r' || s[i] == '\n';
            }
        }
        while (i < n && s[i++] != '\n') {   /* to the next line */
        }
        if (!ok) {
            continue;
        }
        if (count == GK_PLACED_MAX) {
            for (k = 1; k < GK_PLACED_MAX; k++) {
                list[k - 1] = list[k];
            }
            count--;
        }
        list[count].size = v[0];
        list[count].time = v[1];
        count++;
    }
    return count;
}

/* Adds `s` as the newest entry (an equal one moves to the end), keeping the last GK_PLACED_MAX. -> the new count. */
static int gk_placed_add(gk_stamp *list, int n, const gk_stamp *s)
{
    int i, j;

    for (i = j = 0; i < n; i++) {
        if (list[i].size != s->size || list[i].time != s->time) {
            list[j++] = list[i];
        }
    }
    if (j == GK_PLACED_MAX) {
        for (i = 1; i < GK_PLACED_MAX; i++) {
            list[i - 1] = list[i];
        }
        j--;
    }
    list[j++] = *s;
    return j;
}

/* R's text for `list`, into out (GK_PLACED_MAX lines fit in 200 bytes). -> its length. */
static int gk_placed_format(const gk_stamp *list, int n, char *out)
{
    int i, k, len = 0;

    for (i = 0; i < n; i++) {
        for (k = 0; k < 2; k++) {
            unsigned long long v = k ? list[i].time : list[i].size;
            char d[20];
            int m = 0;
            do {
                d[m++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (m) {
                out[len++] = d[--m];
            }
            out[len++] = k ? '\n' : ' ';
        }
    }
    out[len] = 0;
    return len;
}
