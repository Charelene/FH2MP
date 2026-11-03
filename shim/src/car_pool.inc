/* Remote car slots (CarCacheModule::vfunc_2, game+0x47a570).
 * The game builds a car cache at module init with two groups: "Player LOD0" with 1 slot (the local car) and "Non-LOD0" with min(NumAICars if set else 11,
 * 12) slots, a pre-built ForzaCar each. A remote player's car takes a Non-LOD0 slot (CarNetworkedModule::OnNetworkPlayerJoined -> request queue ->
 * helper_47ba20); with no free slot the request stays queued, so the 12th car of a world never exists and its RemoteCarPrediction is never reached. A real
 * client with 17 or 75 players showed exactly 11 remote cars (dev 2026-11-02).
 * Patched: the 11 (imm32 of `mov r9d, 0xb` at +0x28) becomes FH2MP_CAR_SLOTS, and the clamp to 12 (`cmovge rax, rcx` at +0x4d) is nopped, so the group
 * holds that many slots. A user-set NumAICars still wins, unclamped. Not patched: NumAICars itself (it also sets AI/Drivatar counts). Unset FH2MP_CAR_SLOTS
 * changes nothing; the value is 11 to 75. Each slot is a whole ForzaCar built on demand: memory per slot was not measured. 75 slots alone made every
 * remote car land in load state 12: every net vehicle arrives with no car id and takes a stand-in id from a fixed range by its type (FindOrQueue 0xfd08b0:
 * players 0..11, AI 12..29); the first twelve player vehicles use the whole range and the next ones are invalid. The third patch raises that range's top
 * (the imm8 of `lea ebx,[rax+0xb]` at +0x17b) to the slot count; AI ids stay 12..29 and the in-use check keeps every id unique. FH2MP_CAR_DIAG=1
 * installs only the passive log, with the cache left as the game builds it. */
#define CAR_POOL_RVA 0x47a570
#define CAR_POOL_NATIVE 11
#define CAR_POOL_MAX 75

static int car_pool_from_env(void)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA("FH2MP_CAR_SLOTS", v, sizeof v);
    int i, total = 0;

    if (n == 0 || n >= sizeof v) {
        return 0;
    }
    for (i = 0; v[i]; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return 0;
        }
        total = total * 10 + (v[i] - '0');
    }
    return total > CAR_POOL_NATIVE && total <= CAR_POOL_MAX ? total : 0;
}

/* What the cache says while it runs, so a world that shows no cars can be told apart from a cache that never gets a request. The tick function
 * (CarCacheModule::vfunc_4, game+0x4748f0) is hooked passively; the Non-LOD0 group is module+0xf0 (begin/end of the slot vector +0x60/+0x68, of the
 * request queue +0x78/+0x80, free slots +0x90, build budget +0x94), the global load queue is *(game+0x365ad70) (idle when +0x14 == 0 and +0xc == +8,
 * FUN_140fdb330). One line when any of these changes, never per tick. */
#define CAR_POOL_TICK_RVA 0x4748f0
#define CAR_POOL_GROUP 0xf0
#define CAR_POOL_QUEUE_PTR_RVA 0x365ad70
typedef unsigned long long (*car_pool_tick_fn)(void *, void *, void *, void *);
static car_pool_tick_fn g_car_pool_tick_orig;
#define CAR_POOL_SLOT 0x590
#define CAR_POOL_SLOTS_MAX 96
#define CAR_POOL_HELD 22                 /* the scan's counts: 6 below, then the cars' load states 0..15 */
#define CAR_POOL_FIELDS (5 + CAR_POOL_HELD)
static unsigned g_car_pool_seen[CAR_POOL_FIELDS];
static int g_car_pool_seen_any;
static ULONGLONG g_car_pool_scan_at;
static unsigned char g_car_pool_slots[CAR_POOL_SLOTS_MAX * CAR_POOL_SLOT];

/* The slots by what they hold (helper_480a60 promotes a slot's request from state 1 to 2 once its car is loaded and request+0x5a8 is set):
 * cars = slot+0x560 set, assigned = slot+0x558 (a request) set, unloading = slot+0x568 set, then of the assigned ones the request's state (+0x598)
 * 1 or 2 and how many have +0x5a8 set. Scanned twice a second at most. */
/* Each assigned slot's pending vehicle (request+0x568; its info is handle+0x110: the car's unique id short at +0x234 and +0x236, the vehicle type at
 * +0x488, which FindOrQueue 0xfd08b0 uses to pick the stand-in id range). One line per slot when its values change, 300 lines at most. */
static struct { unsigned long long request; int id, id2, type; unsigned state, load; } g_car_pool_vehicle[CAR_POOL_SLOTS_MAX];
static unsigned g_car_pool_vehicle_lines;

static void car_pool_vehicle_line(unsigned index, unsigned long long request, unsigned load)
{
    unsigned long long vehicle = 0;
    unsigned char info[4], kind[4];
    int id, id2, type;
    unsigned state = 0;

    if (g_car_pool_vehicle_lines >= 300 || !read_game_memory((const void *)(ULONG_PTR)(request + 0x568), &vehicle, sizeof vehicle) || !vehicle ||
        !read_game_memory((const void *)(ULONG_PTR)(vehicle + 0x110 + 0x234), info, sizeof info) ||
        !read_game_memory((const void *)(ULONG_PTR)(vehicle + 0x110 + 0x488), kind, sizeof kind)) {
        return;
    }
    read_game_memory((const void *)(ULONG_PTR)(request + 0x598), &state, sizeof state);
    id = (short)(info[0] | (info[1] << 8));
    id2 = (short)(info[2] | (info[3] << 8));
    type = (int)(kind[0] | (kind[1] << 8) | (kind[2] << 16) | ((unsigned)kind[3] << 24));
    if (g_car_pool_vehicle[index].request == request && g_car_pool_vehicle[index].id == id && g_car_pool_vehicle[index].id2 == id2 &&
        g_car_pool_vehicle[index].type == type && g_car_pool_vehicle[index].state == state && g_car_pool_vehicle[index].load == load) {
        return;
    }
    g_car_pool_vehicle[index].request = request;
    g_car_pool_vehicle[index].id = id;
    g_car_pool_vehicle[index].id2 = id2;
    g_car_pool_vehicle[index].type = type;
    g_car_pool_vehicle[index].state = state;
    g_car_pool_vehicle[index].load = load;
    g_car_pool_vehicle_lines++;
    log_begin();
    log_raw("carpool: slot ");
    log_dec(index);
    log_raw(" vehicle id ");
    log_dec((unsigned long)(unsigned short)id);
    log_raw("/");
    log_dec((unsigned long)(unsigned short)id2);
    log_raw(" type ");
    log_dec((unsigned long)type);
    log_raw(", request state ");
    log_dec(state);
    log_raw(", load state ");
    log_dec(load);
    log_raw("\n");
}

static void car_pool_scan(unsigned slots, unsigned *out)
{
    unsigned i;

    memset(out, 0, CAR_POOL_HELD * sizeof out[0]);
    for (i = 0; i < slots; i++) {
        const unsigned char *slot = g_car_pool_slots + (size_t)i * CAR_POOL_SLOT;
        unsigned long long car = *(const unsigned long long *)(slot + 0x560), request = *(const unsigned long long *)(slot + 0x558);
        unsigned char tail[0x11];
        unsigned load = 99;

        out[0] += car != 0;
        if (car) {
            if (read_game_memory((const void *)(ULONG_PTR)(car + 0x20), &load, sizeof load)) {
                out[6 + (load < 15 ? load : 15)]++;      /* the car's load request: 7, 8 and 0xb are 'loaded' (helper_fd0750) */
            } else {
                load = 99;
            }
        }
        if (request) {
            car_pool_vehicle_line(i, request, load);
        }
        out[1] += request != 0;
        out[2] += *(const unsigned long long *)(slot + 0x568) != 0;
        if (request && read_game_memory((const void *)(ULONG_PTR)(request + 0x598), tail, sizeof tail)) {
            unsigned state = *(unsigned *)tail;

            out[3] += state == 1;
            out[4] += state == 2;
            out[5] += tail[0x10] != 0;
        }
    }
}

static void car_pool_state_line(const unsigned char *group, const unsigned *queue, int have_queue, int scanned, const unsigned *held)
{
    unsigned long long slots_b = *(const unsigned long long *)(group + 0x60), slots_e = *(const unsigned long long *)(group + 0x68);
    unsigned long long req_b = *(const unsigned long long *)(group + 0x78), req_e = *(const unsigned long long *)(group + 0x80);
    unsigned now[CAR_POOL_FIELDS], i;

    now[0] = (unsigned)((slots_e - slots_b) / CAR_POOL_SLOT);
    now[1] = *(const unsigned *)(group + 0x90);
    now[2] = *(const unsigned *)(group + 0x94);
    now[3] = (unsigned)((req_e - req_b) / 8);
    now[4] = have_queue ? (queue[3] == 0 && queue[0] == queue[1]) : 2;      /* queue[] is +8, +0xc, +0x10, +0x14: idle when +0x14 == 0 and +0xc == +8 */
    memcpy(now + 5, held, CAR_POOL_HELD * sizeof now[0]);
    if (g_car_pool_seen_any && !memcmp(now, g_car_pool_seen, sizeof now)) {
        return;
    }
    memcpy(g_car_pool_seen, now, sizeof now);
    g_car_pool_seen_any = 1;
    log_begin();
    log_raw("carpool: Non-LOD0 group slots ");
    log_dec(now[0]);
    log_raw(", free ");
    log_dec(now[1]);
    log_raw(", budget ");
    log_dec(now[2]);
    log_raw(", queued requests ");
    log_dec(now[3]);
    log_raw(now[4] == 2 ? ", global load queue unreadable" : now[4] ? ", global load queue idle" : ", global load queue busy");
    if (scanned) {
        log_raw(", cars ");
        log_dec(now[5]);
        log_raw(", assigned ");
        log_dec(now[6]);
        log_raw(", unloading ");
        log_dec(now[7]);
        log_raw(", request state 1 ");
        log_dec(now[8]);
        log_raw(", state 2 ");
        log_dec(now[9]);
        log_raw(", ready flag ");
        log_dec(now[10]);
        log_raw(", car load states");
        for (i = 0; i < 16; i++) {
            if (now[11 + i]) {
                log_raw(" ");
                log_dec(i);
                log_raw("=");
                log_dec(now[11 + i]);
            }
        }
    }
    log_raw("\n");
}

static unsigned long long car_pool_tick(void *self, void *a, void *b, void *c)
{
    unsigned char *img = find_fh2_image();
    unsigned char group[0xa0];
    unsigned queue[6] = {0, 0, 0, 0, 0, 0}, held[CAR_POOL_HELD];
    unsigned long long qptr = 0;
    unsigned long long result = g_car_pool_tick_orig(self, a, b, c);
    int have_queue = 0, scanned = 0;

    memset(held, 0, sizeof held);

    if (img && self && read_game_memory((unsigned char *)self + CAR_POOL_GROUP, group, sizeof group)) {
        unsigned long long begin = *(unsigned long long *)(group + 0x60), end = *(unsigned long long *)(group + 0x68);
        ULONGLONG now = GetTickCount64();

        if (read_game_memory(img + CAR_POOL_QUEUE_PTR_RVA, &qptr, sizeof qptr) && qptr &&
            read_game_memory((unsigned char *)(ULONG_PTR)qptr + 8, queue, 0x10)) {
            have_queue = 1;
        }
        if (end > begin && (end - begin) / CAR_POOL_SLOT <= CAR_POOL_SLOTS_MAX && (end - begin) % CAR_POOL_SLOT == 0) {
            if (now >= g_car_pool_scan_at || !g_car_pool_seen_any) {
                g_car_pool_scan_at = now + 500;
                if (read_game_memory((const void *)(ULONG_PTR)begin, g_car_pool_slots, (SIZE_T)(end - begin))) {
                    car_pool_scan((unsigned)((end - begin) / CAR_POOL_SLOT), held);
                    scanned = 1;
                }
            } else {
                memcpy(held, g_car_pool_seen + 5, sizeof held);
                scanned = g_car_pool_seen_any;
            }
        }
        car_pool_state_line(group, queue, have_queue, scanned, held);
    }
    return result;
}

/* Simulation::FindOrQueueCarLoadForNetVehicle (game+0xfd08b0) creates the load request of a net vehicle: its car id (info+0x234 of the pending vehicle,
 * handle+0x110) decides whether the stand-in id pool is asked, and the request starts in state 1, or in state 12 (invalid) when no id came out. Logged
 * once per call: the vehicle's id and type before, and the request's state and the vehicle's id after. 300 lines at most. */
#define CAR_POOL_FIND_RVA 0xfd08b0
typedef void *(*car_pool_find_fn)(void *, void *, void *, void *, void *, unsigned);
static car_pool_find_fn g_car_pool_find_orig;
static unsigned g_car_pool_find_lines;

static int car_pool_vehicle_info(unsigned long long vehicle, int *id, int *type)
{
    unsigned char info[4], kind[4];

    if (!vehicle || !read_game_memory((const void *)(ULONG_PTR)(vehicle + 0x110 + 0x234), info, sizeof info) ||
        !read_game_memory((const void *)(ULONG_PTR)(vehicle + 0x110 + 0x488), kind, sizeof kind)) {
        return 0;
    }
    *id = (short)(info[0] | (info[1] << 8));
    *type = (int)(kind[0] | (kind[1] << 8) | (kind[2] << 16) | ((unsigned)kind[3] << 24));
    return 1;
}

static void *car_pool_find(void *self, void *out, void *vehicle_ref, void *manager, void *extra, unsigned priority)
{
    unsigned long long vehicle = 0, request = 0;
    int id_before = 0, type = 0, id_after = 0, type_after = 0, have_before = 0, have_after;
    unsigned state = 99;
    void *result;

    if (g_car_pool_find_lines < 300 && vehicle_ref && read_game_memory(vehicle_ref, &vehicle, sizeof vehicle)) {
        have_before = car_pool_vehicle_info(vehicle, &id_before, &type);
    }
    result = g_car_pool_find_orig(self, out, vehicle_ref, manager, extra, priority);
    if (have_before && g_car_pool_find_lines < 300) {
        g_car_pool_find_lines++;
        if (out && read_game_memory(out, &request, sizeof request) && request) {
            read_game_memory((const void *)(ULONG_PTR)(request + 0x20), &state, sizeof state);
        }
        have_after = car_pool_vehicle_info(vehicle, &id_after, &type_after);
        log_begin();
        log_raw("carpool: load request for vehicle id ");
        log_dec((unsigned long)(unsigned short)id_before);
        log_raw(" type ");
        log_dec((unsigned long)type);
        log_raw(" -> state ");
        log_dec(state);
        log_raw(", id then ");
        if (have_after) {
            log_dec((unsigned long)(unsigned short)id_after);
        } else {
            log_raw("unreadable");
        }
        log_raw("\n");
    }
    return result;
}

static void car_pool_install_find(unsigned char *img)
{
    static const unsigned char find_head[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};

    if (code_is("carpool", "the net vehicle load request builder", img, CAR_POOL_FIND_RVA, find_head, sizeof find_head)) {
        plat_hook("carpool: load request log, passive", img + CAR_POOL_FIND_RVA, (void *)car_pool_find, (void **)&g_car_pool_find_orig);
    }
}

static int patch_car_pool(void)
{
    /* FindOrQueue +0x174: `xor eax,eax; movzx edi,ax; lea ebx,[rax+0xb]; jmp`: the stand-in id range 0..11 of vehicle types 0, 1, 3 and 4 (players) */
    static const unsigned char ids_head[] = {0x33, 0xC0, 0x0F, 0xB7, 0xF8, 0x8D, 0x58, 0x0B, 0xEB, 0x03};
    static const unsigned char head[] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x40, 0xB8, 0xFE, 0xFF, 0xFF, 0xFF};
    static const unsigned char native[] = {0x41, 0xB9, 0x0B, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x44, 0x0F, 0x49, 0xCA};   /* +0x26 */
    static const unsigned char clamp[] = {0x41, 0x83, 0xF9, 0x0C, 0x48, 0x0F, 0x4D, 0xC1};                            /* +0x49 */
    static const unsigned char nop4[] = {0x90, 0x90, 0x90, 0x90};
    static const unsigned char tick_head[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
    unsigned char *img = find_fh2_image();
    unsigned char slots[4] = {0, 0, 0, 0};
    int want = car_pool_from_env(), done = 0;
    char diag[8];

    if (!img) {
        return 0;
    }
    if (!want) {
        /* FH2MP_CAR_DIAG=1: only the passive log, the cache is left as the game builds it */
        if (GetEnvironmentVariableA("FH2MP_CAR_DIAG", diag, sizeof diag) && diag[0] == '1' &&
            code_is("carpool", "the car cache's tick", img, CAR_POOL_TICK_RVA, tick_head, sizeof tick_head)) {
            plat_hook("carpool: cache state, passive", img + CAR_POOL_TICK_RVA, (void *)car_pool_tick, (void **)&g_car_pool_tick_orig);
            car_pool_install_find(img);
        }
        return 0;
    }
    if (!code_is("carpool", "the car cache's module init", img, CAR_POOL_RVA, head, sizeof head) ||
        !code_is("carpool", "the Non-LOD0 slot count (11)", img, CAR_POOL_RVA + 0x26, native, sizeof native) ||
        !code_is("carpool", "the clamp to 12 slots", img, CAR_POOL_RVA + 0x49, clamp, sizeof clamp) ||
        !code_is("carpool", "the stand-in id range of player vehicles", img, CAR_POOL_FIND_RVA + 0x174, ids_head, sizeof ids_head)) {
        return 0;
    }
    slots[0] = (unsigned char)want;
    done += plat_patch("carpool: Non-LOD0 car slots", img + CAR_POOL_RVA + 0x28, native + 2, slots, 4);
    done += plat_patch("carpool: no clamp to 12 slots", img + CAR_POOL_RVA + 0x4d, clamp + 4, nop4, 4);
    {
        unsigned char hi = (unsigned char)want;

        done += plat_patch("carpool: stand-in ids of player vehicles", img + CAR_POOL_FIND_RVA + 0x17b, ids_head + 7, &hi, 1);
    }
    if (done == 3) {
        if (code_is("carpool", "the car cache's tick", img, CAR_POOL_TICK_RVA, tick_head, sizeof tick_head)) {
            plat_hook("carpool: cache state, passive", img + CAR_POOL_TICK_RVA, (void *)car_pool_tick, (void **)&g_car_pool_tick_orig);
            car_pool_install_find(img);
        }
        log_begin();
        log_raw("carpool: remote car slots 11 -> ");
        log_dec((unsigned long)want);
        log_raw(", player vehicle ids 0..");
        log_dec((unsigned long)want);
        log_raw(" (FH2MP_CAR_SLOTS)\n");
    }
    return done == 3;
}
