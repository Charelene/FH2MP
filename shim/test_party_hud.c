/* Party HUD past 12 members (src/party_hud.inc): the production hooks against stand-ins for the game's functions, which record what they are given.
 * The layout (rows at +0x130, count at +0x1ac), the two call sites and the prologues come from the decompile; whether the HUD template shows more
 * than 12 rows is not something this can say, nor is it a check against a running game. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char g_raw[1024];
static unsigned char *g_img;
static int g_hooks, g_call_hooks;

static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void log_dec(unsigned long v) { char b[16]; wsprintfA(b, "%lu", v); log_raw(b); }
static void mem_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static unsigned char *find_fh2_image(void) { return g_img; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag;
    if (!memcmp(img + rva, expect, n)) return 1;
    log_raw("mismatch: ");
    log_raw(what);
    log_raw("\n");
    return 0;
}

/* the game's functions */
#define MAX_CALLS 64
static struct { void *component; void **slot; void *item_at_call; void *member; char flag; } g_writes[MAX_CALLS];
static int g_nwrites;
static void *g_board_rows[64];
static int g_board_count, g_board_calls;
static void *g_board_mini;
static int g_frame_count_seen, g_frame_calls;
static unsigned char g_items[64][16];
static int g_nitems;

static void fake_writer(void *component, void **slot, void *member, char flag)
{
    g_writes[g_nwrites].component = component;
    g_writes[g_nwrites].slot = slot;
    g_writes[g_nwrites].item_at_call = *slot;
    g_writes[g_nwrites].member = member;
    g_writes[g_nwrites].flag = flag;
    g_nwrites++;
}
static void fake_board(void *mini, void **rows, int count)
{
    int i;
    g_board_mini = mini;
    g_board_count = count;
    g_board_calls++;
    for (i = 0; i < count; i++) g_board_rows[i] = rows[i];
}
static void fake_frame(void *component, void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    g_frame_count_seen = *(int *)((unsigned char *)component + 0x1ac);
    g_frame_calls++;
}
static void **fake_item(void **out, void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    *out = g_items[g_nitems++];
    return out;
}

static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name; (void)detour;
    g_hooks++;
    *original = (unsigned char *)target - g_img == 0x78bbd0 ? (void *)fake_frame : NULL;
    return *original != NULL;
}
static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    unsigned rva = (unsigned)((unsigned char *)site - g_img);
    (void)name; (void)detour;
    g_call_hooks++;
    *original = rva == 0x78be88 ? (void *)fake_writer : rva == 0x78beb0 ? (void *)fake_board : NULL;
    return *original != NULL;
}

#include "src/party_hud.inc"

static int has(const char *s, const char *part) { return strstr(s, part) != NULL; }

int main(void)
{
    static const unsigned char writer_p[] = {0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44};
    static const unsigned char board_p[] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b};
    static const unsigned char item_p[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48};
    static const unsigned char frame_p[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48};
    static const unsigned char writer_before[] = {0x48, 0x83, 0xc2, 0x26, 0x48, 0x8d, 0x14, 0xd7, 0x4c, 0x8b, 0x03, 0x48, 0x8b, 0xcf};
    static const unsigned char board_before[] = {0x44, 0x8b, 0x87, 0xac, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x8f, 0x28, 0x01, 0x00, 0x00};
    unsigned char component[0x400], other[0x400];
    void **rows = (void **)(component + 0x130);
    int rel, i, k;

    g_img = VirtualAlloc(NULL, 0x1000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_img);

    /* other bytes: nothing is hooked, and the log says which function it was */
    assert(patch_party_hud() == 0 && g_hooks == 0 && g_call_hooks == 0 && has(g_raw, "mismatch: per-row writer"));
    memcpy(g_img + 0x785d30, writer_p, 16);
    memcpy(g_img + 0xabf2d0, board_p, 16);
    memcpy(g_img + 0x7389d0, item_p, 16);
    memcpy(g_img + 0x78bbd0, frame_p, 16);
    memcpy(g_img + 0x78be88 - sizeof writer_before, writer_before, sizeof writer_before);
    memcpy(g_img + 0x78beb0 - sizeof board_before, board_before, sizeof board_before);
    g_img[0x78be88] = g_img[0x78beb0] = 0xe8;
    rel = 0x785d30 + 0x10 - (0x78be88 + 5);               /* the writer call must go to +785d30 */
    memcpy(g_img + 0x78be88 + 1, &rel, 4);
    rel = 0xabf2d0 - (0x78beb0 + 5);
    memcpy(g_img + 0x78beb0 + 1, &rel, 4);
    g_raw[0] = 0;
    assert(patch_party_hud() == 0 && g_hooks == 0 && has(g_raw, "per-row write call in the party HUD rebuild"));
    rel = 0x785d30 - (0x78be88 + 5);
    memcpy(g_img + 0x78be88 + 1, &rel, 4);
    g_raw[0] = 0;
    assert(patch_party_hud() == 1);
    /* fake_item stands in for the factory the installer points at game+0x7389d0 */
    g_party_new_item = fake_item;
    assert(g_hooks == 1 && g_call_hooks == 2 && has(g_raw, "party list guarded, up to 36 rows"));

    /* up to 12 members: the native function gets the native slot, untouched */
    memset(component, 0, sizeof component);
    for (i = 0; i < 12; i++) rows[i] = &g_items[60 + i % 4][0] + i;
    for (i = 0; i < 12; i++) party_row(component, &rows[i], (void *)(ULONG_PTR)(1000 + i), (char)(i < 5));
    assert(g_nwrites == 12 && g_nitems == 0);
    for (i = 0; i < 12; i++) assert(g_writes[i].slot == &rows[i] && g_writes[i].member == (void *)(ULONG_PTR)(1000 + i) && g_writes[i].flag == (i < 5));

    /* member 13 and 14: rows of ours, made once, not component+0x190 where the vector header is; the member and the flag go through */
    g_nwrites = 0;
    party_row(component, &rows[12], (void *)(ULONG_PTR)2012, 0);
    party_row(component, &rows[13], (void *)(ULONG_PTR)2013, 1);
    assert(g_nitems == 2 && g_nwrites == 2);
    for (k = 0; k < 2; k++) {
        assert(g_writes[k].slot != &rows[12 + k] && !((unsigned char *)g_writes[k].slot >= component && (unsigned char *)g_writes[k].slot < component + sizeof component));
        assert(g_writes[k].item_at_call == g_items[k] && g_writes[k].member == (void *)(ULONG_PTR)(2012 + k) && g_writes[k].flag == k);
    }
    /* a second rebuild writes the same rows, it makes no new ones */
    g_nwrites = 0;
    party_row(component, &rows[12], (void *)(ULONG_PTR)3012, 0);
    assert(g_nitems == 2 && g_writes[0].item_at_call == g_items[0]);
    assert(has(g_raw, "more than 12 members, rows from 13 on"));

    /* past the maximum: no write, no row */
    g_nwrites = 0;
    party_row(component, &rows[g_party_max], (void *)1, 0);
    party_row(component, &rows[40], (void *)1, 0);
    assert(g_nwrites == 0 && g_nitems == 2);

    /* the list handed to the mini leaderboard: 12 native rows then ours, in order */
    g_board_calls = 0;
    party_board((void *)0x77, rows, 8);
    assert(g_board_calls == 1 && g_board_count == 8 && g_board_rows[7] == rows[7] && g_board_mini == (void *)0x77);
    party_board((void *)0x77, rows, 14);
    assert(g_board_count == 14);
    for (i = 0; i < 12; i++) assert(g_board_rows[i] == rows[i]);
    assert(g_board_rows[12] == g_items[0] && g_board_rows[13] == g_items[1]);
    /* a count that is not backed by rows of ours shrinks to the rows there are */
    party_board((void *)0x77, rows, 20);
    assert(g_board_count == 14);
    /* more than the maximum is cut to it */
    for (i = 14; i < g_party_max; i++) party_row(component, &rows[i], (void *)1, 0);
    party_board((void *)0x77, rows, 200);
    assert(g_board_count == g_party_max);
    /* the maximum decides even when more rows exist (a lower FH2MP_PARTY_MAX than the rows already made), and a huge count cannot outrun the array */
    k = g_party_max;
    g_party_max = 20;
    party_board((void *)0x77, rows, 200);
    assert(g_board_count == 20);
    party_board((void *)0x77, rows, 0x7fffffff);
    assert(g_board_count == 20);
    g_party_max = k;
    /* a component we never built rows for gets the 12 native rows and nothing else */
    memset(other, 0, sizeof other);
    party_board((void *)0x77, (void **)(other + 0x130), 15);
    assert(g_board_count == 12);

    /* the per-frame update sees at most 12 and the count is back afterwards */
    *(int *)(component + 0x1ac) = 15;
    party_frame(component, NULL, NULL, NULL);
    assert(g_frame_calls == 1 && g_frame_count_seen == 12 && *(int *)(component + 0x1ac) == 15);
    *(int *)(component + 0x1ac) = 9;
    party_frame(component, NULL, NULL, NULL);
    assert(g_frame_calls == 2 && g_frame_count_seen == 9 && *(int *)(component + 0x1ac) == 9);

    /* FH2MP_PARTY_MAX: 12 to 48 is taken, anything else keeps what was set */
    g_party_max = 36;
    SetEnvironmentVariableA("FH2MP_PARTY_MAX", "20");
    party_max_from_env();
    assert(g_party_max == 20);
    SetEnvironmentVariableA("FH2MP_PARTY_MAX", "48");
    party_max_from_env();
    assert(g_party_max == 48);
    { static const char *const bad[] = {"11", "49", "abc", "2x", "", "99999999"};
      for (i = 0; i < 6; i++) {
          g_party_max = 30;
          SetEnvironmentVariableA("FH2MP_PARTY_MAX", bad[i]);
          party_max_from_env();
          assert(g_party_max == 30);
      } }

    printf("party hud: ok\n");
    return 0;
}
