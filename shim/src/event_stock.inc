/* FH2 1.0.0.64, a game that keeps its own car packs (online, its session names no database; the server's part list is loaded, picker_pi.inc): a car
 * picked in an online event car select (Free Roam event, Road Trip) whose parts are not all the server's races as the stock car, the way the game
 * server repairs such a car for everyone else (native/internal/game CheckParts, playersetup.StockCorrection: stock parts, stock tune, stock class
 * and PI). The garage car, its database row and the save are never written, so there is nothing to put back after the race.
 *
 * Where [C: decompile of the Ghidra export; docs in the investigation of 2026-11-08]. The online choices (ForzaPlayerChoices, vtable 0x2c5de38, one
 * per network player) hold the car being picked as a pending copy of the garage car's setup block (this+8 of the subobject at +0xae8: car id, header
 * +0x10, WheelStyle +0x14, the 43 part ids +0x18, tune +0xc4) and its attributes (+0x230, QueryCarAttributes' block: PI double +0x08, car id +0x1e,
 * class +0x22), both filled by SetCarFromId (+2a0dc0) from the garage car. Slot 59 of that subobject (+28d9a0) commits them: it copies the pending
 * block to +0x288 and the attributes to +0x4b0, then publishes car, WheelStyle, parts and tune to the synced PlayerChoices (helper_2aa550), which
 * is what the other players get. The race car is built from the committed block (PrepareCarForLoad +fadac0 via vfunc +0x188 = +292620). The
 * single-player choices (ForzaLocalPlayerChoices) have a NullSub there, so this only ever runs online. The stock tune is 44 floats of -1.0 at +0xc4
 * (FUN_140283150 and the copy in PrepareCarForLoad's ForceStock branch); the stock parts come from the game's own CarParts::ResolveStockParts
 * (+104d5f0, as torque_load_guard.inc calls it), checked against the server's list before they are used.
 *
 * This hooks slot 59 and edits the pending block before the game commits it, only while the event car select countdown runs (carsel_sync.inc hook C
 * ran within 3 s; Free Roam driving is left alone: the server repairs what the others see there). Anything unexpected leaves the car as the
 * player picked it, with one line: the database or the stock resolver is unavailable, the attribute block does not carry this car, the stock parts
 * the player's database gives are not the server's either.
 * Evidence: the data flow is confirmed from the decompile; that every Free Roam event and Road Trip car select commits through slot 59, and that
 * the game's performance check at car load (+f58a90) accepts the result, are NOT verified in a running game. FH2MP_NO_EVENTSTOCK=1 turns it off. */
#define ES_COMMIT_RVA 0x28d9a0u
#define ES_CAR 0x08u
#define ES_WHEEL 0x14u
#define ES_PARTS 0x18u
#define ES_TUNE 0xc4u
#define ES_TUNE_DWORDS 44
#define ES_ATTRS 0x230u
#define ES_SELECT_MS 3000u
#define ES_STOCK_TUNE 0xbf800000u      /* -1.0f: "stock" for every tune value */

typedef void (*es_commit_fn)(void *self);
static es_commit_fn g_es_orig;
static volatile LONG g_es_swapped, g_es_declined;

/* What the swap needs from the game; the hook fills it, the test fakes it. */
typedef struct {
    /* 1 with the car's stock setup (header, WheelStyle, 43 part ids), 0 when the game cannot give one */
    int (*stock_parts)(void *ctx, unsigned car, unsigned stock[45]);
    void *ctx;
} es_ops;

static void es_log_decline(const char *what, unsigned car)
{
    LONG n = InterlockedIncrement(&g_es_declined);
    if (n > 5 && n % 100) {
        return;
    }
    log_begin();
    log_raw("eventstock: car ");
    log_dec(car);
    log_raw(" left as the player picked it: ");
    log_raw(what);
    log_raw(" (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

/* The decision and the edit on a pending block `self` (writable). -> 1 when the car was put to stock. */
static int es_stock_pending(unsigned char *self, const es_ops *es, const ppi_ops *ops)
{
    unsigned car, parts[PPI_SLOTS], stock[45], klass = 0, i;
    float pi = 0;
    ppi_verdict v, sv;
    short attr_id;
    double dpi;
    LONG n;

    memcpy(&car, self + ES_CAR, sizeof car);
    memcpy(parts, self + ES_PARTS, sizeof parts);
    v = ppi_check_parts(parts, ops);
    if (v.kind == PPI_CLEAN) {
        return 0;
    }
    if (v.kind == PPI_NO_ANSWER) {
        es_log_decline("the server's part list could not say", car);
        return 0;
    }
    memcpy(&attr_id, self + ES_ATTRS + PPI_OUT_ID, sizeof attr_id);
    if (attr_id != (short)car) {
        es_log_decline("the attribute block does not carry this car, so its layout is not as expected", car);
        return 0;
    }
    if (ops->stock(ops->ctx, car, &klass, &pi) <= 0 || klass > 255 || !ppi_pi_usable((double)pi)) {
        es_log_decline("the server's list has no stock class and PI for the car", car);
        return 0;
    }
    if (!es->stock_parts(es->ctx, car, stock)) {
        es_log_decline("the game's stock part resolver is unavailable", car);
        return 0;
    }
    sv = ppi_check_parts(stock + 2, ops);
    if (sv.kind != PPI_CLEAN) {
        es_log_decline("the stock parts this game's database gives are not the server's either", car);
        return 0;
    }
    memcpy(self + ES_WHEEL, &stock[1], sizeof stock[1]);
    memcpy(self + ES_PARTS, stock + 2, sizeof parts);
    for (i = 0; i < ES_TUNE_DWORDS; i++) {
        unsigned t = ES_STOCK_TUNE;
        memcpy(self + ES_TUNE + 4 * i, &t, sizeof t);
    }
    self[ES_ATTRS + PPI_OUT_CLASS] = (unsigned char)klass;
    dpi = (double)pi;
    memcpy(self + ES_ATTRS + PPI_OUT_PI, &dpi, sizeof dpi);
    n = InterlockedIncrement(&g_es_swapped);
    if (n <= 20 || n % 50 == 0) {
        log_begin();
        log_raw("eventstock: car ");
        log_dec(car);
        log_raw(" races with its stock parts and tune (class ");
        log_dec(klass);
        log_raw(", PI ");
        ppi_log_pi((double)pi);
        log_raw(") in this online event: slot ");
        log_dec((unsigned long)v.slot);
        log_raw(" held part id ");
        log_dec(v.id);
        log_raw(v.kind == PPI_WRONG_TABLE ? ", a row of another part table" : ", which the server's database does not have");
        log_raw("; the garage car is unchanged (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
    return 1;
}

static int es_game_stock_parts(void *ctx, unsigned car, unsigned stock[45])
{
    void *holder = NULL, *db = NULL;
    (void)ctx;
    if (!g_torque_stock || !g_torque_parts_ctor || !g_ppi_img ||
        !read_game_memory(g_ppi_img + PPI_HOLDER_RVA, &holder, sizeof holder) || !holder ||
        !read_game_memory((const unsigned char *)holder + PPI_HOLDER_DB_OFFSET, &db, sizeof db) || !db) {
        return 0;
    }
    torque_stock_parts(db, car, stock);
    return 1;
}

static void es_hook(void *self)
{
    unsigned char *s = (unsigned char *)self;
    ULONGLONG now = GetTickCount64(), armed = (ULONGLONG)g_cs_count_armed_at;
    unsigned char probe;

    if (s && g_ppi_base_ready && armed && now - armed <= ES_SELECT_MS && read_game_memory(s + ES_ATTRS + PPI_OUT_SIZE - 1, &probe, 1)) {
        ppi_ops ops = {ppi_base_exists, ppi_base_stock, NULL};
        es_ops es = {es_game_stock_parts, NULL};
        es_stock_pending(s, &es, &ops);
    }
    g_es_orig(self);
}

static int patch_event_stock(void)
{
    static const unsigned char prologue[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x51, 0x08, 0x48, 0x8b, 0xd9, 0x48, 0x81, 0xc1};
    unsigned char *img = find_fh2_image();

    if (!g_ppi_base_ready) {
        return 0;               /* the server's database is in place (or offline): every part is the server's */
    }
    if (!img || !code_is("eventstock", "ForzaPlayerChoices commit (slot 59)", img, ES_COMMIT_RVA, prologue, sizeof prologue)) {
        return 0;
    }
    if (!plat_hook("eventstock: online car commit", img + ES_COMMIT_RVA, (void *)es_hook, (void **)&g_es_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: eventstock: in an online event car select, a car with parts the server's database does not have races as the stock car (game+0x28d9a0)\n");
    return 1;
}
