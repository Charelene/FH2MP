/* Isolated tests of src/event_stock.inc: in an online event car select, a car with a part the server's list does not have is put to its stock
 * parts, stock tune and stock class/PI in the pending block; a clean car, a car whose stock parts are not the server's either, and an attribute
 * block of another car are left as they are. The game is never loaded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_log[16384];
static int read_game_memory(const void *p, void *out, SIZE_T n) { if ((UINT_PTR)p < 0x10000) return 0; memcpy(out, p, n); return 1; }
static unsigned char *find_fh2_image(void) { return NULL; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag; (void)what; (void)img; (void)rva; (void)expect; (void)n;
    return 0;
}
static int plat_hook(const char *name, void *target, void *detour, void **original) { (void)name; (void)target; (void)detour; (void)original; return 0; }
static void log_begin(void) { strcat(g_log, "|"); }
static void log_raw(const char *s) { strcat(g_log, s); }
static void log_dec(unsigned long v) { char b[24]; sprintf(b, "%lu", v); strcat(g_log, b); }
#include "src/picker_pi.inc"
/* what event_stock.inc takes from torque_load_guard.inc and carsel_sync.inc */
typedef void (*torque_stock_fn)(void *, void **, unsigned);
typedef void (*torque_parts_ctor_fn)(void *);
static torque_stock_fn g_torque_stock;
static torque_parts_ctor_fn g_torque_parts_ctor;
static void torque_stock_parts(void *db, unsigned car, unsigned parts[45]) { (void)db; (void)car; (void)parts; }
static volatile LONG64 g_cs_count_armed_at;
#include "src/event_stock.inc"

#define CAR 1005
static unsigned g_stock[45];
static int g_resolver_down;
static int fake_stock_parts(void *ctx, unsigned car, unsigned stock[45])
{
    (void)ctx;
    assert(car == CAR);
    if (g_resolver_down) return 0;
    memcpy(stock, g_stock, sizeof g_stock);
    return 1;
}

static unsigned char g_block[0x300];
static void make_block(unsigned engine)
{
    unsigned car = CAR, k, word;
    short id = CAR;
    double pi = 0.8;
    memset(g_block, 0xab, sizeof g_block);
    memcpy(g_block + ES_CAR, &car, 4);
    for (k = 0; k < PPI_SLOTS; k++) { word = PPI_NONE; memcpy(g_block + ES_PARTS + 4 * k, &word, 4); }
    memcpy(g_block + ES_PARTS, &engine, 4);
    word = 900; memcpy(g_block + ES_PARTS + 4 * 8, &word, 4);          /* tires: the server's */
    word = 7; memcpy(g_block + ES_WHEEL, &word, 4);
    memcpy(g_block + ES_ATTRS + PPI_OUT_ID, &id, 2);
    memcpy(g_block + ES_ATTRS + PPI_OUT_PI, &pi, 8);
    g_block[ES_ATTRS + PPI_OUT_CLASS] = 5;
}
static unsigned word_at(unsigned off) { unsigned w; memcpy(&w, g_block + off, 4); return w; }

int main(void)
{
    static const char list[] = "fh2mp-base-parts 1\np Engine 5\np Engine 12\np TireCompound 900\np TireCompound 901\nc 1005 3 512000\n";
    const char *why = NULL;
    ppi_ops ops = {ppi_base_exists, ppi_base_stock, NULL};
    es_ops es = {fake_stock_parts, NULL};
    unsigned k;
    double pi;

    assert(ppi_base_load_text(list, (unsigned)strlen(list), &why));
    for (k = 0; k < 45; k++) g_stock[k] = PPI_NONE;
    g_stock[2] = 5; g_stock[2 + 8] = 901;                                   /* stock engine 5, stock tires 901 */

    /* a clean car (the server's parts only) is not touched */
    make_block(12);
    assert(!es_stock_pending(g_block, &es, &ops) && word_at(ES_PARTS) == 12 && word_at(ES_WHEEL) == 7 && g_block[ES_ATTRS + PPI_OUT_CLASS] == 5);

    /* a car pack's engine 13: the whole car goes to stock, as the server repairs it */
    make_block(13);
    assert(es_stock_pending(g_block, &es, &ops));
    assert(word_at(ES_PARTS) == 5 && word_at(ES_PARTS + 4 * 8) == 901 && word_at(ES_WHEEL) == PPI_NONE);
    for (k = 0; k < ES_TUNE_DWORDS; k++) assert(word_at(ES_TUNE + 4 * k) == ES_STOCK_TUNE);
    assert(g_block[ES_TUNE + 4 * ES_TUNE_DWORDS] == 0xab);                   /* nothing past the tune */
    memcpy(&pi, g_block + ES_ATTRS + PPI_OUT_PI, 8);
    assert(g_block[ES_ATTRS + PPI_OUT_CLASS] == 3 && fabs(pi - 0.512) < 1e-6);
    assert(strstr(g_log, "eventstock: car 1005 races with its stock parts and tune (class 3, PI 0.5120) in this online event: slot 0 held part id 13"));

    /* the stock parts of the player's own database are not the server's: left as picked */
    g_stock[2] = 14;
    make_block(13);
    assert(!es_stock_pending(g_block, &es, &ops) && word_at(ES_PARTS) == 13 && strstr(g_log, "not the server's either"));
    g_stock[2] = 5;

    /* the resolver is unavailable / the attribute block is another car's: left as picked */
    g_resolver_down = 1;
    make_block(13);
    assert(!es_stock_pending(g_block, &es, &ops) && word_at(ES_PARTS) == 13 && strstr(g_log, "resolver is unavailable"));
    g_resolver_down = 0;
    make_block(13);
    { short other = 77; memcpy(g_block + ES_ATTRS + PPI_OUT_ID, &other, 2); }
    assert(!es_stock_pending(g_block, &es, &ops) && word_at(ES_PARTS) == 13 && strstr(g_log, "does not carry this car"));
    puts("event_stock: ok");
    return 0;
}
