/* Isolated tests of the Modded cars event option: src/event_options.inc (the row on the Event Options screen, with stand-ins for the game's
 * functions), src/eventopts_wire.h (the datagrams, against the vectors the server's tests froze) and the state of src/eventopts_sync.inc (what is
 * said to the server and when, what a reply and its absence do). Checked: the row is made the way the game's builder makes its own (title, two
 * choices, a function the game can call), it goes into the page's collection after the game's rows, every object the row code made is given
 * back exactly once, the leader's choice is said for a few seconds after the game calls the row's function, and nothing is added outside a
 * session, against a server that does not answer, or on a page without a collection; the row's setting is said while the screen is open and
 * a change of our row (and only ours) is noticed; an invite toast gets its wrench exactly when the server's newest answer says so. The game is
 * never loaded, no network call is made.
 *   test.exe <request vector> <reply vector, event not modded> <reply vector, event modded> <reply vector, one may be coming> */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_log[16384];
static int read_game_memory(const void *p, void *out, SIZE_T n) { if ((UINT_PTR)p < 0x10000) return 0; memcpy(out, p, n); return 1; }
static void log_begin(void) { strcat(g_log, "|"); }
static void log_raw(const char *s) { strcat(g_log, s); }
static void log_dec(unsigned long v) { char b[24]; sprintf(b, "%lu", v); strcat(g_log, b); }
#include "src/sha256.inc"
#include "src/lighting_wire.h"
static volatile LONG g_ppi_modded_ok;   /* picker_pi.inc's */
static volatile LONG g_rt_layout = 1;   /* race_toast.inc's: the toast's layout is in place */
static ULONGLONG g_now = 100000;
#define EVO_NOW() g_now
#define EVO_TEST
#include "src/eventopts_sync.inc"
#include "src/event_options.inc"

static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

/* ---- a stand-in game: counted objects (the count at +0x10) and display strings released through their vftable ---- */
/* the first 0xa8 bytes are the game's (a choice list's five vftables, a row's SelectedIndex and function go in there); the stand-in's own
 * fields come after */
struct object { void *vtable; void *pad; LONG count; int kind; unsigned char game[0x90]; char text[32]; int value; void *parts[8]; int n; int selected; struct evo_fn fn; };
enum { K_LIST = 1, K_ENTRY, K_TEXT, K_ROW };
static int g_live_objects, g_live_strings, g_freed, g_in_session = 1, g_alloc_fails;
static struct object *g_rows[8];
static int g_row_count;
static char g_type_token, g_list_vt[5], g_fn_vt;

struct dstring { void **vtable; int count; char text[32]; };
static void ds_addref(void *s) { ((struct dstring *)s)->count++; }
static void ds_release(void *s) { if (--((struct dstring *)s)->count == 0) { g_live_strings--; free(s); } }
static void *g_ds_vtable[4] = {NULL, NULL, (void *)ds_addref, (void *)ds_release};

static struct object *new_object(int kind)
{
    struct object *o = calloc(1, sizeof *o);
    o->count = 1;
    o->kind = kind;
    g_live_objects++;
    return o;
}

static void **fake_make_string(void **out, const char *text, int length)
{
    struct dstring *s = calloc(1, sizeof *s);
    CHECK(length == -1);
    s->vtable = g_ds_vtable;
    s->count = 1;
    strcpy(s->text, text);
    g_live_strings++;
    *out = s;
    return out;
}

static void **fake_text_from(void **out, void **display)
{
    struct object *o = new_object(K_TEXT);
    strcpy(o->text, ((struct dstring *)*display)->text);
    *out = o;
    return out;
}

static void fake_free(void *object)
{
    struct object *o = object;
    int i;
    CHECK(o->count == 0);
    for (i = 0; i < o->n; i++) {   /* a list gives back its entries, a row its title and its choices, as the game's do */
        struct object *p = o->parts[i];
        if (InterlockedDecrement(&p->count) == 0) fake_free(p);
    }
    g_live_objects--;
    g_freed++;
    free(o);
}

static void *fake_list_type(void) { return &g_type_token; }

static void *fake_alloc(unsigned long long size, void *type)
{
    CHECK(size == 0x58 && type == &g_type_token);
    return g_alloc_fails ? NULL : new_object(K_LIST);
}

static void *fake_list_base(void *list) { return list; }

static void **fake_make_entry(void **out, int *value, void **display)
{
    struct object *o = new_object(K_ENTRY);
    o->value = *value;
    strcpy(o->text, ((struct dstring *)*display)->text);
    *out = o;
    return out;
}

static void fake_insert(void *list, int index, void *entry)
{
    struct object *l = list;
    CHECK(index == l->n && l->n < 8);
    InterlockedIncrement(&((struct object *)entry)->count);
    l->parts[l->n++] = entry;
}

static void **fake_make_row(void **out, void **title, int selected, void **choices, void *function)
{
    struct object *row = new_object(K_ROW);
    struct evo_fn *fn = function;
    CHECK(fn->vtable == &g_fn_vt && fn->impl == fn);   /* the wrapper in the function's own space */
    row->selected = selected;
    row->fn = *fn;
    row->fn.impl = &row->fn;
    *(int *)((unsigned char *)row + EVO_ROW_SELECTED) = selected;
    *(void **)((unsigned char *)row + EVO_ROW_FN_IMPL) = &row->fn;
    fn->impl = NULL;                                   /* the game empties the function it was given */
    InterlockedIncrement(&((struct object *)*title)->count);
    InterlockedIncrement(&((struct object *)*choices)->count);
    row->parts[row->n++] = *title;
    row->parts[row->n++] = *choices;
    *out = row;
    return out;
}

static int fake_add_row(void *collection, void *row)
{
    CHECK(collection == (void *)g_rows);
    InterlockedIncrement(&((struct object *)row)->count);
    g_rows[g_row_count] = row;
    return g_row_count++;
}

static int evo_in_session(const unsigned char *img) { (void)img; return g_in_session; }

/* the game's setter of a row's SelectedIndex: the int is at +0x28 of the boxed value */
static void *fake_selected(void *property, void *row, void *value)
{
    (void)property;
    *(int *)((unsigned char *)row + EVO_ROW_SELECTED) = *(int *)((unsigned char *)value + 0x28);
    return row;
}
static void set_row(void *row, int index)
{
    unsigned char boxed[0x30] = {0};
    *(int *)(boxed + 0x28) = index;
    CHECK(evo_selected_hook(NULL, row, boxed) == row);
}

/* the game's change notice, and the popup's Text property */
static char g_text_prop;
static int g_notices, g_text_notices;
static void fake_notify(void *object, void *property) { (void)object; g_notices++; if (property == &g_text_prop) g_text_notices++; }
static void *fake_text_property(void) { return &g_text_prop; }

static void *fake_builder(void *page, void *b, void *c, void *d)
{
    (void)b; (void)c; (void)d;
    if (*(void **)((unsigned char *)page + EVO_PAGE_ROWS)) {   /* the game's own rows come first */
        struct object *row = new_object(K_ROW);
        strcpy(row->text, "Car Class");
        g_rows[g_row_count++] = row;
    }
    return (void *)0x1234;
}

static void drop_rows(void)
{
    while (g_row_count) {
        struct object *row = g_rows[--g_row_count];
        if (InterlockedDecrement(&row->count) == 0) fake_free(row);
    }
}

static int hex_value(int c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; }
static unsigned read_hex(const char *path, unsigned char *out, unsigned max)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    unsigned n = 0, i;
    if (!f || !fgets(line, sizeof line, f)) { printf("cannot read %s\n", path); exit(2); }
    fclose(f);
    for (i = 0; line[i] && line[i + 1] && line[i] != '\n' && line[i] != '\r' && n < max; i += 2) out[n++] = (unsigned char)(hex_value(line[i]) * 16 + hex_value(line[i + 1]));
    return n;
}

/* the datagrams against the server's frozen vectors (key 0..31, epoch 16..31, xuid 0x9000123456789, sequence number 9, choice Allowed) */
static void wire(char **argv)
{
    unsigned char key[32], epoch[16], want[128], got[EO_REQUEST], reply[128], flags = 9;
    unsigned i, n;
    for (i = 0; i < 32; i++) key[i] = (unsigned char)i;
    for (i = 0; i < 16; i++) epoch[i] = (unsigned char)(16 + i);
    n = read_hex(argv[1], want, sizeof want);
    eo_request(got, key, 0x9000123456789ull, 9, epoch, 1);
    CHECK(n == EO_REQUEST && !memcmp(got, want, EO_REQUEST));
    eo_request(got, key, 0x9000123456789ull, 9, epoch, 7);
    CHECK(got[52] == EO_NONE);                                             /* anything but 0 or 1 is said as "nothing" */
    n = read_hex(argv[2], reply, sizeof reply);
    CHECK(eo_response(reply, n, key, 0x9000123456789ull, 9, 8, epoch, &flags) && flags == 0);
    n = read_hex(argv[3], reply, sizeof reply);
    CHECK(eo_response(reply, n, key, 0x9000123456789ull, 9, 8, epoch, &flags) && flags == EO_MODDED);
    n = read_hex(argv[4], reply, sizeof reply);
    CHECK(eo_response(reply, n, key, 0x9000123456789ull, 9, 8, epoch, &flags) && flags == EO_COMING);
    CHECK(!eo_response(reply, n, key, 0x9000123456789ull, 9, 9, epoch, &flags));   /* already seen */
    CHECK(!eo_response(reply, n, key, 0x9000123456789ull, 8, 0, epoch, &flags));   /* never asked */
    CHECK(!eo_response(reply, n, key, 0x9000123456788ull, 9, 8, epoch, &flags));   /* another player's */
    CHECK(!eo_response(want, EO_REQUEST, key, 0x9000123456789ull, 9, 8, epoch, &flags));   /* a request is not a reply */
    reply[52] ^= 1;
    CHECK(!eo_response(reply, n, key, 0x9000123456789ull, 9, 8, epoch, &flags));   /* changed on the way */
    reply[52] ^= 1;
    key[0] ^= 1;
    CHECK(!eo_response(reply, n, key, 0x9000123456789ull, 9, 8, epoch, &flags));   /* signed with another key */
}

int main(int argc, char **argv)
{
    static unsigned char page[0x800], bare[0x800];
    struct object *row, *title, *list;
    int i;

    if (argc < 5) { printf("usage: test.exe <request vector> <reply 0 vector> <reply 1 vector> <reply 2 vector>\n"); return 2; }
    wire(argv);

    g_evo.make_string = fake_make_string;
    g_evo.text_from = fake_text_from;
    g_evo.free_object = fake_free;
    g_evo.list_type = fake_list_type;
    g_evo.alloc = fake_alloc;
    g_evo.list_base = fake_list_base;
    g_evo.make_entry = fake_make_entry;
    g_evo.insert = fake_insert;
    g_evo.make_row = fake_make_row;
    g_evo.add_row = fake_add_row;
    for (i = 0; i < 5; i++) g_evo.list_vtable[i] = &g_list_vt[i];
    g_evo.fn_vtable = &g_fn_vt;
    g_evo.notify = fake_notify;
    g_evo.text_property = fake_text_property;
    g_evo_selected_orig = fake_selected;
    g_evo_toast_orig = fake_notify;
    g_evo_builder_orig = fake_builder;
    *(void **)(page + EVO_PAGE_ROWS) = g_rows;

    /* a server that has not answered (one without event options, a Road Trip session): the screen is left as the game makes it */
    CHECK(!evo_server_answers());
    evo_builder_hook(page, NULL, NULL, NULL);
    CHECK(g_row_count == 1);
    drop_rows();
    evo_reply(0, g_now);
    CHECK(evo_server_answers() && !g_ppi_modded_ok);

    /* the screen opens in a session whose server answers: the game's row, then ours */
    CHECK(evo_builder_hook(page, NULL, NULL, NULL) == (void *)0x1234);
    CHECK(g_row_count == 2 && !strcmp(g_rows[0]->text, "Car Class"));
    row = g_rows[1];
    title = row->parts[0];
    list = row->parts[1];
    CHECK(row->kind == K_ROW && row->selected == 0 && row->count == 1);                 /* only the collection holds the row */
    CHECK(title->kind == K_TEXT && !strcmp(title->text, "Modded cars") && title->count == 1);
    CHECK(list->kind == K_LIST && list->count == 1 && list->n == 2);
    for (i = 0; i < 5; i++) CHECK(*(void **)((unsigned char *)list + g_evo_list_vtable_at[i]) == &g_list_vt[i]);
    CHECK(((struct object *)list->parts[0])->value == 0 && !strcmp(((struct object *)list->parts[0])->text, "Not allowed"));
    CHECK(((struct object *)list->parts[1])->value == 1 && !strcmp(((struct object *)list->parts[1])->text, "Allowed"));
    CHECK(((struct object *)list->parts[0])->count == 1 && ((struct object *)list->parts[1])->count == 1);
    CHECK(g_live_strings == 0);                                                          /* every display string was released */
    CHECK(strstr(g_log, "the Modded cars row is on it (#1)") != NULL);

    /* the leader starts the event: the game calls the row's function with its SelectedIndex */
    /* while the screen is open the row's setting is said: Not allowed at first, each change of our row at once, nobody else's row */
    CHECK(g_evo_row == row && evo_choice_to_say(g_now) == 0);
    InterlockedExchange(&g_evo_send_now, 0);
    set_row(g_rows[0], 1);                                                              /* the game's own row */
    CHECK(evo_choice_to_say(g_now) == 0 && !g_evo_send_now);
    set_row(row, 1);
    CHECK(evo_choice_to_say(g_now) == 1 && g_evo_send_now);
    InterlockedExchange(&g_evo_send_now, 0);
    set_row(row, 1);                                                                    /* no change: nothing new to say */
    CHECK(!g_evo_send_now);
    CHECK(evo_choice_to_say(g_now + EVO_OPEN_MS) == 1 && evo_choice_to_say(g_now + EVO_OPEN_MS + 1) == EO_NONE);   /* the screen cannot be open for ever */
    set_row(row, 0);
    CHECK(evo_choice_to_say(g_now) == 0);

    /* the leader starts the event: the choice is final, the row is no longer watched */
    row->fn.fn(NULL, 1);
    CHECK(g_evo_row == NULL);
    set_row(row, 0);
    CHECK(evo_choice_to_say(g_now) == 1 && strstr(g_log, "Modded cars allowed (#1)") != NULL);
    CHECK(evo_choice_to_say(g_now + EVO_SAY_MS) == 1 && evo_choice_to_say(g_now + EVO_SAY_MS + 1) == EO_NONE);   /* said for a while, then no more */
    row->fn.fn(NULL, 0);
    CHECK(evo_choice_to_say(g_now) == 0 && strstr(g_log, "Modded cars not allowed (#2)") != NULL);

    /* the server's answers: the car pickers follow the newest one, and an answer that has gone stale counts for nothing */
    evo_reply(EO_MODDED, g_now);
    CHECK(g_ppi_modded_ok && evo_server_answers());
    evo_expire(g_now + EVO_STALE_MS);
    CHECK(g_ppi_modded_ok);
    g_now += EVO_STALE_MS + 1;
    evo_expire(g_now);
    CHECK(!g_ppi_modded_ok && !evo_server_answers());
    evo_reply(0, g_now);

    /* an invite toast: the wrench's switch (the popup's Text) is only set when the newest answer says modded cars, now or coming */
    {
        static unsigned char popup[0x200];
        struct object *empty = new_object(K_TEXT), *text;
        *(void **)(popup + EVO_TOAST_TEXT) = empty;
        g_notices = g_text_notices = 0;
        evo_toast_hook(popup, NULL);
        CHECK(g_notices == 1 && *(void **)(popup + EVO_TOAST_TEXT) == empty);          /* the game's notice ran, Text is as the game left it */
        evo_reply(EO_COMING, g_now);                                                    /* a player has the row on Allowed right now */
        CHECK(!g_ppi_modded_ok);                                                        /* which lets no car in yet */
        g_rt_layout = 0;
        evo_toast_hook(popup, NULL);
        CHECK(*(void **)(popup + EVO_TOAST_TEXT) == empty);                            /* without the layout there is no wrench to switch */
        g_rt_layout = 1;
        evo_toast_hook(popup, NULL);
        text = *(void **)(popup + EVO_TOAST_TEXT);
        CHECK(text != empty && text->kind == K_TEXT && !strcmp(text->text, " ") && text->count == 1 && g_text_notices == 1);
        CHECK(strstr(g_log, "an invite toast shows the wrench") != NULL);
        evo_reply(EO_MODDED, g_now);
        evo_toast_hook(popup, NULL);
        CHECK(*(void **)(popup + EVO_TOAST_TEXT) != text && g_text_notices == 2);      /* the old Text was given back */
        evo_release(*(void **)(popup + EVO_TOAST_TEXT));
        evo_reply(0, g_now);
        CHECK(g_live_strings == 0);
    }

    /* the screen closes: the game drops its rows, and nothing of ours is left behind */
    drop_rows();
    CHECK(g_live_objects == 0 && g_live_strings == 0);

    /* outside a session the game's rows are left as they are */
    g_in_session = 0;
    evo_builder_hook(page, NULL, NULL, NULL);
    CHECK(g_row_count == 1);
    drop_rows();
    g_in_session = 1;

    /* a page without a row collection, and an allocation that gives nothing: the game's code still runs, nothing is added, nothing leaks */
    CHECK(evo_builder_hook(bare, NULL, NULL, NULL) == (void *)0x1234 && g_row_count == 0);
    CHECK(strstr(g_log, "no row collection") != NULL);
    g_alloc_fails = 1;
    evo_builder_hook(page, NULL, NULL, NULL);
    CHECK(g_row_count == 1);
    drop_rows();
    CHECK(g_live_objects == 0 && g_live_strings == 0);

    printf(failed ? "event options: %d check(s) FAILED\n" : "event options: all checks passed\n", failed);
    return failed != 0;
}
