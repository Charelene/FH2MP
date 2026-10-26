/* Game 1.0.0.64, RVA 0x105e530: sums TorqueScale from three rowsets, then releases each non-null rowset.
 * A missing optional rowset is NULL (report #31, fault +0x105e5dc). Supply an empty rowset only for missing inputs,
 * so the original still applies every valid upgrade and balances its releases. The only called slots for an empty rowset
 * are +0x148 (row count) and +0x18 (release); +0xc0 (read TorqueScale) is skipped when there are zero rows.
 */
typedef void (*torque_scale_fn)(float *, void **, void **, void **);
static torque_scale_fn g_torque_scale_orig;

static int torque_empty_count(void *self)
{
    (void)self;
    return 0;
}

static unsigned torque_empty_release(void *self)
{
    (void)self;
    return 0;
}

static void *const g_torque_empty_vtable[42] = {
    [3] = (void *)torque_empty_release,
    [41] = (void *)torque_empty_count,
};
static void *const *const g_torque_empty = g_torque_empty_vtable;

static void torque_scale_guard(float *out, void **a, void **b, void **c)
{
    void *empty = (void *)&g_torque_empty;
    if (!out || !g_torque_scale_orig) {
        return;
    }
    g_torque_scale_orig(out, a && *a ? a : &empty, b && *b ? b : &empty, c && *c ? c : &empty);
}
