/* The speedometer hook guard (src/hook_guard.inc) and the GameplayLog guard (src/gameplaylog_guard.inc) against real memory under Wine:
 * a fake game image with the call site and a small UpdateGauges that records its arguments, relays made with VirtualAlloc and released with
 * VirtualFree, and a std::map laid out as the decompile reads it. What the game's real UpdateGauges does, and what releases the relay on
 * players' machines, is not something this can show. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static char g_raw[2048];
static unsigned char *g_img;
static int g_failed;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void log_hex(unsigned long long v, int digits) { char b[32]; sprintf(b, "%0*llx", digits, v); log_raw(b); }
static void mem_copy(void *d, const void *s, SIZE_T n) { memcpy(d, s, n); }
static unsigned char *find_fh2_image(void) { return g_img; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag; (void)what;
    return !memcmp(img + rva, expect, n);
}
static int g_call_hooks, g_hooks;
static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    int rel;
    (void)name; (void)detour;
    g_call_hooks++;
    memcpy(&rel, (unsigned char *)site + 1, 4);
    *original = (unsigned char *)site + 5 + rel;          /* the call's old target, as the platform hands it over */
    return 1;
}
static void *g_hook_original;
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name; (void)target; (void)detour;
    g_hooks++;
    *original = g_hook_original;
    return 1;
}

#include "src/hook_guard.inc"
#include "src/gameplaylog_guard.inc"

/* UpdateGauges stand-in: the real prologue, then a call to gauges_called with the same arguments */
static void *g_gauges_self;
static float g_gauges_rpm;
static int g_gauges_calls, g_detour_calls;
static void gauges_called(void *self, float rpm) { g_gauges_self = self; g_gauges_rpm = rpm; g_gauges_calls++; }
static void detour(void *self, float rpm) { (void)self; (void)rpm; g_detour_calls++; }

static void write_gauges(unsigned char *fn)
{
    unsigned char code[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20,                    /* push rbx ; sub rsp,0x20 */
                            0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,                   /* mov rax, gauges_called */
                            0xff, 0xd0,                                           /* call rax */
                            0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};                  /* add rsp,0x20 ; pop rbx ; ret */
    void *target = (void *)gauges_called;
    memcpy(code + 8, &target, 8);
    memcpy(fn, code, sizeof code);
}

/* a relay page near the fake image: jmp [rip+0] to `to` */
static unsigned char *make_relay(void *to)
{
    unsigned char *r = VirtualAlloc(g_img + 0xc00000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    static const unsigned char jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    if (!r) return NULL;
    memcpy(r + 0x24, jmp, 6);
    memcpy(r + 0x2a, &to, 8);
    return r;
}

static void point_at(unsigned char *fn, unsigned char *to)
{
    int rel = (int)(to - (fn + 5));
    fn[0] = 0xe9;
    memcpy(fn + 1, &rel, 4);
}

static void reset(void) { g_raw[0] = 0; g_gauges_calls = g_detour_calls = 0; g_gauges_self = NULL; g_hg_logged = 0; }

static void test_hook_guard(void)
{
    unsigned char *fn, *relay;
    void *self = (void *)0x1234;

    g_img = VirtualAlloc(NULL, 0xb00000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    CHECK(g_img != NULL);
    memcpy(g_img + HG_CALL_SITE, g_hg_call, sizeof g_hg_call);
    fn = g_img + HG_TARGET;
    write_gauges(fn);

    CHECK(patch_hook_guard());
    CHECK(g_call_hooks == 1);
    CHECK(g_hg_orig == (void *)fn);
    CHECK(strstr(g_raw, "checks the jump at game+0xad6070") != NULL);

    /* the game's own code: called as it is */
    reset();
    hg_speedometer(self, 4500.0f);
    CHECK(g_gauges_calls == 1 && g_gauges_self == self && g_gauges_rpm == 4500.0f);
    CHECK(g_raw[0] == 0);

    /* another mod's hook through a live relay: left alone, its detour runs */
    reset();
    relay = make_relay((void *)detour);
    CHECK(relay != NULL);
    point_at(fn, relay + 0x24);
    hg_speedometer(self, 1000.0f);
    CHECK(g_detour_calls == 1 && g_gauges_calls == 0);
    CHECK(fn[0] == 0xe9);
    CHECK(g_raw[0] == 0);

    /* the relay is released: the prologue comes back and the game's own code runs */
    reset();
    CHECK(VirtualFree(relay, 0, MEM_RELEASE));
    hg_speedometer(self, 2000.0f);
    CHECK(!memcmp(fn, g_hg_prologue, 5));
    CHECK(fn[5] == 0x20);                                                 /* byte 6 of sub rsp,0x20 untouched */
    CHECK(g_gauges_calls == 1 && g_gauges_rpm == 2000.0f && g_detour_calls == 0);
    CHECK(strstr(g_raw, "is not executable memory") != NULL && strstr(g_raw, "put back") != NULL);
    hg_speedometer(self, 2100.0f);                                        /* logged once */
    CHECK(g_gauges_calls == 2);
    CHECK(strstr(strstr(g_raw, "put back") + 1, "put back") == NULL);

    /* jmp [slot] whose slot page was released (the platform's stub reads its slot from the next page) */
    reset();
    {
        unsigned char *stub = VirtualAlloc(g_img + 0xc00000, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        static const unsigned char jmp_slot[6] = {0xff, 0x25, 0xfa, 0x0f, 0, 0};   /* jmp [rip+0xffa]: the slot at stub+0x1000 */
        CHECK(stub != NULL);
        memcpy(stub, jmp_slot, 6);
        *(void **)(stub + 0x1000) = (void *)detour;
        point_at(fn, stub);
        hg_speedometer(self, 3000.0f);
        CHECK(g_detour_calls == 1 && g_gauges_calls == 0);
        CHECK(VirtualFree(stub + 0x1000, 0x1000, MEM_DECOMMIT));
        hg_speedometer(self, 3100.0f);
        CHECK(!memcmp(fn, g_hg_prologue, 5) && g_gauges_calls == 1);
        VirtualFree(stub, 0, MEM_RELEASE);
    }

    /* a jump into memory that is readable but not executable */
    reset();
    {
        unsigned char *data = VirtualAlloc(g_img + 0xc00000, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(data != NULL);
        point_at(fn, data);
        hg_speedometer(self, 3200.0f);
        CHECK(!memcmp(fn, g_hg_prologue, 5) && g_gauges_calls == 1);
        VirtualFree(data, 0, MEM_RELEASE);
    }

    /* not the expected call: not installed */
    g_img[HG_CALL_SITE] = 0x90;
    g_call_hooks = 0;
    CHECK(!patch_hook_guard() && g_call_hooks == 0);
    VirtualFree(g_img, 0, MEM_RELEASE);
    g_img = NULL;
}

/* MSVC std::map node: [0] left, [1] parent, [2] right, +0x19 is-nil, +0x20 key, value from +0x28, the log pointer at +0x60 */
typedef struct node { struct node *left, *parent, *right; unsigned char color, isnil, pad[6]; unsigned long long key; unsigned char value[0x38]; void *log; } node;

static int g_orig_calls;
static void orig_handler(void *manager, void *event) { (void)manager; (void)event; g_orig_calls++; }

static void test_gameplaylog_guard(void)
{
    static unsigned char manager[0x40], event[0x1400];
    node head, a, b, c;
    int dummy_log;
    static const unsigned long long keys[] = {0x9000011111111ULL, 0x9000022222222ULL, 0x9000033333333ULL};

    CHECK(sizeof(node) == 0x68 && (size_t)&((node *)0)->key == 0x20 && (size_t)&((node *)0)->log == 0x60);
    memset(&head, 0, sizeof head);
    head.isnil = 1;
    head.parent = &head;                                                   /* empty map: the root is the head */
    *(node **)(manager + 0x20) = &head;
    *(unsigned long long *)(event + 0x1300) = keys[1];
    g_hook_original = (void *)orig_handler;
    g_gl_orig = orig_handler;                                              /* as patch_gameplaylog_guard sets it, checked at the end */
    g_orig_calls = 0;
    g_raw[0] = 0;

    gl_handler(manager, event);                                            /* empty map: dropped */
    CHECK(g_orig_calls == 0 && strstr(g_raw, "is left out") != NULL);

    /* b at the root, a left, c right */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    a.key = keys[0]; b.key = keys[1]; c.key = keys[2];
    a.left = a.right = b.left = b.right = c.left = c.right = &head;          /* leaves point at the nil head */
    b.left = &a; b.right = &c; a.parent = c.parent = &b; b.parent = &head;
    head.parent = &b;
    a.log = &dummy_log; c.log = &dummy_log; b.log = NULL;

    gl_handler(manager, event);                                            /* b has a record but a NULL log: dropped */
    CHECK(g_orig_calls == 0);
    *(unsigned long long *)(event + 0x1300) = keys[0];
    gl_handler(manager, event);
    CHECK(g_orig_calls == 1);
    *(unsigned long long *)(event + 0x1300) = keys[2];
    gl_handler(manager, event);
    CHECK(g_orig_calls == 2);
    *(unsigned long long *)(event + 0x1300) = keys[2] + 1;                 /* larger than every key */
    gl_handler(manager, event);
    *(unsigned long long *)(event + 0x1300) = keys[0] - 1;                 /* smaller than every key */
    gl_handler(manager, event);
    *(unsigned long long *)(event + 0x1300) = keys[1] + 1;                 /* between two keys */
    gl_handler(manager, event);
    CHECK(g_orig_calls == 2);
    b.log = &dummy_log;
    *(unsigned long long *)(event + 0x1300) = keys[1];
    gl_handler(manager, event);
    CHECK(g_orig_calls == 3);
    *(node **)(manager + 0x20) = NULL;                                      /* no map at all */
    gl_handler(manager, event);
    CHECK(g_orig_calls == 3);
    CHECK(strstr(strstr(g_raw, "is left out") + 1, "is left out") == NULL);  /* logged once */

    /* install: the handler's prologue is checked first */
    g_img = VirtualAlloc(NULL, 0xa00000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(g_img != NULL);
    memcpy(g_img + GL_HANDLER, g_gl_prologue, sizeof g_gl_prologue);
    g_hooks = 0;
    g_gl_orig = NULL;
    CHECK(patch_gameplaylog_guard() && g_hooks == 1 && g_gl_orig == orig_handler);
    g_img[GL_HANDLER] = 0xcc;
    CHECK(!patch_gameplaylog_guard() && g_hooks == 1);
    VirtualFree(g_img, 0, MEM_RELEASE);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_hook_guard();
    test_gameplaylog_guard();
    if (!g_failed) puts("crash guards: all checks pass");
    return g_failed != 0;
}
