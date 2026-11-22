/* Isolated tests of src/activity_report.inc's file text (encore-activity.json, read by Encore's encore-fh2 activity.rs): no game is
 * loaded, the hooks are left out. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define AR_NO_HOOKS
#include "src/activity_report.inc"

static void text_is(const ar_state *s, const char *want)
{
    char out[160];
    unsigned n = ar_format(out, sizeof out, s);
    if (strcmp(out, want) != 0 || n != strlen(want)) {
        printf("got  %s\nwant %s\n", out, want);
        assert(0);
    }
}

int main(void)
{
    ar_state s = {2, 542, 1, -1234, 5678, 1}, t;
    /* the table is the game's, in its order (0x1e63f40) */
    assert(strcmp(g_ar_names[0], "RP_INVALID") == 0);
    assert(strcmp(g_ar_names[12], "RP_PAINTSHOP") == 0);
    assert(strcmp(g_ar_names[23], "RP_ISLAND_FREEROAM") == 0);
    assert(strcmp(g_ar_names[37], "RP_ISLAND_CAR_MEET") == 0);

    text_is(&s, "{\"v\":1,\"game\":\"fh2\",\"rp\":\"RP_FREEROAM\",\"track\":542,\"x\":-1234,\"z\":5678,\"online\":1}\n");
    s.has_pos = 0;
    s.value = 13;
    s.online = 0;
    text_is(&s, "{\"v\":1,\"game\":\"fh2\",\"rp\":\"RP_GARAGE\",\"track\":542,\"online\":0}\n");
    s.value = 99;
    text_is(&s, "{\"v\":1,\"game\":\"fh2\",\"rp\":\"RP_INVALID\",\"track\":542,\"online\":0}\n");
    {
        ar_state big = {0, 0, 1, -2147483647L - 1, 2147483647L, 0};
        text_is(&big, "{\"v\":1,\"game\":\"fh2\",\"rp\":\"RP_INVALID\",\"track\":0,\"x\":-2147483648,\"z\":2147483647,\"online\":0}\n");
    }
    {
        /* a short buffer is cut, never overrun */
        char small[10];
        memset(small, 'X', sizeof small);
        ar_format(small, sizeof small, &s);
        assert(small[9] == 0 && strlen(small) == 9);
    }

    s = (ar_state){2, 542, 1, 100, 100, 1};
    t = s;
    assert(!ar_changed(&s, &t));
    t.x = 140;
    assert(!ar_changed(&s, &t));
    t.x = 151;
    assert(ar_changed(&s, &t));
    t = s;
    t.z = 49;
    assert(ar_changed(&s, &t));
    t = s;
    t.value = 15;
    assert(ar_changed(&s, &t));
    t = s;
    t.has_pos = 0;
    assert(ar_changed(&s, &t));

    assert(ar_metres(12.4f) == 12 && ar_metres(12.6f) == 13 && ar_metres(-12.6f) == -13);
    assert(ar_metres(1.0e9f) == 0);
    {
        volatile float zero = 0.0f;
        assert(ar_metres(zero / zero) == 0);
    }
    printf("activity: ok\n");
    return 0;
}
