#!/usr/bin/env bash
# Missing upgrade rows must not erase valid torque modifiers or leak their rowset references.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test.c" <<'C'
#include <assert.h>
#include <math.h>
#include "torque_scale.inc"
typedef struct { void *const *vt; float scale; unsigned releases; } row;
static int count(void *p) { (void)p; return 1; }
static unsigned release(void *p) { return ++((row *)p)->releases; }
static void *const vt[42] = { [3] = (void *)release, [41] = (void *)count };
static void original(float *out, void **a, void **b, void **c)
{
    void **args[] = {a, b, c};
    *out = 1.0f;
    for (unsigned i = 0; i < 3; i++) {
        void *obj = *args[i];
        void *const *v = *(void *const **)obj;
        if (((int (*)(void *))v[41])(obj)) *out += ((row *)obj)->scale - 1.0f;
        ((unsigned (*)(void *))v[3])(obj);
    }
}
int main(void)
{
    g_torque_scale_orig = original;
    for (unsigned mask = 0; mask < 8; mask++) {
        row rows[] = {{vt, 1.2f, 0}, {vt, 1.3f, 0}, {vt, 1.4f, 0}};
        void *p[3];
        float want = 1.0f, out = 0;
        for (unsigned i = 0; i < 3; i++) {
            p[i] = mask & (1u << i) ? &rows[i] : 0;
            if (p[i]) want += rows[i].scale - 1.0f;
        }
        torque_scale_guard(&out, &p[0], &p[1], &p[2]);
        assert(fabsf(out - want) < 0.00001f);
        for (unsigned i = 0; i < 3; i++) assert(rows[i].releases == !!(mask & (1u << i)));
    }
    float out = 0;
    torque_scale_guard(&out, 0, 0, 0);
    assert(out == 1.0f);
    return 0;
}
C
${HOSTCC:-cc} -O2 -Wall -Wextra -I src "$tmp/test.c" -o "$tmp/test"
"$tmp/test"
echo 'torque_scale: all missing-row combinations preserve valid modifiers and releases'
