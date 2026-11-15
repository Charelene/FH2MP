/* FH2 1.0.0.64: a "Modded cars" row on the session leader's Event Options screen, so the leader says per event whether car packs' own cars may
 * race (today they never do: game.CustomCar on the server, "pickerpi: custom cars out of online events" here).
 *
 * How the screen gets its rows [C: disassembly of the exe; S: seen in a game 2026-11-15 with a probe that added two rows, started two events
 * with them and opened the screen twice, beside FH2 Extra Options 0.6.1]:
 *   - Event Options is OnlineEventOptions (Scenes/Anthem/A_OnlineEventOptions.xaml, vftable +2e0cbd8). One function, +d78e50, makes all of its
 *     rows each time the screen opens; which rows depends on the event (five for a point-to-point race: Car Class, Teams, Weather, Time of Day,
 *     Collisions; a circuit adds Laps). It is called from one place, +d8200d.
 *   - A row is an OnlineEventOptionsItem (vftable +2e0c898, 0xa8 bytes) in the collection at page+0x410: Title +0x70, SelectedIndex +0x78, the
 *     choices (Data) +0x80, and a std::function<void(NetworkRouteActivationComponent::LaunchOptions &, int)> at +0x88.
 *     +d709f0 (out, &title, selected, &choices, &function) makes one, +d6e650 (collection, row) adds it.
 *   - The title is a String made from a display string: MakeDisplayString +323310 (out, text, -1), then +140af10 (out, &display string).
 *   - The choices are a list like the game's EnumDataSource<PlaygroundTypeOptions> (constructor +d63cc0): 0x58 bytes allocated (+14424e0) as
 *     the type +1476110 gives, the base constructor +aefd60, five vftables (+0, +0x30, +0x38, +0x40, +0x48), then one entry per choice, made by
 *     +c65b90 (out, &value, &display string) and put in by +b52170 (list, index, entry).
 *   - When the leader starts the event, +d6f470 calls every row's function with the row's SelectedIndex, and stops the game (bad_function_call)
 *     at a row whose function is empty. So the row carries one: the game's own wrapper for a plain function pointer (vftable +2e105c0, the
 *     pointer at +8; its call +17f1410 is fn(LaunchOptions &, index)), laid out in the std::function's own space with the pointer to it at
 *     +0x18, exactly as the builder lays out its own at +d78f69. The game copies and destroys it with its own code.
 *   - Objects are counted at +0x10 and freed by +257730 when the count reaches 0; a display string is released through its vftable (+0x18).
 *
 * FH2 Extra Options (0.6.x) replaces the builder's one call (+d8200d, its "offline_events_division_thunk") with a function that runs the builder
 * and then changes rows itself, with these same game functions. So the builder is hooked here, not its call: the row is added right after the
 * game's own rows and before whatever Extra Options does, and neither mod's hook overlaps the other's.
 *
 * What this does: after the game has made its rows, in a network session whose server answers event options (eventopts_sync.inc: not a Road
 * Trip session, not a server without them), it adds "Modded cars" with "Not allowed" (the default, every time) and "Allowed". When the event
 * is started the row's function hands the leader's choice to eventopts_sync.inc, which says it to the server for a few seconds; the server
 * makes it the event's setting and tells every game of the session. FH2MP_NO_EVENTOPTS=1 turns the row and the channel off.
 *
 * Safe by default: every function called and the two vftable sets are byte-checked before the hook goes in, and any mismatch leaves the screen
 * as the game makes it. At a call the page's collection is read through read_game_memory first. The game's functions are called as the builder
 * calls them, on the thread the builder runs on. */
#define EVO_BUILDER_RVA 0xd78e50u        /* OnlineEventOptions: make the rows */
#define EVO_MAKE_ROW_RVA 0xd709f0u       /* (out, &title, selected, &choices, &function) */
#define EVO_ADD_ROW_RVA 0xd6e650u        /* (collection, row) */
#define EVO_MAKE_STRING_RVA 0x323310u    /* MakeDisplayString(out, text, length) */
#define EVO_TEXT_FROM_RVA 0x140af10u     /* (out, &display string): the String a row or a choice shows */
#define EVO_FREE_RVA 0x257730u           /* an object whose count reached 0 */
#define EVO_LIST_TYPE_RVA 0x1476110u     /* the type the choice lists are allocated as */
#define EVO_ALLOC_RVA 0x14424e0u         /* alloc(size, type) */
#define EVO_LIST_BASE_RVA 0xaefd60u      /* the choice lists' base constructor */
#define EVO_ENTRY_RVA 0xc65b90u          /* (out, &value, &display string): one choice */
#define EVO_INSERT_RVA 0xb52170u         /* (list, index, choice) */
#define EVO_LIST_CTOR_LEA_RVA 0xd63cddu  /* EnumDataSource<PlaygroundTypeOptions>'s constructor loading its first vftable */
#define EVO_FN_VTABLE_RVA 0x2e105c0u     /* std::function's wrapper for a plain function (LaunchOptions &, int) */
#define EVO_FN_CALL_RVA 0x17f1410u       /* its call, the third vftable entry */
#define EVO_START_RVA 0xd6f4c7u          /* the start of the event reading a row's SelectedIndex and function */
#define EVO_PAGE_ROWS 0x410u
#define EVO_LIST_SIZE 0x58u
#define EVO_COUNT 0x10u

static const unsigned g_evo_list_vtable_rva[5] = {0x2e0e750u, 0x2e0e7c8u, 0x2e0e828u, 0x2e0e840u, 0x2e0e860u};
static const unsigned g_evo_list_vtable_at[5] = {0, 0x30, 0x38, 0x40, 0x48};

/* The game's functions this calls (filled from the image once the bytes are checked; a test fills in its own). */
struct evo_game {
    void **(*make_string)(void **out, const char *text, int length);
    void **(*text_from)(void **out, void **display);
    void (*free_object)(void *object);
    void *(*list_type)(void);
    void *(*alloc)(unsigned long long size, void *type);
    void *(*list_base)(void *list);
    void **(*make_entry)(void **out, int *value, void **display);
    void (*insert)(void *list, int index, void *entry);
    void **(*make_row)(void **out, void **title, int selected, void **choices, void *function);
    int (*add_row)(void *collection, void *row);
    void *list_vtable[5];
    void *fn_vtable;
};

/* std::function<void(LaunchOptions &, int)> holding a plain function: the wrapper in the function's own space, the pointer to it at +0x18. */
struct evo_fn {
    void *vtable;
    void (*fn)(void *launch_options, int index);
    void *unused;
    void *impl;
};

typedef void *(*evo_builder_fn)(void *page, void *b, void *c, void *d);

static struct evo_game g_evo;
static evo_builder_fn g_evo_builder_orig;
static const unsigned char *g_evo_img;
static volatile LONG g_evo_added, g_evo_started, g_evo_no_rows;

static void evo_release(void *object)
{
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + EVO_COUNT)) == 0) {
        g_evo.free_object(object);
    }
}

static void evo_release_string(void *display)
{
    if (display) {
        (*(void (***)(void *))display)[3](display);
    }
}

/* The row's function: the game calls it with the row's SelectedIndex when the leader starts the event. */
static void evo_on_start(void *launch_options, int index)
{
    LONG n = InterlockedIncrement(&g_evo_started);
    (void)launch_options;
    evo_leader_chose(index == 1);
    if (n <= 20 || n % 100 == 0) {
        log_begin();
        log_raw("eventopts: the leader started an event with Modded cars ");
        log_raw(index == 1 ? "allowed" : "not allowed");
        log_raw(" (#");
        log_dec((unsigned long)n);
        log_raw("); the server is told\n");
    }
}

/* A choice list of `n` texts, made as EnumDataSource<PlaygroundTypeOptions>'s constructor makes its own. -> the list, holding one count. */
static void *evo_choices(const char *const *texts, int n)
{
    void *list = g_evo.alloc(EVO_LIST_SIZE, g_evo.list_type());
    int i;
    if (!list) {
        return NULL;
    }
    g_evo.list_base(list);
    for (i = 0; i < 5; i++) {
        *(void **)((unsigned char *)list + g_evo_list_vtable_at[i]) = g_evo.list_vtable[i];
    }
    for (i = 0; i < n; i++) {
        int value = i;
        void *name = NULL, *entry = NULL;
        g_evo.make_string(&name, texts[i], -1);
        g_evo.make_entry(&entry, &value, &name);
        if (entry) {
            g_evo.insert(list, i, entry);
        }
        evo_release(entry);
        evo_release_string(name);
    }
    return list;
}

/* Adds the row to the page's rows. -> 1 added. */
static int evo_add_row(void *page)
{
    static const char *const choices[2] = {"Not allowed", "Allowed"};
    void *collection = NULL, *list, *name = NULL, *title = NULL, *row = NULL;
    struct evo_fn fn;

    if (!page || !read_game_memory((const unsigned char *)page + EVO_PAGE_ROWS, &collection, sizeof collection) || !collection) {
        if (InterlockedIncrement(&g_evo_no_rows) == 1) {
            log_begin();
            log_raw("eventopts: the Event Options page has no row collection where it is expected, the row is not added\n");
        }
        return 0;
    }
    list = evo_choices(choices, 2);
    if (!list) {
        return 0;
    }
    g_evo.make_string(&name, "Modded cars", -1);
    g_evo.text_from(&title, &name);
    fn.vtable = g_evo.fn_vtable;
    fn.fn = evo_on_start;
    fn.unused = NULL;
    fn.impl = &fn;
    g_evo.make_row(&row, &title, 0, &list, &fn);   /* takes the function out of `fn` */
    if (row) {
        g_evo.add_row(collection, row);
    }
    evo_release(row);
    evo_release(title);
    evo_release_string(name);
    evo_release(list);
    return row != NULL;
}

/* 1 when this game is in a network session (the test has its own). */
static int evo_in_session(const unsigned char *img);

static void *evo_builder_hook(void *page, void *b, void *c, void *d)
{
    void *ret = g_evo_builder_orig(page, b, c, d);
    if (evo_server_answers() && evo_in_session(g_evo_img) && evo_add_row(page)) {
        LONG n = InterlockedIncrement(&g_evo_added);
        if (n <= 20 || n % 100 == 0) {
            log_begin();
            log_raw("eventopts: Event Options opened, the Modded cars row is on it (#");
            log_dec((unsigned long)n);
            log_raw(")\n");
        }
    }
    return ret;
}

#ifndef EVO_TEST
static int evo_in_session(const unsigned char *img)
{
    return ppi_in_session(img);
}

static int patch_event_options(void)
{
    static const struct { unsigned rva; unsigned char n; const char *what; unsigned char code[17]; } checks[] = {
        {EVO_BUILDER_RVA, 16, "OnlineEventOptions row builder", {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xf0}},
        {EVO_MAKE_ROW_RVA, 16, "make a row", {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30}},
        {EVO_ADD_ROW_RVA, 16, "add a row", {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x50, 0x4c, 0x8b, 0x09, 0x4c, 0x8b, 0xc2}},
        {EVO_MAKE_STRING_RVA, 15, "MakeDisplayString", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff}},
        {EVO_TEXT_FROM_RVA, 16, "a row's text", {0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe}},
        {EVO_FREE_RVA, 16, "free an object", {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xb2, 0xa2, 0x1f, 0x01, 0x48, 0x85}},
        {EVO_LIST_TYPE_RVA, 8, "the choice list type", {0x48, 0x8d, 0x05, 0x49, 0x10, 0x21, 0x02, 0xc3}},
        {EVO_ALLOC_RVA, 16, "alloc", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x05, 0x6f, 0x97, 0x26}},
        {EVO_LIST_BASE_RVA, 16, "the choice list base constructor", {0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24}},
        {EVO_ENTRY_RVA, 16, "make a choice", {0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe}},
        {EVO_INSERT_RVA, 16, "put a choice in", {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0x98, 0xfe}},
        {EVO_LIST_CTOR_LEA_RVA, 7, "the choice list's first vftable", {0x48, 0x8d, 0x05, 0x6c, 0xaa, 0x0a, 0x02}},
        {EVO_FN_CALL_RVA, 16, "the function wrapper's call", {0x48, 0x8b, 0xc2, 0x41, 0x8b, 0x10, 0x4c, 0x8b, 0xc9, 0x48, 0x8b, 0xc8, 0x49, 0xff, 0x61, 0x08}},
        {EVO_START_RVA, 17, "the start of the event", {0x8b, 0x42, 0x78, 0x89, 0x44, 0x24, 0x70, 0x48, 0x8b, 0x8a, 0xa0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9}},
    };
    unsigned char *img = find_fh2_image();
    void *call = NULL;
    unsigned i;

    if (!img) {
        return 0;
    }
    for (i = 0; i < sizeof checks / sizeof checks[0]; i++) {
        if (!code_is("eventopts", checks[i].what, img, checks[i].rva, checks[i].code, checks[i].n)) {
            return 0;
        }
    }
    if (!read_game_memory(img + EVO_FN_VTABLE_RVA + 2 * sizeof(void *), &call, sizeof call) || call != (void *)(img + EVO_FN_CALL_RVA)) {
        log_begin();
        log_raw("eventopts: not patched, the function wrapper's vftable at game+0x");
        log_hex(EVO_FN_VTABLE_RVA, 8);
        log_raw(" does not hold its call\n");
        return 0;
    }
    g_evo.make_string = (void **(*)(void **, const char *, int))(void *)(img + EVO_MAKE_STRING_RVA);
    g_evo.text_from = (void **(*)(void **, void **))(void *)(img + EVO_TEXT_FROM_RVA);
    g_evo.free_object = (void (*)(void *))(void *)(img + EVO_FREE_RVA);
    g_evo.list_type = (void *(*)(void))(void *)(img + EVO_LIST_TYPE_RVA);
    g_evo.alloc = (void *(*)(unsigned long long, void *))(void *)(img + EVO_ALLOC_RVA);
    g_evo.list_base = (void *(*)(void *))(void *)(img + EVO_LIST_BASE_RVA);
    g_evo.make_entry = (void **(*)(void **, int *, void **))(void *)(img + EVO_ENTRY_RVA);
    g_evo.insert = (void (*)(void *, int, void *))(void *)(img + EVO_INSERT_RVA);
    g_evo.make_row = (void **(*)(void **, void **, int, void **, void *))(void *)(img + EVO_MAKE_ROW_RVA);
    g_evo.add_row = (int (*)(void *, void *))(void *)(img + EVO_ADD_ROW_RVA);
    for (i = 0; i < 5; i++) {
        g_evo.list_vtable[i] = img + g_evo_list_vtable_rva[i];
    }
    g_evo.fn_vtable = img + EVO_FN_VTABLE_RVA;
    g_evo_img = img;
    if (!plat_hook("eventopts: the Event Options rows", img + EVO_BUILDER_RVA, (void *)evo_builder_hook, (void **)&g_evo_builder_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: eventopts: the session leader's Event Options get a Modded cars row while the server answers event options (game+0xd78e50)\n");
    return 1;
}
#endif
