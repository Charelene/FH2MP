/* Opt-in lighting side channel. Native game packets are never extended/rewritten.
 * Native addresses/layout evidence: docs/lighting-sync.md (Anthem 1.0.0.64).
 * Car memory is touched only inside native callbacks, never by the UDP worker.
 */
#include "lighting_wire.h"
typedef struct { unsigned word, owner; } light_car_key;
typedef struct {
    void *car, *animation;
    unsigned id, owner;
    ULONGLONG seen;
    unsigned doors_applied;     /* the open parts this car was last shown with (lighting v2) */
} light_remote_car;
static SRWLOCK g_light_lock = SRWLOCK_INIT;
static unsigned char *g_light_image;
static struct {
    unsigned char address[28], epoch[16], records[LIGHT_MAX*6], car, flags;
    void *local_hint;
    int address_len;
    unsigned count, record_size, doors;     /* record_size 4 (v1 reply) or 6 (v2); doors: the local car's open parts */
    ULONGLONG sample_at, prediction_at, response_at, generation;
} g_light;
static light_remote_car g_light_cars[32];
static unsigned g_light_next;
static volatile LONG g_light_enabled;
static void *(*g_light_send_original)(void *, void *, const light_car_key *, const void *, unsigned);
static unsigned char (*g_light_receive_original)(void *, const void *, const void *, unsigned);
static void (*g_light_publish_original)(void *, float);
static void (*g_light_animation_original)(void *);
static void (*g_door_event_original)(void *, const char *);

/* ---- open parts (lighting v2): doors, hood, trunk, roof, storage, active spoiler ----
 * A car's parts move by named events of its animation graph ("hood:open", "doorLF:close", "roof:toggle"), sent with game+0xF5D300(car, name);
 * FH2 ANNA and the game itself use it [C: decompile; FH2 ANNA 1.1.0 src/car.cpp]. The game keeps no "open" state, so the shim counts the events
 * the local car gets (its own hook on 0xF5D300) and sends the result in its lighting polls; a remote car whose owner's parts differ from what it
 * was last shown with gets the same events (in the remote prediction receiver, on the game thread, outside the car's own update). The graph is
 * car+0x4D40 -> +0x1C8, its events an MSVC unordered_map<std::string> at +0x268 (list head at +0x268, size +0x270), looked up with
 * game+0x16FD4D0(map, &node, &key); game+0x16FBE90(graph, 0) puts it at full detail first so a far part still moves. The pop-up headlights stay
 * with the headlights: the first event a graph gets drops them (ANNA's note), so they are not sent. */
static const char *const g_door_parts[LIGHT_DOOR_BITS] = {"roof", "hood", "trunk", "doorLF", "doorRF", "doorLR", "doorRR", "storage", "storageM",
                                                          "active_aero"};
typedef struct { union { char text[16]; const char *pointer; } u; unsigned long long size, capacity; } door_key;

/* the part bit of an event "part:action" and what it does: 1 open, 0 close, 2 toggle; -1 when it is not a synced part */
static int door_parse(const char *name, int *action)
{
    char buf[40]; unsigned n = 0, k, len;
    while (n < sizeof buf - 1 && read_game_memory(name + n, buf + n, 1) && buf[n]) n++;
    buf[n] = 0;
    for (k = 0; k < LIGHT_DOOR_BITS; k++) {
        len = (unsigned)lstrlenA(g_door_parts[k]);
        if (n > len && !memcmp(buf, g_door_parts[k], len) && buf[len] == ':') {
            const char *a = buf + len + 1;
            *action = !lstrcmpA(a, "open") ? 1 : !lstrcmpA(a, "close") ? 0 : !lstrcmpA(a, "toggle") ? 2 : -1;
            return *action < 0 ? -1 : (int)k;
        }
    }
    return -1;
}

static void lighting_door_event(void *car, const char *name)
{
    if (g_light_enabled && car && name) {
        int action, k;
        void *local;
        AcquireSRWLockShared(&g_light_lock);
        local = g_light.local_hint;
        ReleaseSRWLockShared(&g_light_lock);
        if (car == local && (k = door_parse(name, &action)) >= 0) {
            AcquireSRWLockExclusive(&g_light_lock);
            if (g_light.local_hint == car) {
                if (action == 1) g_light.doors |= 1u << k;
                else if (action == 0) g_light.doors &= ~(1u << k);
                else g_light.doors ^= 1u << k;
            }
            ReleaseSRWLockExclusive(&g_light_lock);
        }
    }
    g_door_event_original(car, name);
}

static int door_has_event(unsigned char *graph, const char *name)
{
    door_key key;
    unsigned long long head = 0, size = 0;
    void *node = NULL;
    if (!read_game_memory(graph + 0x268, &head, 8) || !read_game_memory(graph + 0x270, &size, 8) || !head || size > 65536) return 0;
    memset(&key, 0, sizeof key);
    key.size = (unsigned long long)lstrlenA(name);
    if (key.size < 16) { memcpy(key.u.text, name, (size_t)key.size + 1); key.capacity = 15; }
    else { key.u.pointer = name; key.capacity = key.size; }
    ((void *(*)(void *, void **, const door_key *))(g_light_image + 0x16fd4d0))(graph + 0x268, &node, &key);
    return node && (unsigned long long)node != head;
}

/* Sends what moves the remote car's parts from `from` to `to`. -> the parts it now shows (a part the car cannot move counts as moved). */
static unsigned door_apply(void *car, unsigned from, unsigned to)
{
    unsigned char *animator = NULL, *graph = NULL;
    unsigned k;
    if (!read_game_memory((unsigned char *)car + 0x4d40, &animator, 8) || !animator || !read_game_memory(animator + 0x1c8, &graph, 8) || !graph) {
        return from;
    }
    for (k = 0; k < LIGHT_DOOR_BITS; k++) {
        char name[32];
        int open = (to >> k) & 1;
        if (!(((from ^ to) >> k) & 1)) continue;
        lstrcpyA(name, g_door_parts[k]);
        lstrcatA(name, open ? ":open" : ":close");
        if (!door_has_event(graph, name)) {
            lstrcpyA(name, g_door_parts[k]);
            lstrcatA(name, ":toggle");
            if (!door_has_event(graph, name)) continue;
        }
        ((void (*)(void *, int))(g_light_image + 0x16fbe90))(graph, 0);
        g_door_event_original(car, name);
    }
    return to;
}

/* Volatile byte copy keeps the no-CRT shim free of compiler-generated memcpy imports. */
static void light_copy(volatile unsigned char *to, const volatile unsigned char *from, unsigned n)
{
    unsigned i; for(i=0;i<n;i++) to[i]=from[i];
}
static void lighting_note_ticket(const void *to, int len, const unsigned char *nonce)
{
    unsigned i;
    if (!g_light_enabled || !to || len<16 || len>28) return;
    if (*(const unsigned short *)to!=AF_INET && *(const unsigned short *)to!=AF_INET6) return;
    AcquireSRWLockExclusive(&g_light_lock);
    light_copy(g_light.address,to,(unsigned)len);
    for(i=0;i<16;i++) g_light.epoch[i]=nonce[i];
    g_light.address_len=len;
    g_light.response_at=0; g_light.count=0; g_light.generation++;
    ReleaseSRWLockExclusive(&g_light_lock);
}

/* Strong native reference returned by LocalPlayer::GetCar; release via virtual
 * slot18 exactly as its native callers do. Do not retain it beyond the callback.
 */
static void lighting_sample(void *published_car)
{
    void *manager=NULL, *player=NULL, *car=NULL, *vtable=NULL;
    unsigned char manual, forced, automatic, signals[3];
    ULONGLONG now=GetTickCount64(), sampled; void *hint;
    AcquireSRWLockShared(&g_light_lock);
    hint=g_light.local_hint; sampled=g_light.sample_at;
    ReleaseSRWLockShared(&g_light_lock);
    if(hint && hint!=published_car && now-sampled<250) return;
    if(!read_game_memory(g_light_image+0x34aba60,&manager,8) || !manager) return;
    player=((void *(*)(void *, int))(g_light_image+0x21e0c0))(manager,0);
    if(!player) return;
    ((void *(*)(void *,void **))(g_light_image+0x2da060))(player,&car);
    if(!car) return;
    if(car==published_car && read_game_memory(car,&vtable,8) && vtable==g_light_image+0x2c5cb80 &&
       read_game_memory((unsigned char *)car+0x51a1,&manual,1) &&
       read_game_memory((unsigned char *)car+0x51a2,&forced,1) &&
       read_game_memory((unsigned char *)car+0x51b4,&automatic,1) &&
       read_game_memory((unsigned char *)car+0x51bb,signals,3)) {
        AcquireSRWLockExclusive(&g_light_lock);
        if(g_light.local_hint!=car) g_light.doors=0;
        g_light.local_hint=car;
        g_light.flags=light_flags(manual,forced,automatic,signals[0],signals[1],signals[2]);
        g_light.sample_at=now;
        ReleaseSRWLockExclusive(&g_light_lock);
    }
    /* GetCar returns a native intrusive reference, even when the vtable check
     * fails. Its release slot is validated as game code before calling it. */
    {
        void *release=NULL;
        if(read_game_memory(car,&vtable,8) && read_game_memory((unsigned char *)vtable+0x18,&release,8) &&
           (unsigned char *)release>=g_light_image+0x1000 && (unsigned char *)release<g_light_image+0x1dcc000)
            ((void (*)(void *))release)(car);
    }
}
static void *lighting_send(void *out, void *stream, const light_car_key *key, const void *state, unsigned stamp)
{
#ifdef FH2MP_PERF
    LONG64 perf_a=perf_now();
#endif
    if(g_light_enabled && key && !(key->word & 0xe0000000u) && (key->word & 0x1fffffffu)<=127) {
        AcquireSRWLockExclusive(&g_light_lock);
        g_light.car=(unsigned char)(key->word & 0x1fffffffu);
        g_light.prediction_at=GetTickCount64();
        ReleaseSRWLockExclusive(&g_light_lock);
    }
#ifdef FH2MP_PERF
    perf_add(PERF_SEND,perf_now()-perf_a);
#endif
    return g_light_send_original(out,stream,key,state,stamp);
}
static unsigned char lighting_receive(void *self, const void *context, const void *state, unsigned stamp)
{
    /* ICarNetworkedEvents is the +88 subobject. The callback's second argument
     * is message context, not the target car key. NetworkedObject stores its
     * own key at +20/+24, i.e. self-68. Constructors1789ba0/17dbdd0/9afb10 and
     * getter17a5dc0 confirm this layout. */
    static volatile LONG observations;
#ifdef FH2MP_PERF
    LONG64 perf_a=perf_now(),perf_b,perf_c;
#endif
    light_car_key native_key={0}; const light_car_key *key=&native_key;
    int have_key=read_game_memory((unsigned char *)self-0x68,&native_key,sizeof native_key);
    void *holder=NULL,*prediction=NULL,*car=NULL,*animation=NULL;
    unsigned previous=0; unsigned char ready=0; int accepted=0;
    if(g_light_enabled && have_key && (key->word & 0xe0000000u)==0x40000000u &&
       (key->word & 0x1fffffffu)<=127 && key->owner>0 && key->owner<255 &&
       read_game_memory((unsigned char *)self+0x2d8,&holder,8) && holder &&
       read_game_memory((unsigned char *)holder+0x28,&prediction,8) && prediction &&
       read_game_memory((unsigned char *)prediction+0x34,&previous,4) &&
       read_game_memory((unsigned char *)prediction+0x1b,&ready,1) && ready &&
       (LONG)(stamp-previous)>0 && read_game_memory((unsigned char *)prediction+8,&car,8) && car &&
       read_game_memory((unsigned char *)car+0x4d40,&animation,8) && animation) accepted=1;
    if(g_light_enabled && InterlockedIncrement(&observations)<=3) {
        log_begin();log_raw("lighting: receiver native key=");log_hex(key->word,8);
        log_raw(" owner=");log_dec(key->owner);log_raw(accepted?" accepted\n":" awaiting usable prediction\n");
    }
    {
#ifdef FH2MP_PERF
        perf_b=perf_now();
#endif
        unsigned char result=g_light_receive_original(self,context,state,stamp);
#ifdef FH2MP_PERF
        perf_c=perf_now();
#endif
        if(accepted) {
            unsigned i,slot=32; int first=0;
            AcquireSRWLockExclusive(&g_light_lock);
            for(i=0;i<32;i++) if(g_light_cars[i].car==car) {slot=i;break;}
            if(slot==32) {slot=g_light_next++%32;first=1;}
            {
                unsigned applied=first?0:g_light_cars[slot].doors_applied;
                g_light_cars[slot]=(light_remote_car){car,animation,key->word & 0x1fffffffu,key->owner,GetTickCount64(),applied};
            }
            ReleaseSRWLockExclusive(&g_light_lock);
            {
                /* the owner's open parts (v2 replies only); applied here, on the game thread, outside the car's update */
                unsigned want=0,applied=0; int have=0; ULONGLONG now=GetTickCount64();
                AcquireSRWLockShared(&g_light_lock);
                if(g_light.record_size==6 && g_light.response_at && now-g_light.response_at<=LIGHT_LEASE_MS) {
                    for(i=0;i<g_light.count;i++) {
                        unsigned char *r=g_light.records+6*i;
                        if(r[0]==(key->owner) && r[1]==(key->word & 0x1fffffffu)) {want=(unsigned)(r[3] | r[4]<<8);have=1;break;}
                    }
                }
                applied=g_light_cars[slot].doors_applied;
                ReleaseSRWLockShared(&g_light_lock);
                if(have && want!=applied && g_door_event_original) {
                    static volatile LONG door_lines;
                    unsigned shown=door_apply(car,applied,want);
                    AcquireSRWLockExclusive(&g_light_lock);
                    if(g_light_cars[slot].car==car) g_light_cars[slot].doors_applied=shown;
                    ReleaseSRWLockExclusive(&g_light_lock);
                    if(InterlockedIncrement(&door_lines)<=20) {
                        log_begin();log_raw("lighting: remote car owner=");log_dec(key->owner);log_raw(" open parts ");log_hex(applied,3);
                        log_raw(" -> ");log_hex(want,3);log_raw("\n");
                    }
                }
            }
            if(first) {
                log_begin();log_raw("lighting: remote car mapped owner=");log_dec(key->owner);
                log_raw(" id=");log_dec(key->word & 0x1fffffffu);log_raw("\n");
            }
        }
#ifdef FH2MP_PERF
        perf_add(PERF_RECEIVE,(perf_b-perf_a)+(perf_now()-perf_c));
#endif
        return result;
    }
}
/* Temporary visual override: restoring immediately makes timeout, despawn and
 * playing without an updated peer revert to the game's own state automatically.
 */
typedef struct { unsigned char manual, forced, signals[3]; int active; } light_scope;
static light_scope lighting_begin(void *car)
{
    light_scope scope={0}; void *animation=NULL; unsigned i,j; int flags=-1;
    ULONGLONG now=GetTickCount64();
    if(!g_light_enabled || !read_game_memory((unsigned char *)car+0x4d40,&animation,8)) return scope;
    AcquireSRWLockShared(&g_light_lock);
    if(g_light.response_at && now-g_light.response_at<=LIGHT_LEASE_MS) {
        for(i=0;i<32 && flags<0;i++) {
            light_remote_car *remote=g_light_cars+i;
            if(remote->car!=car || remote->animation!=animation || now-remote->seen>LIGHT_LEASE_MS) continue;
            for(j=0;j<g_light.count;j++) {
                unsigned char *r=g_light.records+g_light.record_size*j;
                if(r[0]==remote->owner && r[1]==remote->id) {flags=r[2];break;}
            }
        }
    }
    ReleaseSRWLockShared(&g_light_lock);
    if(flags<0) return scope;
    {
        unsigned char *c=car;
        scope.manual=c[0x51a1]; scope.forced=c[0x51a2];
        for(i=0;i<3;i++) scope.signals[i]=c[0x51bb+i];
        c[0x51a1]=1; c[0x51a2]=flags&1;
        c[0x51bb]=(flags>>1)==3; c[0x51bc]=(flags>>1)==1; c[0x51bd]=(flags>>1)==2;
        scope.active=1;
    }
    return scope;
}
static void lighting_end(void *car, light_scope scope)
{
    unsigned i; unsigned char *c=car;
    if(!scope.active) return;
    c[0x51a1]=scope.manual; c[0x51a2]=scope.forced;
    for(i=0;i<3;i++) c[0x51bb+i]=scope.signals[i];
}
static void lighting_publish(void *car, float dt)
{
#ifdef FH2MP_PERF
    LONG64 t0=perf_now(),t1,t2;
    perf_tick(car);
#endif
    if(g_light_enabled) lighting_sample(car);
    light_scope scope=lighting_begin(car);
#ifdef FH2MP_PERF
    t1=perf_now();
#endif
    g_light_publish_original(car,dt);
#ifdef FH2MP_PERF
    t2=perf_now();
#endif
    lighting_end(car,scope);
#ifdef FH2MP_PERF
    perf_add(PERF_PUBLISH,(t1-t0)+(perf_now()-t2));
#endif
}
static void lighting_animation(void *car)
{
#ifdef FH2MP_PERF
    LONG64 t0=perf_now(),t1,t2;
#endif
    light_scope scope=lighting_begin(car);
#ifdef FH2MP_PERF
    t1=perf_now();
#endif
    g_light_animation_original(car);
#ifdef FH2MP_PERF
    t2=perf_now();
#endif
    lighting_end(car,scope);
#ifdef FH2MP_PERF
    perf_add(PERF_ANIM,(t1-t0)+(perf_now()-t2));
#endif
}

static DWORD WINAPI lighting_worker(void *unused)
{
    HMODULE ws=LoadLibraryW(L"ws2_32.dll");
    SOCKET s=INVALID_SOCKET; ULONGLONG seq=0,received=0,generation=0;
    SOCKET (WSAAPI *make_socket)(int,int,int);
    int (WSAAPI *nonblocking)(SOCKET,long,u_long *);
    int (WSAAPI *receive)(SOCKET,char *,int,int,struct sockaddr *,int *);
    int (WSAAPI *option)(SOCKET,int,int,const char *,int);
    int (WSAAPI *close_socket)(SOCKET);
    u_long nb=1; int v6only=0;
    ULONGLONG polls=0; unsigned rejected=0; int silent_told=0, socket_told=0, v2=1, v1_told=0;
    (void)unused;
    if(!ws) {log_begin();log_raw("lighting: sync is off, ws2_32.dll could not be loaded (error ");log_dec(GetLastError());log_raw(")\n");return 0;}
    make_socket=(void *)GetProcAddress(ws,"socket"); nonblocking=(void *)GetProcAddress(ws,"ioctlsocket");
    receive=(void *)GetProcAddress(ws,"recvfrom"); option=(void *)GetProcAddress(ws,"setsockopt");
    close_socket=(void *)GetProcAddress(ws,"closesocket");
    if(!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin();log_raw("lighting: sync is off, ws2_32.dll lacks socket, ioctlsocket, recvfrom, setsockopt or closesocket\n");return 0;
    }
    while(g_light_enabled) {
        unsigned char address[28],epoch[16],packet[LIGHT2_LIMIT],car,flags; unsigned doors;
        int length, i, n, drained; ULONGLONG sampled,predicted,current_generation,now=GetTickCount64();
        AcquireSRWLockShared(&g_light_lock);
        length=g_light.address_len; sampled=g_light.sample_at; predicted=g_light.prediction_at; car=g_light.car; flags=g_light.flags;
        doors=g_light.doors;
        current_generation=g_light.generation;
        light_copy(address,g_light.address,(unsigned)length);
        for(i=0;i<16;i++) epoch[i]=g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if(generation!=current_generation) {
            if(s!=INVALID_SOCKET) close_socket(s);
            s=INVALID_SOCKET; received=0; generation=current_generation;
            polls=0; rejected=0; silent_told=0; v2=1;
        }
        if(!length || !sampled || !predicted || now-sampled>1000 || now-predicted>1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100);continue;}
        if(s==INVALID_SOCKET) {
            s=make_socket(*(const unsigned short *)address,SOCK_DGRAM,IPPROTO_UDP);
            if(s==INVALID_SOCKET) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("lighting: could not open the poll socket, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                Sleep(500);continue;
            }
            if(*(const unsigned short *)address==AF_INET6) option(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&v6only,sizeof v6only);
            if(nonblocking(s,FIONBIO,&nb)) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("lighting: could not make the poll socket non-blocking, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                close_socket(s);s=INVALID_SOCKET;Sleep(500);continue;
            }
            socket_told=0;
        }
        /* Drain replies from previous polls; accept delayed replies in sequence
         * order instead of requiring a round trip shorter than the poll period. */
        for(drained=0;drained<32 && (n=receive(s,(char *)packet,sizeof packet,0,NULL,NULL))>0;drained++) {
            if(v2 ? light_response2(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch)
                  : light_response(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch)) {
                unsigned count=packet[54], size=v2?6u:4u; int first=!received;
                AcquireSRWLockExclusive(&g_light_lock);
                if(g_light.generation==generation) {
                    light_copy(g_light.records,packet+(v2?LIGHT2_BASE:LIGHT_BASE),count*size);
                    g_light.count=count; g_light.record_size=size; g_light.response_at=GetTickCount64();
                    received=light_u64(packet+28);
                }
                ReleaseSRWLockExclusive(&g_light_lock);
                if(first && received) {log_begin();log_raw("lighting: authenticated world snapshot received after ");log_dec(polls);log_raw(" polls\n");}
            } else rejected++;
        }
        if(v2 && !received && polls>=30) {
            /* 3 s of v2 polls without one valid reply: a server without open parts (before lighting v2). v1 keeps the lights going. */
            v2=0;
            if(!v1_told) {v1_told=1;log_begin();log_raw("lighting: the server does not answer v2 polls (open parts), using v1: lights are shared, doors and hoods are not\n");}
        }
        if(v2) light_request2(packet,g_ticket_key,g_xuid,++seq,epoch,car,flags,doors);
        else light_request(packet,g_ticket_key,g_xuid,++seq,epoch,car,flags);
        {
            WSABUF b; DWORD sent=0; b.len=v2?LIGHT2_REQUEST:LIGHT_REQUEST; b.buf=(char *)packet;
            g_r_wsasendto(s,&b,1,&sent,0,address,length,NULL,NULL);
        }
        if(++polls%100==0) {
            /* every 10 s: said once when the server never answered, and when replies arrived but were refused (bad MAC, old sequence, bad layout) */
            if(!received && !silent_told) {
                silent_told=1;
                log_begin();log_raw("lighting: no snapshot reply to ");log_dec((unsigned long)polls);
                log_raw(" polls in 10 s, the server does not answer lighting polls (server without lighting, or UDP replies blocked); other players' lights stay as the game has them\n");
            }
            if(rejected) {
                log_begin();log_raw("lighting: ");log_dec(rejected);log_raw(" reply datagram(s) refused in the last 10 s (not a valid, authentic, in-sequence snapshot for this session)\n");
                rejected=0;
            }
        }
        Sleep(100);
    }
    if(s!=INVALID_SOCKET) close_socket(s);
    return 0;
}
static int patch_lighting(void)
{
    HANDLE thread; unsigned i;
    static const struct { unsigned rva; unsigned char bytes[8]; } checks[]={
        {0x1808170,{0x48,0x89,0x4c,0x24,0x08,0x41,0x56,0x48}},
        {0x9c7300,{0x48,0x8b,0x89,0xd8,0x02,0x00,0x00,0x49}},
        {0xf5c900,{0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57}},
        {0xf7bcc0,{0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54}},
    };
    g_light_image=find_fh2_image(); if(!g_light_image || !g_ticket_ready) return 0;
    for(i=0;i<sizeof checks/sizeof checks[0];i++) {
        static const char *const names[]={"the local prediction sender","the remote prediction receiver","the light material publisher","the headlight animator"};
        unsigned k;
        for(k=0;k<8 && g_light_image[checks[i].rva+k]==checks[i].bytes[k];k++) {}
        if(k<8 && g_light_image[checks[i].rva]==0xe9) {
            /* another mod's inline hook (a JMP over the first 5 bytes, e.g. FH2 ANNA on the publisher and the animator): the platform chains ours
             * behind it, so the rest of the bytes decide */
            for(k=5;k<8 && g_light_image[checks[i].rva+k]==checks[i].bytes[k];k++) {}
            if(k==8) {log_begin();log_raw("lighting: ");log_raw(names[i]);log_raw(" starts with another mod's hook; ours goes behind it through the platform\n");}
        }
        if(k<8) {
            log_begin();log_raw("lighting: unexpected bytes in ");log_raw(names[i]);log_raw(" at game+0x");log_hex(checks[i].rva+k,8);
            log_raw(" (first difference at +");log_dec(k);log_raw(" of 8): expected ");log_hex(checks[i].bytes[k],2);
            log_raw(", found ");log_hex(g_light_image[checks[i].rva+k],2);log_raw("; not enabled\n");
            return 0;
        }
    }
    if(!plat_hook("lighting: local prediction sample",g_light_image+0x1808170,(void *)lighting_send,(void **)&g_light_send_original) ||
       !plat_hook("lighting: remote prediction identity",g_light_image+0x9c7300,(void *)lighting_receive,(void **)&g_light_receive_original) ||
       !plat_hook("lighting: light material publication",g_light_image+0xf5c900,(void *)lighting_publish,(void **)&g_light_publish_original) ||
       !plat_hook("lighting: headlight animation parameter",g_light_image+0xf7bcc0,(void *)lighting_animation,(void **)&g_light_animation_original)) return 0;
    {
        static const unsigned char door_bytes[16]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x89,0x40,0x4d,0x00,0x00,0x48,0x8b,0xda};
        unsigned k;
        for(k=0;k<16 && g_light_image[0xf5d300+k]==door_bytes[k];k++) {}
        if(k<16 || !plat_hook("lighting: car part events (doors)",g_light_image+0xf5d300,(void *)lighting_door_event,(void **)&g_door_event_original)) {
            g_door_event_original=NULL;
            log_begin();log_raw("lighting: the car part event function (game+0xf5d300) is not as expected or could not be hooked: doors and hoods are not shared\n");
        }
    }
    InterlockedExchange(&g_light_enabled,1);
#ifdef FH2MP_PERF
    perf_start();
#endif
    thread=CreateThread(NULL,0,lighting_worker,NULL,0,NULL);
    if(!thread) {
        DWORD err=GetLastError();
        InterlockedExchange(&g_light_enabled,0);
        log_begin();log_raw("lighting: not enabled, the poll thread could not be created, error ");log_dec(err);log_raw(" (the four hooks stay in place but do nothing)\n");
        return 0;
    }
    CloseHandle(thread);
    return 1;
}
