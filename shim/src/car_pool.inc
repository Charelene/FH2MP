/* Remote car slots (CarCacheModule::vfunc_2, game+0x47a570).
 * The game builds a car cache at module init with two groups: "Player LOD0" with 1 slot (the local car) and "Non-LOD0" with min(NumAICars if set else 11,
 * 12) slots, a pre-built ForzaCar each. A remote player's car takes a Non-LOD0 slot (CarNetworkedModule::OnNetworkPlayerJoined -> request queue ->
 * helper_47ba20); with no free slot the request stays queued, so the 12th car of a world never exists and its RemoteCarPrediction is never reached. A real
 * client with 17 or 75 players showed exactly 11 remote cars (dev 2026-11-02).
 * Patched: the 11 (imm32 of `mov r9d, 0xb` at +0x28) becomes FH2MP_CAR_SLOTS, and the clamp to 12 (`cmovge rax, rcx` at +0x4d) is nopped, so the group
 * holds that many slots. A user-set NumAICars still wins, unclamped. Not patched: NumAICars itself (it also sets AI/Drivatar counts). Unset FH2MP_CAR_SLOTS
 * changes nothing; the value is 11 to 75. Each slot is a whole ForzaCar built on demand: memory per slot was not measured. 75 slots alone made every
 * remote car land in load state 12: every net vehicle takes a stand-in id from a fixed range by its type (FindOrQueue 0xfd08b0: players 0..11, AI 12..29) and the
 * game's own Drivatar cars (types 0/1) take 0..11 before a player joins. The AI simulation holds 12 controllers in fixed arrays (FUN_140f44170 loops over
 * 0xc0 bytes), so ids past 11 crash (FH2MP_CAR_IDS=1 raises them anyway: game+0xf2b32d, then game+0xf445ec). FH2MP_CAR_NODRIVATARS=1 stops the game loading
 * its own Drivatars so 12 players fit in ids 0..11, see below. FH2MP_CAR_NOAI=1 sets NumAICars to 0 after the cache is built (late for the AI module). FH2MP_CAR_DIAG=1
 * installs only the passive log, with the cache left as the game builds it. */
#define CAR_POOL_RVA 0x47a570
#define CAR_POOL_NATIVE 11
#define CAR_POOL_MAX 75

/* Default profile (every player, no env needed): the setup that held 72 cars on a real client (dev 2026-11-03, 71 bots + 1 player, all loaded, 60 fps):
 * FH2MP_CAR_SLOTS=75, FH2MP_CAR_NODRIVATARS=x, FH2MP_CAR_NOAICTL=1, FH2MP_CAR_IDS=1, FH2MP_CAR_VIEWLOD=1. A world of 12 players or fewer is not changed by it
 * (the slots are only filled by cars that exist). Each of those variables still wins when it is set; FH2MP_NO_CARSLOTS=1 turns the whole profile off and
 * leaves the game's 11 remote cars. Not measured: memory with 75 slots in a small world. */
static int car_pool_profile_off(void)
{
    char v[8];

    return GetEnvironmentVariableA("FH2MP_NO_CARSLOTS", v, sizeof v) && v[0] == '1';
}

static int car_pool_unset(const char *name)
{
    char v[2];

    return GetEnvironmentVariableA(name, v, sizeof v) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
}

/* a switch the profile turns on: its own variable wins when it is set ('1' on, anything else off) */
static int car_pool_profile_flag(const char *name)
{
    char v[8];

    if (car_pool_unset(name)) {
        return !car_pool_profile_off();
    }
    return GetEnvironmentVariableA(name, v, sizeof v) && v[0] == '1';
}

static int car_pool_from_env(void)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA("FH2MP_CAR_SLOTS", v, sizeof v);
    int i, total = 0;

    if (car_pool_unset("FH2MP_CAR_SLOTS")) {
        return car_pool_profile_off() ? 0 : CAR_POOL_MAX;
    }
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

/* FH2MP_CAR_NOAI=1: once, from the first tick of the cache (it has been built by then), NumAICars (options+0x2538, default -1) becomes 0. The options
 * singleton is CForzaCommandLineParameters_GetInstance (game+0x295940). */
#define CAR_POOL_OPTIONS_RVA 0x295940
#define CAR_POOL_NUMAICARS 0x2538
static int g_car_pool_no_ai_done;
typedef unsigned char *(*car_pool_options_fn)(void);
static car_pool_options_fn g_car_pool_options;      /* tests; NULL = the game's GetInstance */

static void car_pool_no_ai(void)
{
    unsigned char *img = find_fh2_image();
    char v[8];
    unsigned char *options;
    int before;

    if (g_car_pool_no_ai_done || !img) {
        return;
    }
    g_car_pool_no_ai_done = 1;
    if (!GetEnvironmentVariableA("FH2MP_CAR_NOAI", v, sizeof v) || v[0] != '1') {
        return;
    }
    options = (g_car_pool_options ? g_car_pool_options : (car_pool_options_fn)(img + CAR_POOL_OPTIONS_RVA))();
    if (!options || !read_game_memory(options + CAR_POOL_NUMAICARS, &before, sizeof before)) {
        return;
    }
    *(int *)(options + CAR_POOL_NUMAICARS) = 0;
    log_begin();
    log_raw("carpool: NumAICars ");
    log_dec((unsigned long)(unsigned int)before);
    log_raw(" -> 0 (FH2MP_CAR_NOAI): the game does not load its own festival cars\n");
}

static void *g_car_cache_self;      /* the CarCacheModule, remembered from its tick (the car wait below reads its Non-LOD0 group) */

static unsigned long long car_pool_tick(void *self, void *a, void *b, void *c)
{
    unsigned char *img = find_fh2_image();
    unsigned char group[0xa0];
    unsigned queue[6] = {0, 0, 0, 0, 0, 0}, held[CAR_POOL_HELD];
    unsigned long long qptr = 0;
    unsigned long long result = g_car_pool_tick_orig(self, a, b, c);
    int have_queue = 0, scanned = 0;

    g_car_cache_self = self;
    car_pool_no_ai();

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

#define CAR_POOL_SHARED_ID2 0x4000          /* the second half of the key of a car that shares an id, plus a counter */
static unsigned g_car_pool_share_next;        /* how many player cars shared an id so far */
static int g_car_pool_shared_now;             /* set by the id search when it handed out an id already in use, read by FindOrQueue's wrapper */
static int g_car_pool_cur_type = -1;          /* the vehicle type FindOrQueue is working on, -1 outside it (the call runs under the loader's lock) */

static void *car_pool_find(void *self, void *out, void *vehicle_ref, void *manager, void *extra, unsigned priority)
{
    unsigned long long vehicle = 0, request = 0;
    int id_before = 0, type = 0, id_after = 0, type_after = 0, have_before = 0, have_after;
    unsigned state = 99;
    void *result;

    if (vehicle_ref && read_game_memory(vehicle_ref, &vehicle, sizeof vehicle)) {
        have_before = car_pool_vehicle_info(vehicle, &id_before, &type);
    }
    g_car_pool_cur_type = have_before ? type : -1;
    g_car_pool_shared_now = 0;
    result = g_car_pool_find_orig(self, out, vehicle_ref, manager, extra, priority);
    g_car_pool_cur_type = -1;
    if (g_car_pool_shared_now && vehicle) {
        /* The car's key is (id, id2) (CarUniqueId at info+0x234, read by CarLabelComponent and the car events): a shared id needs its own second half or two cars have the same key. */
        *(unsigned short *)((unsigned char *)(ULONG_PTR)vehicle + 0x110 + 0x236) = (unsigned short)(CAR_POOL_SHARED_ID2 + g_car_pool_share_next);
    }
    g_car_pool_shared_now = 0;
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

/* Simulation::helper_fceee0 (game+0xfceee0, only called from FindOrQueue) returns the lowest stand-in id in [lo, hi] that no pending vehicle holds, 0xffff when
 * the range is full, and a vehicle with 0xffff starts its load request invalid (state 12) and is never drawn. Vehicle types 0, 1, 3 and 4 share 0..11. An id
 * past 11 is not an option: the AI simulation indexes arrays of 12 with it (dev 2026-11-02: game+0xf2b32d for the training table, then game+0xf445ec in the AI
 * update loop even with no controller attached). With FH2MP_CAR_IDS=1 a network player's car (type 3) that finds the range full takes an id already in use
 * (1..11 in turn): such cars are driven by the network and, with FH2MP_CAR_NOAICTL, have no controller to share. */
#define CAR_POOL_IDS_RVA 0xfceee0
#define CAR_POOL_IDS_PLAYER_TYPE 3
#define CAR_POOL_IDS_NONE 0xffff
typedef unsigned short (*car_pool_ids_fn)(void *, unsigned short, unsigned short, void *);
static car_pool_ids_fn g_car_pool_ids_orig;
static int g_car_pool_share_ids;

static unsigned short car_pool_ids(void *manager, unsigned short lo, unsigned short hi, void *extra)
{
    unsigned short id = g_car_pool_ids_orig(manager, lo, hi, extra);

    if (id == CAR_POOL_IDS_NONE && g_car_pool_share_ids && g_car_pool_cur_type == CAR_POOL_IDS_PLAYER_TYPE && lo == 0 && hi == 0x0b) {
        id = (unsigned short)(1 + g_car_pool_share_next++ % 11);
        g_car_pool_shared_now = 1;
        if (g_car_pool_share_next <= 5) {
            log_begin();
            log_raw("carpool: stand-in ids full, a player car shares id ");
            log_dec(id);
            log_raw(" (FH2MP_CAR_IDS)\n");
        }
    }
    return id;
}

static void car_pool_install_find(unsigned char *img)
{
    static const unsigned char find_head[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};

    if (code_is("carpool", "the net vehicle load request builder", img, CAR_POOL_FIND_RVA, find_head, sizeof find_head)) {
        plat_hook("carpool: load request log, passive", img + CAR_POOL_FIND_RVA, (void *)car_pool_find, (void **)&g_car_pool_find_orig);
    }
}

/* FH2MP_CAR_NOAICTL=1: a remote player's car gets no AI controller. Car::helper_f74050 (game+0xf74050, run when a car is initialised with its flag set)
 * calls FUN_140f73e10 (game+0xf73e10) to attach a RacingAI controller; the controllers live in fixed arrays of 12 (FUN_140f44170 loops over 0xc0 bytes), which
 * is why a car with a stand-in id past 11 crashed the game at game+0xf445ec. A car driven by the network (ForzaCar+0x5aa8 = its NetworkCar, whose vslot 0xc0 is
 * true for the local player's own car) needs none. Only remote network cars are skipped. */
#define CAR_POOL_AICTL_RVA 0xf73e10
#define CAR_POOL_NETWORK_CAR 0x5aa8
#define CAR_POOL_NETWORK_CAR_IS_LOCAL 0xc0
typedef void (*car_pool_aictl_fn)(void *);
static car_pool_aictl_fn g_car_pool_aictl_orig;
static unsigned g_car_pool_aictl_skipped;

static int car_pool_is_remote_network_car(void *car)
{
    unsigned char *network_car = car ? *(unsigned char **)((unsigned char *)car + CAR_POOL_NETWORK_CAR) : NULL;
    void **vtable;
    unsigned char (*is_local)(void *);

    if (!network_car) {
        return 0;
    }
    vtable = *(void ***)network_car;
    if (!vtable) {
        return 0;
    }
    is_local = (unsigned char (*)(void *))vtable[CAR_POOL_NETWORK_CAR_IS_LOCAL / sizeof(void *)];
    return is_local && is_local(network_car) == 0;
}

static void car_pool_aictl(void *car)
{
    if (car_pool_is_remote_network_car(car)) {
        if (g_car_pool_aictl_skipped++ < 5) {
            log_begin();
            log_raw("carpool: no AI controller for remote network car #");
            log_dec(g_car_pool_aictl_skipped);
            log_raw(" (FH2MP_CAR_NOAICTL)\n");
        }
        return;
    }
    g_car_pool_aictl_orig(car);
}

/* The name tags (CarLabelCacheModule::vfunc_2, game+0x47a830) are a pool of 12 label objects made at module start (`inc edi; cmp edi, 0xc; jb` at +0x146); every
 * remote car takes one. The 13th remote car found the list empty and CarLabelComponent::OnAdded dereferenced its sentinel (game+0x46dfc0 reading 0x6233, dev
 * 2026-11-02, the 14th car both times). The pool is made FH2MP_CAR_SLOTS + 2 big (at most 127: the compare is an imm8). */
#define CAR_POOL_LABELS_RVA 0x47a830
#define CAR_POOL_LABELS_CMP 0x146           /* from the start of the function to `cmp edi, 0xc` */

/* FH2MP_CAR_NOBUDGET=1: the allocator's category budgets are off. Mem::MemoryManager::AllocateFromHeap (game+0x125d850) asks FUN_14125de50 (game+0x125de50)
 * first; with budget enforcement on (bit 4 of the byte at +0x10a3f6 of the tracker) a category that has a budget and not enough left makes the allocation return
 * NULL, no log line. The 14th remote car's CPresentationView constructor stores its Mem::Alloc(0x3ea0) at +0x4d8 unchecked and the render thread wrote through it
 * (game+0x1334f34, dev 2026-11-03). Budgets are an Xbox One memory model; the check is made to return true. Also used by two graphics allocators. */
#define CAR_POOL_BUDGET_RVA 0x125de50

/* The name tag allocator (EntityModel::CarLabelComponent::helper_46df80, game+0x46df80) takes the last label of the pool's free list (the list at module+0x60, its
 * head's _Prev) without looking whether there is one: an empty list's head holds no value, and the 0x6233 it read there was dereferenced (game+0x46dfc0). Dev
 * 2026-11-04: a client joining a world of 71 players crashed when the 13th car asked for a label ("SYNCING Player 13 / 71"); in the dump the module (vtable
 * game+0x2c817a8) had 12 in use and 0 free, so its pool was the stock 12 although the bound above was patched in memory (the same module in the other client: 77).
 * The game handles a car without a label everywhere else (OnAdded takes one only when +0x130 is null, and most handlers test it), so an empty pool answers
 * "no label" and the car has no name tag. The pool maker is wrapped to log how big each pool was built, which says whether the bound was patched in time. */
#define CAR_POOL_LABEL_TAKE_RVA 0x46df80
typedef void **(*car_pool_label_take_fn)(void *, void **);
static car_pool_label_take_fn g_car_pool_label_take_orig;
static unsigned g_car_pool_label_empty;

static void **car_pool_label_take(void *self, void **out)
{
    unsigned char *list = NULL, *last = NULL;

    if (self && out && read_game_memory((unsigned char *)self + 0x60, &list, sizeof list) && list &&
        read_game_memory(list + 8, &last, sizeof last) && last == list) {
        if (g_car_pool_label_empty++ < 5) {
            unsigned long long used = 0;

            read_game_memory((unsigned char *)self + 0x58, &used, sizeof used);
            log_begin();
            log_raw("carpool: name tag pool empty with ");
            log_dec((unsigned long)used);
            log_raw(" in use, the car gets no name tag (#");
            log_dec(g_car_pool_label_empty);
            log_raw(")\n");
        }
        out[0] = NULL;
        out[1] = NULL;
        return out;
    }
    return g_car_pool_label_take_orig(self, out);
}

typedef void (*car_pool_labels_make_fn)(void *);
static car_pool_labels_make_fn g_car_pool_labels_make_orig;

static void car_pool_labels_make(void *module)
{
    unsigned char *img = find_fh2_image();
    unsigned char bound = 0;
    unsigned long long free_now = 0;

    g_car_pool_labels_make_orig(module);
    if (img && module && read_game_memory(img + CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4, &bound, 1) &&
        read_game_memory((unsigned char *)module + 0x68, &free_now, sizeof free_now)) {
        log_begin();
        log_raw("carpool: name tag pool built with ");
        log_dec((unsigned long)free_now);
        log_raw(" labels, loop bound in memory ");
        log_dec(bound);
        log_raw("\n");
    }
}

/* FH2MP_CAR_ALLOCDIAG=1: the first 30 allocations Mem::MemoryManager::AllocateFromHeap (game+0x125d850) fails (returns NULL) are logged with their size, tag
 * (the category: high byte) and heap mode (manager+0x18: below 2 HeapAlloc, otherwise XMemAlloc). Passive. */
#define CAR_POOL_ALLOC_RVA 0x125d850
typedef void *(*car_pool_alloc_fn)(void *, unsigned long long, unsigned short, unsigned long long *);
static car_pool_alloc_fn g_car_pool_alloc_orig;
static unsigned g_car_pool_alloc_failed;

static void *car_pool_alloc(void *manager, unsigned long long size, unsigned short tag, unsigned long long *extra)
{
    void *result = g_car_pool_alloc_orig(manager, size, tag, extra);

    if (!result && size && g_car_pool_alloc_failed < 30) {
        g_car_pool_alloc_failed++;
        log_begin();
        log_raw("carpool: allocation failed, ");
        log_dec((unsigned long)size);
        log_raw(" bytes, tag 0x");
        log_hex(tag, 4);
        log_raw(", heap mode ");
        log_dec(manager ? *((unsigned char *)manager + 0x18) : 99);
        log_raw("\n");
    }
    return result;
}

/* FH2MP_CAR_VIEWGUARD=1: the render thread's two callers of CPresentationView::helper_1334e30 (helper_1355ad0, game+0x1355ad0, and FUN_14135b640, game+0x135b640)
 * pass *(view + 0x4d8) as its out pointer and the function writes through it first thing. For the 14th remote car that pointer was NULL (game+0x1334f34 writing 0x0,
 * dev 2026-11-03; not an allocation failure: the allocator log was empty). Such a view is not rendered this frame, and the first ten are logged with the view's
 * vtable (its class) so the cause can be found. */
#define CAR_POOL_VIEW_A_RVA 0x1355ad0
#define CAR_POOL_VIEW_B_RVA 0x135b640
#define CAR_POOL_VIEW_FRAME 0x4d8
typedef void (*car_pool_view_fn)(void *);
static car_pool_view_fn g_car_pool_view_a_orig, g_car_pool_view_b_orig;
static unsigned g_car_pool_view_skipped;

static int car_pool_view_ready(void *view, const char *which)
{
    if (view && *(void **)((unsigned char *)view + CAR_POOL_VIEW_FRAME)) {
        return 1;
    }
    if (g_car_pool_view_skipped++ < 10) {
        unsigned char *img = find_fh2_image();
        unsigned long long vtable = view ? *(unsigned long long *)view : 0;

        log_begin();
        log_raw("carpool: view without frame data skipped (");
        log_raw(which);
        log_raw("), #");
        log_dec(g_car_pool_view_skipped);
        log_raw(", vtable game+0x");
        log_hex(vtable > (unsigned long long)(ULONG_PTR)img ? vtable - (unsigned long long)(ULONG_PTR)img : 0, 8);
        log_raw("\n");
    }
    return 0;
}

static void car_pool_view_a(void *view)
{
    if (car_pool_view_ready(view, "a")) {
        g_car_pool_view_a_orig(view);
    }
}

static void car_pool_view_b(void *view)
{
    if (car_pool_view_ready(view, "b")) {
        g_car_pool_view_b_orig(view);
    }
}

static int car_pool_env_flag(const char *name)
{
    char v[8];

    return GetEnvironmentVariableA(name, v, sizeof v) && v[0] == '1';
}

/* FH2MP_CAR_NODRIVATARS parts: "1" is all of them; letters pick some (to find which one a build of the game does not like): c = the cache slot count ignores
 * NumAICars, x = Drivatar maximum 0, n = Drivatar minimum 0, a = festival AI count 0 for the connect call. */
#define CAR_POOL_PART_CACHE 1
#define CAR_POOL_PART_MAX 2
#define CAR_POOL_PART_MIN 4
#define CAR_POOL_PART_AI 8

static int car_pool_parts(void)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", v, sizeof v);
    int parts = 0;
    unsigned i;

    if (car_pool_unset("FH2MP_CAR_NODRIVATARS")) {
        return car_pool_profile_off() ? 0 : CAR_POOL_PART_MAX;
    }

    if (n == 0 || n >= sizeof v) {
        return 0;
    }
    if (v[0] == '1' && v[1] == 0) {
        return CAR_POOL_PART_CACHE | CAR_POOL_PART_MAX | CAR_POOL_PART_MIN | CAR_POOL_PART_AI;
    }
    for (i = 0; v[i]; i++) {
        parts |= v[i] == 'c' ? CAR_POOL_PART_CACHE : v[i] == 'x' ? CAR_POOL_PART_MAX : v[i] == 'n' ? CAR_POOL_PART_MIN : v[i] == 'a' ? CAR_POOL_PART_AI : 0;
    }
    return parts;
}

/* FH2MP_CAR_NODRIVATARS=1: the game does not load its own AI cars (Drivatars), which are vehicle types 0 and 1 and take the stand-in ids 0..11 and cache slots
 * before a player's car can. Three things make them: the Free Roam Drivatar component's MaxDrivatarsToAcquire (member +0xf0 of the component, set by its
 * property setter game+0x5430d0 from the entity XML, 16) and MinDrivatarsForFreeroam (+0xb0, setter +0x653780) are set to 0 by the setters themselves, and
 * the festival AI count that AICarLoadingModule::ConnectSignalHandlers (game+0x720960) reads from NumAICars (options+0x2538) is 0 for the call. The cache's own
 * slot count would follow a NumAICars of 0, so its `cmovns r9d, edx` (cache init +0x2e) is nopped: the count is FH2MP_CAR_SLOTS whatever NumAICars says. */
#define CAR_POOL_DRIVATAR_MAX_SETTER_RVA 0x5430d0
#define CAR_POOL_DRIVATAR_MIN_SETTER_RVA 0x653780
#define CAR_POOL_AIMODULE_RVA 0x720960
typedef void (*car_pool_ai_fn)(void *);
static car_pool_ai_fn g_car_pool_ai_orig;

static void car_pool_ai_connect(void *module)
{
    unsigned char *img = find_fh2_image();
    unsigned char *options;
    int saved = 0, have = 0;

    options = img ? (g_car_pool_options ? g_car_pool_options : (car_pool_options_fn)(img + CAR_POOL_OPTIONS_RVA))() : NULL;
    if (options && read_game_memory(options + CAR_POOL_NUMAICARS, &saved, sizeof saved)) {
        have = 1;
        *(int *)(options + CAR_POOL_NUMAICARS) = 0;
    }
    g_car_pool_ai_orig(module);
    if (have) {
        *(int *)(options + CAR_POOL_NUMAICARS) = saved;
    }
}

/* FH2MP_CAR_VIEWLOD=1: the 14th remote car crashed the render thread because CPresentationView::vfunc_42 (game+0x1344820) keeps one LOD value per presentation
 * object in a table of 32 dwords at view+0x454 (cleared by a memset of 0x80 bytes) and the object index reached 33: the store at game+0x1346639 (`mov [rsi+r13*4+454h],
 * r14d`, r13 = 0x21) zeroed the low dword of the frame data pointer behind the table at +0x4d8, which the render thread then wrote through (TTD trace of dev 2026-11-03,
 * last write to the field, view 0x19a83ffca00, left 0x19a00000000). Only vfunc_42 uses the table (the memset, three stores, one load). The table is moved to the end
 * of the view, which CPresentation::vfunc_19 (game+0x1302160) allocates with Mem::Alloc(0x24df0): the allocation grows by the table, the displacement of the six
 * accesses and the memset size follow, and a wrapper clears the new part (the constructor only clears what it knows). Nothing else in the exe addresses a view's
 * bytes past 0x24df0; the tail field at +0x24de0 stays where it is. */
#define CAR_POOL_VIEW_NEW_RVA 0x1302160
#define CAR_POOL_VIEW_SIZE 0x24df0                  /* the view's allocation, and where the new table starts */
#define CAR_POOL_VIEWLOD_ENTRIES 1024
#define CAR_POOL_VIEWLOD_BYTES (CAR_POOL_VIEWLOD_ENTRIES * 4)
typedef void *(*car_pool_view_new_fn)(void *);
static car_pool_view_new_fn g_car_pool_view_new_orig;

static void *car_pool_view_new(void *presentation)
{
    unsigned char *view = g_car_pool_view_new_orig(presentation);

    if (view) {
        memset(view + CAR_POOL_VIEW_SIZE, 0, CAR_POOL_VIEWLOD_BYTES);
    }
    return view;
}

static void car_pool_put32(unsigned char *at, unsigned long v)
{
    at[0] = (unsigned char)v;
    at[1] = (unsigned char)(v >> 8);
    at[2] = (unsigned char)(v >> 16);
    at[3] = (unsigned char)(v >> 24);
}

static int car_pool_install_viewlod(unsigned char *img)
{
    static const unsigned char new_head[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xC6, 0x81, 0xA8, 0x26, 0x00, 0x00, 0x01,
                                             0xB9, 0xF0, 0x4D, 0x02, 0x00};                                          /* ... mov ecx, 0x24df0 (the alloc size) */
    static const unsigned char clear_site[] = {0x48, 0x8D, 0x8E, 0x54, 0x04, 0x00, 0x00, 0x33, 0xD2, 0x41, 0xB8, 0x80, 0x00, 0x00, 0x00};   /* lea rcx,[rsi+454h]; xor edx,edx; mov r8d,80h */
    static const unsigned char store_a[] = {0x46, 0x89, 0xB4, 0xAE, 0x54, 0x04, 0x00, 0x00};                         /* mov [rsi+r13*4+454h], r14d */
    static const unsigned char store_b[] = {0x42, 0xC7, 0x84, 0xAE, 0x54, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};  /* mov dword [rsi+r13*4+454h], 1 */
    static const unsigned char store_c[] = {0x42, 0x89, 0x84, 0xAE, 0x54, 0x04, 0x00, 0x00};                         /* mov [rsi+r13*4+454h], eax */
    static const unsigned char load_d[] = {0x42, 0x8B, 0x84, 0xAE, 0x54, 0x04, 0x00, 0x00};                          /* mov eax, [rsi+r13*4+454h] */
    static const unsigned char old_disp[] = {0x54, 0x04, 0x00, 0x00};
    static const unsigned char old_size[] = {0x80, 0x00, 0x00, 0x00};
    static const unsigned char old_alloc[] = {0xF0, 0x4D, 0x02, 0x00};
    static const struct { unsigned rva; unsigned disp_at; const unsigned char *code; unsigned n; const char *what; } sites[] = {
        {0x1346332, 3, clear_site, sizeof clear_site, "the LOD table clear"},
        {0x1346639, 4, store_a, sizeof store_a, "the LOD store (a)"},
        {0x1346649, 4, store_b, sizeof store_b, "the LOD store (b)"},
        {0x1346664, 4, store_c, sizeof store_c, "the LOD store (c)"},
        {0x134666c, 4, load_d, sizeof load_d, "the LOD load"},
    };
    unsigned char table[4], size[4], alloc[4];
    int done = 0, expected = 8;
    unsigned i;

    for (i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        if (!code_is("carpool", sites[i].what, img, sites[i].rva, sites[i].code, sites[i].n)) {
            return 0;
        }
    }
    if (!code_is("carpool", "the view allocation", img, CAR_POOL_VIEW_NEW_RVA, new_head, sizeof new_head)) {
        return 0;
    }
    car_pool_put32(table, CAR_POOL_VIEW_SIZE);
    car_pool_put32(size, CAR_POOL_VIEWLOD_BYTES);
    car_pool_put32(alloc, CAR_POOL_VIEW_SIZE + CAR_POOL_VIEWLOD_BYTES);
    /* the clearing wrapper first and the bigger allocation before any access moves: a moved access on a small view would run off its end */
    done += plat_hook("carpool: view allocation cleared past its old end", img + CAR_POOL_VIEW_NEW_RVA, (void *)car_pool_view_new, (void **)&g_car_pool_view_new_orig) ? 1 : 0;
    if (done != 1) {
        return 0;
    }
    done += plat_patch("carpool: view allocation grown", img + CAR_POOL_VIEW_NEW_RVA + 0x15, old_alloc, alloc, 4);
    if (done != 2) {
        return 0;
    }
    for (i = 0; i < sizeof sites / sizeof sites[0]; i++) {
        done += plat_patch(sites[i].what, img + sites[i].rva + sites[i].disp_at, old_disp, table, 4);
    }
    done += plat_patch("carpool: LOD table clear size", img + sites[0].rva + 11, old_size, size, 4);
    log_begin();
    if (done == expected) {
        log_raw("carpool: view LOD table 32 -> ");
        log_dec(CAR_POOL_VIEWLOD_ENTRIES);
        log_raw(" entries at the end of the view (FH2MP_CAR_VIEWLOD)\n");
        return 1;
    }
    log_raw("carpool: view LOD table only partly moved, ");
    log_dec((unsigned long)done);
    log_raw(" of 8 steps done (FH2MP_CAR_VIEWLOD)\n");
    return 0;
}

static int patch_car_pool(void)
{
    static const unsigned char head[] = {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x40, 0xB8, 0xFE, 0xFF, 0xFF, 0xFF};
    static const unsigned char native[] = {0x41, 0xB9, 0x0B, 0x00, 0x00, 0x00, 0x85, 0xD2, 0x44, 0x0F, 0x49, 0xCA};   /* +0x26 */
    static const unsigned char clamp[] = {0x41, 0x83, 0xF9, 0x0C, 0x48, 0x0F, 0x4D, 0xC1};                            /* +0x49 */
    static const unsigned char nop4[] = {0x90, 0x90, 0x90, 0x90};
    static const unsigned char labels_head[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20};
    static const unsigned char labels_cmp[] = {0xFF, 0xC7, 0x83, 0xFF, 0x0C, 0x0F, 0x82};                              /* inc edi; cmp edi, 0xc; jb */
    static const unsigned char label_take_head[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x28, 0xFE, 0xFF, 0xFF, 0xFF,
                                                    0x48, 0x89, 0x5C, 0x24, 0x50, 0x48, 0x89, 0x6C, 0x24, 0x60, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xE9, 0x33, 0xC0, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8B, 0x59, 0x60};
    static const unsigned char budget_head[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};
    static const unsigned char budget_true[] = {0xB0, 0x01, 0xC3};               /* mov al, 1; ret */
    static const unsigned char tick_head[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
    static const unsigned char find_head[] = {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};
    static const unsigned char ids_head[] = {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x40, 0xC8, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x10};
    static const unsigned char aictl_head[] = {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x50, 0x03, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x40, 0xFE, 0xFF, 0xFF, 0xFF};
    static const unsigned char max_setter[] = {0x44, 0x89, 0x82, 0xF0, 0x00, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC};     /* mov [rdx+0xf0], r8d; ret; padding */
    static const unsigned char min_setter[] = {0x44, 0x89, 0x82, 0xB0, 0x00, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC};     /* mov [rdx+0xb0], r8d; ret; padding */
    static const unsigned char max_zero[] = {0xC7, 0x82, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC3};      /* mov dword [rdx+0xf0], 0; ret */
    static const unsigned char min_zero[] = {0xC7, 0x82, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC3};      /* mov dword [rdx+0xb0], 0; ret */
    static const unsigned char ai_head[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    unsigned char slots[4] = {0, 0, 0, 0};
    int want = car_pool_from_env(), done = 0, expected = 3;
    unsigned char labels = 0;
    int parts = car_pool_parts(), ids = car_pool_profile_flag("FH2MP_CAR_IDS"), aictl = car_pool_profile_flag("FH2MP_CAR_NOAICTL"), budget = car_pool_env_flag("FH2MP_CAR_NOBUDGET");

    if (!img) {
        return 0;
    }
    if (car_pool_env_flag("FH2MP_CAR_VIEWGUARD")) {
        static const unsigned char view_head[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x48};

        if (code_is("carpool", "the view render call a", img, CAR_POOL_VIEW_A_RVA, view_head, sizeof view_head) &&
            code_is("carpool", "the view render call b", img, CAR_POOL_VIEW_B_RVA, view_head, sizeof view_head)) {
            plat_hook("carpool: views without frame data skipped (a)", img + CAR_POOL_VIEW_A_RVA, (void *)car_pool_view_a, (void **)&g_car_pool_view_a_orig);
            plat_hook("carpool: views without frame data skipped (b)", img + CAR_POOL_VIEW_B_RVA, (void *)car_pool_view_b, (void **)&g_car_pool_view_b_orig);
        }
    }
    if (car_pool_profile_flag("FH2MP_CAR_VIEWLOD")) {
        car_pool_install_viewlod(img);
    }
    if (car_pool_env_flag("FH2MP_CAR_ALLOCDIAG")) {
        static const unsigned char alloc_head[] = {0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x02, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x58};

        if (code_is("carpool", "the heap allocator", img, CAR_POOL_ALLOC_RVA, alloc_head, sizeof alloc_head)) {
            plat_hook("carpool: failed allocations, passive", img + CAR_POOL_ALLOC_RVA, (void *)car_pool_alloc, (void **)&g_car_pool_alloc_orig);
        }
    }
    if (!want) {
        /* FH2MP_CAR_DIAG=1: only the passive log, the cache is left as the game builds it */
        if (car_pool_env_flag("FH2MP_CAR_DIAG") && code_is("carpool", "the car cache's tick", img, CAR_POOL_TICK_RVA, tick_head, sizeof tick_head)) {
            plat_hook("carpool: cache state, passive", img + CAR_POOL_TICK_RVA, (void *)car_pool_tick, (void **)&g_car_pool_tick_orig);
            car_pool_install_find(img);
        }
        return 0;
    }
    if (!code_is("carpool", "the car cache's module init", img, CAR_POOL_RVA, head, sizeof head) ||
        !code_is("carpool", "the Non-LOD0 slot count (11)", img, CAR_POOL_RVA + 0x26, native, sizeof native) ||
        !code_is("carpool", "the clamp to 12 slots", img, CAR_POOL_RVA + 0x49, clamp, sizeof clamp) ||
        !code_is("carpool", "the net vehicle load request builder", img, CAR_POOL_FIND_RVA, find_head, sizeof find_head) ||
        !code_is("carpool", "the name tag pool's start", img, CAR_POOL_LABELS_RVA, labels_head, sizeof labels_head) ||
        !code_is("carpool", "the name tag pool's size", img, CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP, labels_cmp, sizeof labels_cmp) ||
        !code_is("carpool", "the name tag allocator", img, CAR_POOL_LABEL_TAKE_RVA, label_take_head, sizeof label_take_head) ||
        !code_is("carpool", "the car cache's tick", img, CAR_POOL_TICK_RVA, tick_head, sizeof tick_head) ||
        (budget && !code_is("carpool", "the allocator's budget check", img, CAR_POOL_BUDGET_RVA, budget_head, sizeof budget_head)) ||
        (aictl && !code_is("carpool", "the AI controller attach", img, CAR_POOL_AICTL_RVA, aictl_head, sizeof aictl_head)) ||
        (ids && !code_is("carpool", "the stand-in id search", img, CAR_POOL_IDS_RVA, ids_head, sizeof ids_head)) ||
        ((parts & CAR_POOL_PART_MAX) && !code_is("carpool", "the Drivatar maximum setter", img, CAR_POOL_DRIVATAR_MAX_SETTER_RVA, max_setter, sizeof max_setter)) ||
        ((parts & CAR_POOL_PART_MIN) && !code_is("carpool", "the Drivatar minimum setter", img, CAR_POOL_DRIVATAR_MIN_SETTER_RVA, min_setter, sizeof min_setter)) ||
        ((parts & CAR_POOL_PART_AI) && !code_is("carpool", "the festival AI module's connect", img, CAR_POOL_AIMODULE_RVA, ai_head, sizeof ai_head))) {
        return 0;
    }
    slots[0] = (unsigned char)want;
    done += plat_patch("carpool: Non-LOD0 car slots", img + CAR_POOL_RVA + 0x28, native + 2, slots, 4);
    done += plat_patch("carpool: no clamp to 12 slots", img + CAR_POOL_RVA + 0x4d, clamp + 4, nop4, 4);
    labels = (unsigned char)(want + 2 > 127 ? 127 : want + 2);
    done += plat_patch("carpool: name tag pool", img + CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4, labels_cmp + 4, &labels, 1);
    if (budget) {
        expected++;
        done += plat_patch("carpool: no allocator budget", img + CAR_POOL_BUDGET_RVA, budget_head, budget_true, sizeof budget_true);
    }
    if (parts & CAR_POOL_PART_CACHE) {
        expected++;
        done += plat_patch("carpool: slot count ignores NumAICars", img + CAR_POOL_RVA + 0x2e, native + 8, nop4, 4);
    }
    if (parts & CAR_POOL_PART_MAX) {
        expected++;
        done += plat_patch("carpool: no Drivatars to acquire", img + CAR_POOL_DRIVATAR_MAX_SETTER_RVA, max_setter, max_zero, sizeof max_zero);
    }
    if (parts & CAR_POOL_PART_MIN) {
        expected++;
        done += plat_patch("carpool: no Drivatars needed for Free Roam", img + CAR_POOL_DRIVATAR_MIN_SETTER_RVA, min_setter, min_zero, sizeof min_zero);
    }
    if (done == expected) {
        plat_hook("carpool: cache state, passive", img + CAR_POOL_TICK_RVA, (void *)car_pool_tick, (void **)&g_car_pool_tick_orig);
        plat_hook("carpool: no name tag instead of a crash when the pool is empty", img + CAR_POOL_LABEL_TAKE_RVA, (void *)car_pool_label_take, (void **)&g_car_pool_label_take_orig);
        plat_hook("carpool: name tag pool size, passive", img + CAR_POOL_LABELS_RVA, (void *)car_pool_labels_make, (void **)&g_car_pool_labels_make_orig);
        car_pool_install_find(img);
        if (parts & CAR_POOL_PART_AI) {
            plat_hook("carpool: festival AI count 0", img + CAR_POOL_AIMODULE_RVA, (void *)car_pool_ai_connect, (void **)&g_car_pool_ai_orig);
        }
        if (aictl) {
            plat_hook("carpool: no AI controller for remote cars", img + CAR_POOL_AICTL_RVA, (void *)car_pool_aictl, (void **)&g_car_pool_aictl_orig);
        }
        if (ids && plat_hook("carpool: player cars share stand-in ids when they run out", img + CAR_POOL_IDS_RVA, (void *)car_pool_ids, (void **)&g_car_pool_ids_orig)) {
            g_car_pool_share_ids = 1;
        }
        log_begin();
        log_raw("carpool: remote car slots 11 -> ");
        log_dec((unsigned long)want);
        log_raw(parts ? ", no Drivatars, parts " : "");
        if (parts) {
            log_raw(parts & CAR_POOL_PART_CACHE ? "c" : "");
            log_raw(parts & CAR_POOL_PART_MAX ? "x" : "");
            log_raw(parts & CAR_POOL_PART_MIN ? "n" : "");
            log_raw(parts & CAR_POOL_PART_AI ? "a" : "");
        }
        log_raw(g_car_pool_share_ids ? ", player cars share stand-in ids (FH2MP_CAR_IDS)" : "");
        log_raw(" (FH2MP_CAR_SLOTS)\n");
    }
    return done == expected;
}


/* The wait for every player's car (NetworkWaitForAllPlayerCarsLoaded: 0x8e06b0 asks 0x7cbf10, which walks CarNetworkedModule's players, map at +0xa8, and
 * answers 0 while one has no car) has no timeout, and a client holds 11 remote cars at most (the cache's Non-LOD0 group). A world of 16 players showed
 * "SINCRONIZANDO <name> - 11 / 16" with its spinner for good (prod 2026-11-07): the 12th car waits for a slot nobody gives up, so the wait never ends and the
 * Free Roam flow that follows it ("Syncing traffic", Synchronization Complete, events) never starts. The cars that cannot load stay unloaded either way.
 * Hook on 0x7cbf10: while the game says 'not yet', the wait passes when (a) the group has no free slot, no assigned slot's car is still loading (request
 * state 1) and the wait has gone on for CAR_WAIT_FULL_MS: nothing is in flight, the rest can only wait for a leaver; or (b) nothing in the group changed for
 * CAR_WAIT_STALL_MS (a car that never loads for another reason). One log line per pass. The cache comes from the tick hook (car_pool_tick, or the small
 * one here when car_pool did not install its own). Without the cache the game's answer stands. Not verified in a running game. */
#define CAR_WAIT_RVA 0x7cbf10
#define CAR_WAIT_FULL_MS 1500
#define CAR_WAIT_STALL_MS 20000
#define CAR_WAIT_GAP_MS 2000                /* a call after this long a pause starts a new wait */
#define CAR_WAIT_REPORT_MS 20000            /* a wait this long is reported to the server once (carsel_sync.inc, defined further down) */
static int carsel_report_post(unsigned char code, unsigned detail, const char *text);
typedef unsigned long long (*car_wait_fn)(void *);
static car_wait_fn g_car_wait_orig;
static ULONGLONG g_car_wait_start, g_car_wait_last, g_car_wait_changed;
static unsigned g_car_wait_sig[4];
static int g_car_wait_full_logged, g_car_wait_reported;
static unsigned g_car_wait_passed;
static ULONGLONG (*g_car_wait_clock)(void);      /* tests; NULL = GetTickCount64 */

/* free slots, requests waiting for one, assigned slots whose car is loading (request state 1) and loaded (2) */
static int car_wait_group(unsigned *out)
{
    unsigned char group[0xa0];
    unsigned long long begin, end, rb, re;
    unsigned i, slots;

    if (!g_car_cache_self || !read_game_memory((unsigned char *)g_car_cache_self + CAR_POOL_GROUP, group, sizeof group)) {
        return 0;
    }
    begin = *(unsigned long long *)(group + 0x60);
    end = *(unsigned long long *)(group + 0x68);
    rb = *(unsigned long long *)(group + 0x78);
    re = *(unsigned long long *)(group + 0x80);
    if (end <= begin || (end - begin) % CAR_POOL_SLOT || (end - begin) / CAR_POOL_SLOT > CAR_POOL_SLOTS_MAX || re < rb) {
        return 0;
    }
    slots = (unsigned)((end - begin) / CAR_POOL_SLOT);
    out[0] = *(unsigned *)(group + 0x90);
    out[1] = (unsigned)((re - rb) / 8);
    out[2] = out[3] = 0;
    for (i = 0; i < slots; i++) {
        unsigned long long request = 0;
        unsigned state = 0;

        if (!read_game_memory((const void *)(ULONG_PTR)(begin + (unsigned long long)i * CAR_POOL_SLOT + 0x558), &request, sizeof request)) {
            return 0;
        }
        if (request && read_game_memory((const void *)(ULONG_PTR)(request + 0x598), &state, sizeof state)) {
            out[2] += state == 1;
            out[3] += state == 2;
        }
    }
    return 1;
}

static unsigned long long car_wait(void *module)
{
    unsigned long long answer = g_car_wait_orig(module);
    unsigned now_sig[4];
    ULONGLONG now = g_car_wait_clock ? g_car_wait_clock() : GetTickCount64();
    int why = 0;

    if (answer) {
        g_car_wait_start = 0;
        g_car_wait_full_logged = g_car_wait_reported = 0;
        return answer;
    }
    if (!g_car_wait_start || now - g_car_wait_last > CAR_WAIT_GAP_MS) {
        g_car_wait_start = g_car_wait_changed = now;
        memset(g_car_wait_sig, 0xff, sizeof g_car_wait_sig);
        g_car_wait_full_logged = g_car_wait_reported = 0;
    }
    g_car_wait_last = now;
    if (!car_wait_group(now_sig)) {
        return answer;
    }
    if (memcmp(now_sig, g_car_wait_sig, sizeof now_sig)) {
        memcpy(g_car_wait_sig, now_sig, sizeof now_sig);
        g_car_wait_changed = now;
    }
    if (!g_car_wait_reported && now - g_car_wait_start >= CAR_WAIT_REPORT_MS) {
        char text[160];
        const char *label[4] = {"; free slots ", ", cars waiting for a slot ", ", loading ", ", loaded "};
        unsigned i, n = 0;
        const char *head = "the wait for every player's car has lasted 20 s";
        g_car_wait_reported = 1;
        for (i = 0; head[i]; i++) text[n++] = head[i];
        for (i = 0; i < 4; i++) {
            char digits[12];
            unsigned d = 0, v = now_sig[i], j;
            const char *l;
            do {digits[d++] = (char)('0' + v % 10); v /= 10;} while (v);
            for (l = label[i]; *l && n < sizeof text - 12; l++) text[n++] = *l;
            for (j = d; j > 0 && n < sizeof text - 1; j--) text[n++] = digits[j - 1];
        }
        text[n] = 0;
        carsel_report_post(2, 20, text);
    }
    if (now_sig[0] == 0 && now_sig[2] == 0 && now - g_car_wait_changed >= CAR_WAIT_FULL_MS) {
        why = 1;
    } else if (now - g_car_wait_changed >= CAR_WAIT_STALL_MS) {
        why = 2;
    }
    if (!why) {
        return answer;
    }
    g_car_wait_passed++;
    log_begin();
    log_raw("carwait: the wait for every player's car is let through after ");
    log_dec((unsigned long)((now - g_car_wait_start) / 1000));
    log_raw(" s: ");
    log_raw(why == 1 ? "all car slots are taken and none is loading, the other players' cars show once a slot frees up" : "nothing in the car cache changed for 20 s");
    log_raw(" (free slots ");
    log_dec(now_sig[0]);
    log_raw(", cars waiting for a slot ");
    log_dec(now_sig[1]);
    log_raw(", loading ");
    log_dec(now_sig[2]);
    log_raw(", loaded ");
    log_dec(now_sig[3]);
    log_raw(")\n");
    g_car_wait_start = 0;
    return 1;
}

static unsigned long long car_wait_tick(void *self, void *a, void *b, void *c)
{
    g_car_cache_self = self;
    return g_car_pool_tick_orig(self, a, b, c);
}

static int patch_car_wait(void)
{
    static const unsigned char wait_head[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0xA8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x18, 0x48};
    static const unsigned char tick_head[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
    unsigned char *img = find_fh2_image();

    if (!img || !code_is("carwait", "the player car wait (0x7cbf10)", img, CAR_WAIT_RVA, wait_head, sizeof wait_head) ||
        !code_is("carwait", "the car cache's tick", img, CAR_POOL_TICK_RVA, tick_head, sizeof tick_head)) {
        return 0;
    }
    if (!g_car_pool_tick_orig &&
        !plat_hook("carwait: the car cache is found through its tick", img + CAR_POOL_TICK_RVA, (void *)car_wait_tick, (void **)&g_car_pool_tick_orig)) {
        return 0;
    }
    if (!plat_hook("carwait: the wait for every player's car gives up when no car can load", img + CAR_WAIT_RVA, (void *)car_wait, (void **)&g_car_wait_orig)) {
        return 0;
    }
    return 1;
}
