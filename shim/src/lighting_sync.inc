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
} light_remote_car;
static SRWLOCK g_light_lock = SRWLOCK_INIT;
static unsigned char *g_light_image;
static struct {
    unsigned char address[28], epoch[16], records[LIGHT_MAX*4], car, flags;
    void *local_hint;
    int address_len;
    unsigned count;
    ULONGLONG sample_at, prediction_at, response_at, generation;
} g_light;
static light_remote_car g_light_cars[32];
static unsigned g_light_next;
static volatile LONG g_light_enabled;
static void *(*g_light_send_original)(void *, void *, const light_car_key *, const void *, unsigned);
static unsigned char (*g_light_receive_original)(void *, const void *, const void *, unsigned);
static void (*g_light_publish_original)(void *, float);
static void (*g_light_animation_original)(void *);

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
    if(g_light_enabled && key && !(key->word & 0xe0000000u) && (key->word & 0x1fffffffu)<=127) {
        AcquireSRWLockExclusive(&g_light_lock);
        g_light.car=(unsigned char)(key->word & 0x1fffffffu);
        g_light.prediction_at=GetTickCount64();
        ReleaseSRWLockExclusive(&g_light_lock);
    }
    return g_light_send_original(out,stream,key,state,stamp);
}
static unsigned char lighting_receive(void *self, const void *context, const void *state, unsigned stamp)
{
    /* ICarNetworkedEvents is the +88 subobject. The callback's second argument
     * is message context, not the target car key. NetworkedObject stores its
     * own key at +20/+24, i.e. self-68. Constructors1789ba0/17dbdd0/9afb10 and
     * getter17a5dc0 confirm this layout. */
    static volatile LONG observations;
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
        unsigned char result=g_light_receive_original(self,context,state,stamp);
        if(accepted) {
            unsigned i,slot=32; int first=0;
            AcquireSRWLockExclusive(&g_light_lock);
            for(i=0;i<32;i++) if(g_light_cars[i].car==car) {slot=i;break;}
            if(slot==32) {slot=g_light_next++%32;first=1;}
            g_light_cars[slot]=(light_remote_car){car,animation,key->word & 0x1fffffffu,key->owner,GetTickCount64()};
            ReleaseSRWLockExclusive(&g_light_lock);
            if(first) {
                log_begin();log_raw("lighting: remote car mapped owner=");log_dec(key->owner);
                log_raw(" id=");log_dec(key->word & 0x1fffffffu);log_raw("\n");
            }
        }
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
                unsigned char *r=g_light.records+4*j;
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
    if(g_light_enabled) lighting_sample(car);
    light_scope scope=lighting_begin(car);
    g_light_publish_original(car,dt);
    lighting_end(car,scope);
}
static void lighting_animation(void *car)
{
    light_scope scope=lighting_begin(car);
    g_light_animation_original(car);
    lighting_end(car,scope);
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
    (void)unused;
    if(!ws) return 0;
    make_socket=(void *)GetProcAddress(ws,"socket"); nonblocking=(void *)GetProcAddress(ws,"ioctlsocket");
    receive=(void *)GetProcAddress(ws,"recvfrom"); option=(void *)GetProcAddress(ws,"setsockopt");
    close_socket=(void *)GetProcAddress(ws,"closesocket");
    if(!make_socket || !nonblocking || !receive || !option || !close_socket) return 0;
    while(g_light_enabled) {
        unsigned char address[28],epoch[16],packet[LIGHT_LIMIT],car,flags;
        int length, i, n, drained; ULONGLONG sampled,predicted,current_generation,now=GetTickCount64();
        AcquireSRWLockShared(&g_light_lock);
        length=g_light.address_len; sampled=g_light.sample_at; predicted=g_light.prediction_at; car=g_light.car; flags=g_light.flags;
        current_generation=g_light.generation;
        light_copy(address,g_light.address,(unsigned)length);
        for(i=0;i<16;i++) epoch[i]=g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if(generation!=current_generation) {
            if(s!=INVALID_SOCKET) close_socket(s);
            s=INVALID_SOCKET; received=0; generation=current_generation;
        }
        if(!length || !sampled || !predicted || now-sampled>1000 || now-predicted>1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100);continue;}
        if(s==INVALID_SOCKET) {
            s=make_socket(*(const unsigned short *)address,SOCK_DGRAM,IPPROTO_UDP);
            if(s==INVALID_SOCKET) {Sleep(500);continue;}
            if(*(const unsigned short *)address==AF_INET6) option(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&v6only,sizeof v6only);
            if(nonblocking(s,FIONBIO,&nb)) {close_socket(s);s=INVALID_SOCKET;Sleep(500);continue;}
        }
        /* Drain replies from previous polls; accept delayed replies in sequence
         * order instead of requiring a round trip shorter than the poll period. */
        for(drained=0;drained<32 && (n=receive(s,(char *)packet,sizeof packet,0,NULL,NULL))>0;drained++) {
            if(light_response(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch)) {
                unsigned count=packet[54]; int first=!received;
                AcquireSRWLockExclusive(&g_light_lock);
                if(g_light.generation==generation) {
                    light_copy(g_light.records,packet+LIGHT_BASE,count*4);
                    g_light.count=count; g_light.response_at=GetTickCount64();
                    received=light_u64(packet+28);
                }
                ReleaseSRWLockExclusive(&g_light_lock);
                if(first && received) {log_begin();log_raw("lighting: authenticated world snapshot received\n");}
            }
        }
        light_request(packet,g_ticket_key,g_xuid,++seq,epoch,car,flags);
        {
            WSABUF b; DWORD sent=0; b.len=LIGHT_REQUEST; b.buf=(char *)packet;
            g_r_wsasendto(s,&b,1,&sent,0,address,length,NULL,NULL);
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
    for(i=0;i<sizeof checks/sizeof checks[0];i++)
        if(!mem_equal(g_light_image+checks[i].rva,checks[i].bytes,8)) {log_begin();log_raw("lighting: unexpected native bytes, disabled\n");return 0;}
    if(!plat_hook("lighting: local prediction sample",g_light_image+0x1808170,(void *)lighting_send,(void **)&g_light_send_original) ||
       !plat_hook("lighting: remote prediction identity",g_light_image+0x9c7300,(void *)lighting_receive,(void **)&g_light_receive_original) ||
       !plat_hook("lighting: light material publication",g_light_image+0xf5c900,(void *)lighting_publish,(void **)&g_light_publish_original) ||
       !plat_hook("lighting: headlight animation parameter",g_light_image+0xf7bcc0,(void *)lighting_animation,(void **)&g_light_animation_original)) return 0;
    InterlockedExchange(&g_light_enabled,1);
    thread=CreateThread(NULL,0,lighting_worker,NULL,0,NULL);
    if(!thread) {InterlockedExchange(&g_light_enabled,0);return 0;}
    CloseHandle(thread);
    log_begin();log_raw("lighting: authenticated visual sync enabled (native physics unchanged)\n");
    return 1;
}
