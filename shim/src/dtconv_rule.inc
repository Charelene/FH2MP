/* The pure decision of dtconv_guard.inc (tested natively by tests/test_dtconv.sh): does a converter's value name a car ordinal that has no
 * car data? The game's functions come in through dtc_game so the test can give fakes. */
typedef struct {
    void *(*int_type)(void *);                 /* game+0x15c8b20: AVUI's int type (called with ecx = 0, as the converter does) */
    void *(*int64_type)(void *);               /* game+0x15c94b0: AVUI's 64-bit int type */
    unsigned char (*is_a)(void *, void *);     /* game+0x14530e0: is this boxed value of that type */
    void *(*car)(void *, unsigned);            /* game+0x26f900: a car table's entry for an ordinal, NULL past its end */
    void *const *service;                      /* game+0x34aba60: the service provider; its +0x3a0 is the car table (game+0x21dec0) */
} dtc_game;

/* 1 when `value` is a boxed int or int64 whose low 32 bits (the converter reads [value+0x28] either way) are a car ordinal with no entry
 * in the car table: the converter would read through the NULL it gets back. 0 for anything else, including when the table cannot be
 * reached: then the game's own code runs as before. */
static int dtc_car_missing(const dtc_game *g, void *value)
{
    void *sp, *table;
    if (!value || !g->service) {
        return 0;
    }
    if (!g->is_a(value, g->int_type(NULL)) && !g->is_a(value, g->int64_type(NULL))) {
        return 0;
    }
    sp = *g->service;
    table = sp ? *(void **)((char *)sp + 0x3a0) : NULL;
    if (!table) {
        return 0;
    }
    return g->car(table, *(const unsigned *)((const char *)value + 0x28)) == NULL;
}
