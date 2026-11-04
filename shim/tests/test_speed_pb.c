/* Boundary harness: native save/HUD ownership and asynchronous UI delivery are
 * stand-ins. Tests the production hooks with real Windows TLS and weak counts;
 * this is not an in-game verification of the recovered component offsets. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "src/speed_pb.inc"

typedef struct { void **vtable; LONG strong, weak; unsigned deleted; } control;
static void destroy(void *p) { (void)p; }
static void dispose(void *p) { ((control *)p)->deleted++; }
static void *vtable[] = {(void *)destroy, (void *)dispose};
typedef struct {
    unsigned char trigger[0x400], saved[16], hud[0x400], comparison[0x200], request[0xa368];
    control controls[3];
    speed_pb_ref result;
} fixture;
static HANDLE gate_a, gate_b;
static fixture *nested;
static int threaded;

static speed_pb_ref *resolve(void *bound, speed_pb_ref *out)
{
    *out = *(speed_pb_ref *)((unsigned char *)bound + 0x38);
    if (!speed_pb_alive(out)) out->object = out->control = NULL;
    else InterlockedIncrement((LONG *)out->control + 2);
    return out;
}
static float property(void *p, speed_pb_string *key)
{
    assert(!strcmp(key->text, "PersonalBest") && key->size == 12 && key->capacity == 15);
    key->text[0] = 0; key->size = 0; /* native consumes the by-value string */
    return *(float *)p;
}
static float score(void *p) { return (float)*(double *)((unsigned char *)p + 0xa2e0); }
static void comparison(void *p, speed_pb_ref *r)
{
    unsigned char *c = p;
    if (nested) {
        fixture *other = nested;
        nested = NULL;
        speed_pb_compare(other->comparison, &other->result);
    }
    if (threaded) {
        HANDLE mine = c[0x1ff] ? gate_b : gate_a;
        HANDLE other = c[0x1ff] ? gate_a : gate_b;
        SetEvent(mine);
        assert(WaitForSingleObject(other, 5000) == WAIT_OBJECT_0);
    }
    *(float *)(c + 0x120) = speed_pb_score(r->object);
    c[0x128] = *(float *)(c + 0x120) < *(float *)(c + 0x11c);
    /* Observers run inside the native comparison, before it returns. */
    c[0x1fe] = c[0x128];
    *(float *)(c + 0x11c) = -1;
}
static void completed(void *p, int zone)
{
    unsigned char *trigger = p;
    speed_pb_ref saved = {0}, hud = {0}, ui = {0};
    resolve(trigger + (zone ? 0x318 : 0x190), &saved);
    resolve(trigger + (zone ? 0x1f8 : 0x148), &hud);
    if (saved.object && hud.object) {
        float speed = *(float *)(trigger + (zone ? 0xd0 : 0xb0));
        if (speed > *(float *)saved.object) *(float *)saved.object = speed;
        resolve((unsigned char *)hud.object + 0x380, &ui);
        if (ui.object) *(float *)((unsigned char *)ui.object + 0x11c) = speed;
    }
    speed_pb_release(&ui); speed_pb_release(&hud); speed_pb_release(&saved);
}
static void trap(void *p, void *player) { assert(player == (void *)123); completed(p, 0); }
static void zone(void *p) { if (!*((unsigned char *)p + 0x3b8)) completed(p, 1); }
static void setup(fixture *f, float saved, float speed, double online, int is_zone)
{
    memset(f, 0, sizeof *f);
    for (int i = 0; i < 3; ++i) f->controls[i] = (control){vtable, 1, 1, 0};
    *(float *)f->saved = saved;
    *(float *)(f->trigger + (is_zone ? 0xd0 : 0xb0)) = speed;
    *(speed_pb_ref *)(f->trigger + (is_zone ? 0x318 : 0x190) + 0x38) = (speed_pb_ref){f->saved, &f->controls[0]};
    *(speed_pb_ref *)(f->trigger + (is_zone ? 0x1f8 : 0x148) + 0x38) = (speed_pb_ref){f->hud, &f->controls[1]};
    *(speed_pb_ref *)(f->hud + 0x380 + 0x38) = (speed_pb_ref){f->comparison, &f->controls[2]};
    *(double *)(f->request + 0xa2e0) = online;
    f->result = (speed_pb_ref){f->request, NULL};
}
static void clear_entries(void)
{
    for (unsigned i = 0; i < SPEED_PB_SLOTS; ++i) speed_pb_weak_release(&g_speed_pb_entries[i].weak);
}
static void check(fixture *f, float expected_previous, int expected_pb)
{
    double online = *(double *)(f->request + 0xa2e0);
    speed_pb_compare(f->comparison, &f->result);
    assert(*(float *)(f->comparison + 0x120) == expected_previous);
    assert(f->comparison[0x128] == expected_pb && f->comparison[0x1fe] == expected_pb);
    assert(*(double *)(f->request + 0xa2e0) == online);
    for (int i = 0; i < 3; ++i) assert(f->controls[i].strong == 1);
    assert(f->controls[0].weak == 1 && f->controls[1].weak == 1);
    assert(TlsGetValue(g_speed_pb_tls) == NULL);
}
static DWORD WINAPI compare_thread(void *arg)
{
    fixture *f = arg;
    speed_pb_compare(f->comparison, &f->result);
    assert(TlsGetValue(g_speed_pb_tls) == NULL);
    return 0;
}
int main(void)
{
    fixture *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    assert(a && b);
    g_speed_pb_resolve = resolve; g_speed_pb_property = property;
    g_speed_pb_trap = trap; g_speed_pb_zone = zone;
    g_speed_pb_compare = comparison; g_speed_pb_score = score;
    g_speed_pb_tls = TlsAlloc(); assert(g_speed_pb_tls != TLS_OUT_OF_INDEXES);

    /* Passing control demonstrates the reported old behavior. */
    setup(a, 50, 40, 0, 0); speed_pb_trap(a->trigger, (void *)123); check(a, 0, 1);
    g_speed_pb_active = 1;
    const struct { float saved, speed, online, previous; int pb; } cases[] = {
        {50,40,0,50,0}, {50,50,0,50,0}, {50,60,0,50,1}, {0,20,0,0,1},
        {50,40,30,50,0}, {50,60,30,50,1}, {50,60,70,70,0}, {50,80,70,70,1},
    };
    for (int z = 0; z < 2; ++z) for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        clear_entries(); setup(a, cases[i].saved, cases[i].speed, cases[i].online, z);
        if (z) speed_pb_zone(a->trigger); else speed_pb_trap(a->trigger, (void *)123);
        check(a, cases[i].previous, cases[i].pb);
        assert(*(float *)a->saved == (cases[i].saved > cases[i].speed ? cases[i].saved : cases[i].speed));
        assert(a->controls[2].weak == 2);
    }
    clear_entries(); setup(a, 50, 60, 0, 0); setup(b, 100, 80, 0, 1);
    speed_pb_trap(a->trigger, (void *)123); speed_pb_zone(b->trigger);
    check(b, 100, 0); check(a, 50, 1); /* reverse asynchronous delivery */
    *(float *)(a->comparison + 0x11c) = 45; check(a, 0, 1); /* unmatched attempt: native behavior */
    /* A dead weak target at a reused address cannot supply the prior owner's PB. */
    a->controls[2].strong = 0; a->controls[2].weak--;
    *(float *)(a->comparison + 0x11c) = 60;
    speed_pb_compare(a->comparison, &a->result);
    assert(a->comparison[0x128] && a->controls[2].deleted == 1);
    clear_entries(); setup(a, 50, 60, 0, 0); setup(b, 100, 80, 0, 1);
    speed_pb_trap(a->trigger, (void *)123); speed_pb_zone(b->trigger);
    nested = b; check(a, 50, 1); assert(b->comparison[0x128] == 0);
    *(float *)(a->comparison + 0x11c) = 60; *(float *)(b->comparison + 0x11c) = 80;
    gate_a = CreateEventW(NULL, TRUE, FALSE, NULL); gate_b = CreateEventW(NULL, TRUE, FALSE, NULL);
    assert(gate_a && gate_b); b->comparison[0x1ff] = 1; threaded = 1;
    HANDLE threads[] = {CreateThread(NULL, 0, compare_thread, a, 0, NULL), CreateThread(NULL, 0, compare_thread, b, 0, NULL)};
    assert(threads[0] && threads[1]);
    assert(WaitForMultipleObjects(2, threads, TRUE, 10000) == WAIT_OBJECT_0);
    assert(a->comparison[0x128] == 1 && b->comparison[0x128] == 0);
    threaded = 0;
    CloseHandle(threads[0]); CloseHandle(threads[1]); CloseHandle(gate_a); CloseHandle(gate_b);
    clear_entries();
    assert(a->controls[2].weak == 1 && b->controls[2].weak == 1);
    setup(a, 50, 40, 0, 1); a->trigger[0x3b8] = 1;
    speed_pb_zone(a->trigger);
    assert(a->controls[2].weak == 1 && *(float *)a->saved == 50);
    fixture *many = calloc(SPEED_PB_SLOTS + 1, sizeof *many);
    assert(many);
    for (unsigned i = 0; i <= SPEED_PB_SLOTS; ++i) {
        setup(&many[i], 50, 40, 0, 0);
        speed_pb_trap(many[i].trigger, (void *)123);
    }
    check(&many[0], 0, 1); /* eviction preserves original behavior */
    for (unsigned i = 1; i <= SPEED_PB_SLOTS; ++i) check(&many[i], 50, 0);
    clear_entries();
    for (unsigned i = 0; i <= SPEED_PB_SLOTS; ++i) assert(many[i].controls[2].weak == 1);
    free(many);
    TlsFree(g_speed_pb_tls); free(a); free(b);
    puts("speed_pb: old-behavior control, trap/zone PBs, ties, stale online rows, async/nested/threaded comparisons and weak lifetime pass");
    return 0;
}
