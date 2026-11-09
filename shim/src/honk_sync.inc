/* Horns of other players (issue 293). HonkHonk (suderek22, fh2-mods\HonkHonk.dll 0.4.0 pinned, 0.3.4 still known, a required add-on) plays the local
 * horn through the game's own FMOD; this file reads which horn the player is holding out of HonkHonk's memory, tells the server (a side channel like
 * lighting, native/internal/game/side.go, its own magic FH2MPHN1) and plays the horns the server lists at those cars with the same FMOD calls and the same bank.
 * HonkHonk itself is not changed on disk; the addresses in honk_builds are per known DLL build and are checked byte for byte, another build is left alone.
 *
 * Controls: HonkHonk honks on the left stick (L3, button bit 0x40, one `test byte [rsp+disp8], 0x40` in its input loop). Forza honks on the right
 * stick and Extra Options' headlights are moved to the left stick by the launcher, so the immediate is changed to 0x80 in memory. That is its own
 * step: it happens whether or not horns can be shared, and only when FH2_ExtraOptions.ini has the headlight/roof toggles off the right stick, so the
 * horn and a toggle never end up on one stick; data\bin\fh2mp-controls.txt tells the launcher where the horn is (launcher/fh2mp_launcher/shim.py).
 * A function start another mod hooked through the platform (Horizon Wave Radio hooks createSound) is called through that hook, as the game does.
 *
 * Remote voices run on the game's audio update (AudioEngineFmod::vfunc_5, 0x10140d0: Studio::System::update(*(this+0x7f0)), the call HonkHonk hooks
 * one level down), so every FMOD call is made on the thread HonkHonk's own calls use. Three limits keep a voice from sticking: the server's 0.5 s
 * lease (a horn nobody refreshes is gone from the list), 10 s per press (server and here), and a voice whose owner is missing from the list for 0.3 s
 * stops. The network thread never touches FMOD or a car. Needs the lighting hooks (the server address, ticket epoch and car id are theirs). */
#include "honk_wire.h"

#define HH_SYSTEM_RVA 0x10140d0u      /* AudioEngineFmod::vfunc_5 */
#define HH_STUDIO_OFFSET 0x7f0u
#define HH_VOICES 8
#define HH_SILENT_MS 300u
#define HH_SNAPSHOT_MS 600u

typedef struct {
    int (*get_low_level)(void *studio, void **core);
    int (*create_sound)(void *core, const void *data, unsigned mode, void *info, void **sound);
    int (*play_sound)(void *core, void *sound, void *group, int paused, void **channel);
    int (*get_listener)(void *core, int index, float *pos, float *vel, float *fwd, float *up);
    int (*set_3d)(void *channel, const float *pos, const float *vel, const float *alt);
    int (*set_min_max)(void *channel, float min, float max);
    int (*set_volume)(void *channel, float volume);
    int (*set_paused)(void *channel, int paused);
    int (*stop)(void *channel);
    int (*set_loop_points)(void *sound, unsigned start, unsigned start_type, unsigned end, unsigned end_type);
} honk_fmod;
typedef struct { unsigned off, bytes, loop_start, loop_end; } honk_sample;
typedef struct { void *channel; ULONGLONG started, seen; unsigned short press; unsigned char owner, horn; } honk_voice;

static SRWLOCK g_honk_lock = SRWLOCK_INIT;
static volatile LONG g_honk_enabled;
static struct { honk_record records[HONK_MAX]; unsigned count; ULONGLONG response_at; } g_honk;
static honk_fmod g_honk_fmod;
static void *g_honk_core;
static void *g_honk_sounds[HONK_HORNS];
static unsigned g_honk_sound_failed;                 /* bit per horn whose createSound failed (said once) */
static honk_voice g_honk_voices[HH_VOICES];
static unsigned short g_honk_capped[256];            /* per owner: the press that hit the cap */
static unsigned char g_honk_capped_set[256];
static unsigned char *g_honk_bank;
static DWORD g_honk_bank_size;
static honk_sample g_honk_samples[HONK_HORNS];
static volatile LONG g_honk_bank_ok;
static float g_honk_volume = 0.7f;
static volatile LONG g_honk_started, g_honk_positions_car, g_honk_positions_server;
static void (*g_honk_update_original)(void *);

static void honk_zero(void *p, unsigned n) { volatile unsigned char *q=p; unsigned i; for(i=0;i<n;i++) q[i]=0; }
static void honk_copy(void *to, const void *from, SIZE_T n) { volatile unsigned char *t=to; const volatile unsigned char *f=from; SIZE_T i; for(i=0;i<n;i++) t[i]=f[i]; }
static float honk_parse_float(const wchar_t *s)
{
    float v=0, scale=1; int dot=0;
    for(;*s;s++) {
        if(*s>=L'0' && *s<=L'9') {v=v*10+(*s-L'0'); if(dot) scale*=10;}
        else if(*s==L'.' && !dot) dot=1;
        else break;
    }
    return v/scale;
}

/* ---- the bank: HonkHonk's own FSB5 (mono PCM16, 31 samples), read exactly the way HonkHonk reads it ---- */
static int honk_parse_bank(const unsigned char *f, DWORD size, honk_sample out[HONK_HORNS])
{
    unsigned h[6], i; unsigned long long p=60, limit;
    if(size<60 || f[0]!='F' || f[1]!='S' || f[2]!='B' || f[3]!='5') return 0;
    honk_copy(h,f+4,sizeof h);
    /* h: version, samples, sample header bytes, name table bytes, data bytes, mode (2 = PCM16) */
    limit=60ull+h[2];
    if(h[5]!=2 || h[1]!=HONK_HORNS || limit+h[3]+h[4]>size) return 0;
    for(i=0;i<HONK_HORNS;i++) {
        unsigned long long v, off, frames; unsigned more;
        if(p+8>limit) return 0;
        honk_copy(&v,f+p,8); p+=8;
        out[i].loop_start=out[i].loop_end=0;
        for(more=(unsigned)(v&1);more;) {
            unsigned c, chunk, type;
            if(p+4>limit) return 0;
            honk_copy(&c,f+p,4); p+=4;
            more=c&1; chunk=(c>>1)&0xFFFFFF; type=c>>25;
            if(p+chunk>limit) return 0;
            if(type==3 && chunk>=8) {honk_copy(&out[i].loop_start,f+p,4); honk_copy(&out[i].loop_end,f+p+4,4);}
            p+=chunk;
        }
        off=((v>>7)&0x7FFFFFF)*32; frames=v>>34;
        if(off+frames*2>h[4]) return 0;
        if(out[i].loop_end>frames) out[i].loop_end=0;
        out[i].off=(unsigned)(60u+h[2]+h[3]+off); out[i].bytes=(unsigned)(frames*2);
    }
    return 1;
}
/* The sample as a WAV in memory (what FMOD's createSound is given): 44 byte header + PCM16 mono 44100 Hz, like HonkHonk. */
static unsigned char *honk_wav(unsigned index, DWORD *length)
{
    const honk_sample *s=g_honk_samples+index; unsigned char *w; unsigned riff, rate=44100, byte_rate=88200, i;
    static const unsigned char tail[]={'d','a','t','a'};
    if(!g_honk_bank_ok || index>=HONK_HORNS || !s->bytes) return NULL;
    w=HeapAlloc(GetProcessHeap(),0,44u+s->bytes);
    if(!w) return NULL;
    riff=36u+s->bytes;
    honk_copy(w,"RIFF",4); honk_copy(w+4,&riff,4); honk_copy(w+8,"WAVEfmt ",8);
    i=16; honk_copy(w+16,&i,4);
    w[20]=1; w[21]=0; w[22]=1; w[23]=0; honk_copy(w+24,&rate,4); honk_copy(w+28,&byte_rate,4);
    w[32]=2; w[33]=0; w[34]=16; w[35]=0; honk_copy(w+36,tail,4); honk_copy(w+40,&s->bytes,4);
    honk_copy(w+44,g_honk_bank+s->off,s->bytes);
    *length=44u+s->bytes;
    return w;
}
static void *honk_sound(unsigned index)
{
    unsigned char info[0xd8]; unsigned char *wav; DWORD length=0; void *sound=NULL; int r;
    if(index>=HONK_HORNS) return NULL;
    if(g_honk_sounds[index]) return g_honk_sounds[index];
    if((g_honk_sound_failed>>index) & 1) return NULL;
    wav=honk_wav(index,&length);
    if(wav) {
        honk_zero(info,sizeof info);
        *(unsigned *)info=0xd8; *(unsigned *)(info+4)=length;
        /* OPENMEMORY | CREATESAMPLE | 3D | LOOP_NORMAL | 3D_WORLDRELATIVE | 3D_INVERSEROLLOFF, as HonkHonk */
        r=g_honk_fmod.create_sound(g_honk_core,wav,0x800|0x100|0x10|0x2|0x80000|0x100000,info,&sound);
        if(r==0 && sound) {
            const honk_sample *s=g_honk_samples+index;
            if(s->loop_end>s->loop_start) g_honk_fmod.set_loop_points(sound,s->loop_start,2,s->loop_end-1,2);
            g_honk_sounds[index]=sound;
            return sound;
        }
        log_begin();log_raw("honk: createSound failed for horn ");log_dec(index);log_raw(" (FMOD error ");log_dec((unsigned long)r);log_raw("), that horn is not played\n");
    } else {
        log_begin();log_raw("honk: could not build horn ");log_dec(index);log_raw(" from the bank\n");
    }
    g_honk_sound_failed|=1u<<index;
    return NULL;
}

/* ---- remote voices (game audio thread) ---- */
static int honk_voice_position(const honk_record *r, float out[3])
{
    void *car=NULL; unsigned i; ULONGLONG now=GetTickCount64();
    AcquireSRWLockShared(&g_light_lock);
    for(i=0;i<32;i++)
        if(g_light_cars[i].car && g_light_cars[i].owner==r->owner && g_light_cars[i].id==r->car && now-g_light_cars[i].seen<=LIGHT_LEASE_MS) {car=g_light_cars[i].car;break;}
    ReleaseSRWLockShared(&g_light_lock);
    if(car) {
        /* the position HonkHonk reads for the local car (car+0x4c08 -> +0xef0), accepted when it agrees with the server's within 100 m */
        void *state=NULL; float p[3];
        if(read_game_memory((unsigned char *)car+0x4c08,&state,8) && state && read_game_memory((unsigned char *)state+0xef0,p,12) &&
           honk_finite(p[0]) && honk_finite(p[1]) && honk_finite(p[2])) {
            float dx=p[0]-r->pos[0], dy=p[1]-r->pos[1], dz=p[2]-r->pos[2];
            if(!(r->flags&1) || dx*dx+dy*dy+dz*dz<=10000.0f) {
                out[0]=p[0]; out[1]=p[1]; out[2]=p[2];
                InterlockedIncrement(&g_honk_positions_car);
                return 1;
            }
        }
    }
    if(r->flags&1) {
        out[0]=r->pos[0]; out[1]=r->pos[1]; out[2]=r->pos[2];
        InterlockedIncrement(&g_honk_positions_server);
        return 1;
    }
    return 0;
}
static void honk_voice_stop(honk_voice *v)
{
    if(v->channel) g_honk_fmod.stop(v->channel);
    v->channel=NULL;
}
static void honk_voices_step(ULONGLONG now)
{
    honk_record records[HONK_MAX]; unsigned count, i, j; float lp[3], lv[3], lf[3], lu[3]; ULONGLONG listed;
    AcquireSRWLockShared(&g_honk_lock);
    listed=g_honk.response_at;
    count=(listed && now-listed<=HH_SILENT_MS)?g_honk.count:0;      /* a list older than this says nothing: its voices run out */
    for(i=0;i<count;i++) records[i]=g_honk.records[i];
    ReleaseSRWLockShared(&g_honk_lock);
    if(!count) {
        for(j=0;j<HH_VOICES;j++) if(g_honk_voices[j].channel && now-g_honk_voices[j].seen>HH_SILENT_MS) honk_voice_stop(g_honk_voices+j);
        return;
    }
    if(g_honk_fmod.get_listener(g_honk_core,0,lp,lv,lf,lu)!=0) {lv[0]=lv[1]=lv[2]=0;}
    for(i=0;i<count;i++) {
        const honk_record *r=records+i; honk_voice *v=NULL, *free_slot=NULL; float pos[3];
        for(j=0;j<HH_VOICES;j++) {
            if(g_honk_voices[j].channel && g_honk_voices[j].owner==r->owner) v=g_honk_voices+j;
            else if(!g_honk_voices[j].channel && !free_slot) free_slot=g_honk_voices+j;
        }
        if(v && (v->press!=r->press || v->horn!=r->horn)) {honk_voice_stop(v); free_slot=v; v=NULL;}
        if(v) {
            if(now-v->started>HONK_CAP_MS) {
                g_honk_capped[r->owner]=v->press; g_honk_capped_set[r->owner]=1;
                honk_voice_stop(v);
                continue;
            }
            v->seen=listed;
            if(honk_voice_position(r,pos) && g_honk_fmod.set_3d(v->channel,pos,lv,NULL)!=0) honk_voice_stop(v);
            continue;
        }
        if(!free_slot || r->age>=HONK_CAP_MS || (g_honk_capped_set[r->owner] && g_honk_capped[r->owner]==r->press)) continue;
        if(!honk_voice_position(r,pos)) continue;
        {
            void *sound=honk_sound(r->horn), *channel=NULL;
            if(!sound) continue;
            if(g_honk_fmod.play_sound(g_honk_core,sound,NULL,1,&channel)!=0 || !channel) continue;
            g_honk_fmod.set_3d(channel,pos,lv,NULL);
            g_honk_fmod.set_volume(channel,g_honk_volume);
            g_honk_fmod.set_min_max(channel,5.0f,150.0f);
            g_honk_fmod.set_paused(channel,0);
            free_slot->channel=channel; free_slot->started=now-r->age; free_slot->seen=listed;
            free_slot->press=r->press; free_slot->owner=r->owner; free_slot->horn=r->horn;
            g_honk_capped_set[r->owner]=0;
            if(InterlockedIncrement(&g_honk_started)<=3) {
                log_begin();log_raw("honk: playing owner ");log_dec(r->owner);log_raw("'s horn ");log_dec(r->horn);
                log_raw(" at their car (");log_raw(g_honk_positions_car?"position read from the car":"position from the server");log_raw(")\n");
            }
        }
    }
    for(j=0;j<HH_VOICES;j++) if(g_honk_voices[j].channel && now-g_honk_voices[j].seen>HH_SILENT_MS) honk_voice_stop(g_honk_voices+j);
}
static void honk_audio_tick(void *engine)
{
    ULONGLONG now=GetTickCount64(); unsigned j; int active=0;
    if(!g_honk_bank_ok) return;
    for(j=0;j<HH_VOICES;j++) if(g_honk_voices[j].channel) active=1;
    if(!active) {
        AcquireSRWLockShared(&g_honk_lock);
        active=g_honk.count && g_honk.response_at && now-g_honk.response_at<=HH_SILENT_MS;
        ReleaseSRWLockShared(&g_honk_lock);
        if(!active) return;
    }
    if(!g_honk_core) {
        void *studio=NULL, *core=NULL;
        if(!read_game_memory((unsigned char *)engine+HH_STUDIO_OFFSET,&studio,8) || !studio || g_honk_fmod.get_low_level(studio,&core)!=0 || !core) return;
        g_honk_core=core;
        log_begin();log_raw("honk: the game's FMOD core captured for other players' horns\n");
    }
    honk_voices_step(now);
}
static void honk_audio_update(void *engine)
{
#ifdef FH2MP_PERF
    LONG64 perf_a;
#endif
    g_honk_update_original(engine);
#ifdef FH2MP_PERF
    perf_a=perf_now();
#endif
    if(g_honk_enabled) honk_audio_tick(engine);
#ifdef FH2MP_PERF
    perf_add(PERF_HORN,perf_now()-perf_a);
#endif
}

/* ---- HonkHonk.dll: the horn it holds, its button, its bank ----
 * One entry per known build, recognised by its request setter. request: fmodhorn::request(int), `mov [rip+disp32], ecx ; ret` (the horn index or -1;
 * both builds store to RVA 0xe044). button: the left stick test of the input loop, its immediate (+4) is 0x40 and becomes 0x80 for the right stick.
 * xinput_redirect: the build polls XInputGetState itself (0.3.4 only; 0.4.0 reads the game's active controller, its PR #1). */
typedef struct {
    const char *version;
    unsigned request_rva; unsigned char request_code[7];
    unsigned button_rva; unsigned char button_code[7];
    int xinput_redirect;
} honk_build;
static const honk_build honk_builds[]={
    {"0.4.0",0x68c0u,{0x89,0x0d,0x7e,0x77,0x00,0x00,0xc3},0x2267u,{0xf6,0x44,0x24,0x60,0x40,0x74,0x5e},0},   /* sha256 b42484b7..., test [rsp+0x60] ; je */
    {"0.3.4",0x6840u,{0x89,0x0d,0xfe,0x77,0x00,0x00,0xc3},0x222du,{0xf6,0x44,0x24,0x2c,0x40,0x75,0x05},1},   /* sha256 4ed6ba03..., test [rsp+0x2c] ; jne */
};
#define HH_BUILDS (sizeof honk_builds/sizeof honk_builds[0])
#define HH_BUTTON_IMM 4u
static unsigned char *g_honk_request;                /* HonkHonk's request variable, NULL until a known build is recognised */

static int honk_code_is(unsigned char *at, const unsigned char *code, unsigned n)
{
    unsigned char now[8]; unsigned i;
    if(!read_game_memory(at,now,n)) return 0;
    for(i=0;i<n;i++) if(now[i]!=code[i]) return 0;
    return 1;
}
/* The known build at HonkHonk's image `hh`, NULL for another build. */
static const honk_build *honk_build_at(unsigned char *hh)
{
    unsigned i;
    for(i=0;i<HH_BUILDS;i++) if(honk_code_is(hh+honk_builds[i].request_rva,honk_builds[i].request_code,7)) return &honk_builds[i];
    return NULL;
}
/* The build's button test with the immediate `imm` (0x40 as built, 0x80 once moved). */
static int honk_button_is(unsigned char *hh, const honk_build *b, unsigned char imm)
{
    unsigned char code[7]; unsigned i;
    for(i=0;i<7;i++) code[i]=b->button_code[i];
    code[HH_BUTTON_IMM]=imm;
    return honk_code_is(hh+b->button_rva,code,7);
}
/* Left stick -> right stick in HonkHonk's input loop (the immediate of the one `test byte [rsp+disp8], imm8`). */
static int honk_move_button(unsigned char *hh, const honk_build *b)
{
    unsigned char *imm=hh+b->button_rva+HH_BUTTON_IMM; DWORD old;
    if(!VirtualProtect(imm,1,PAGE_EXECUTE_READWRITE,&old)) return 0;
    *imm=0x80;
    FlushInstructionCache(GetCurrentProcess(),imm,1);
    VirtualProtect(imm,1,old,&old);
    return 1;
}

/* HonkHonk 0.3.4's worker calls XInputGetState on all four pad slots every 16 ms (0.4.0 reads the game's controller itself, this is not needed there). Under Wine one call costs 0.5-0.8 ms even for an empty slot (measured under
 * Proton 11 in a scratch prefix: avg 530-825 us, max 3-5 ms): ~240 calls/s, about a sixth of a core and constant wineserver traffic. With HonkHonk off the
 * lag in a full lobby was gone (issue #354, hand test), and with the pad read from the game it stayed gone. So on Wine its import slot for XInputGetState
 * (ordinal 2 of XINPUT1_4.dll, IAT slot HonkHonk.dll+0x92b0, the target of the thunk at +0x7678) points at honk_xinput_state: the button mask comes from the
 * game's own active controller, the way Extra Options reads it (input manager game+0x365d7c8 with vtable game+0x2e7a2e8, active controller at +0xab28 inside
 * 8 controllers of 0x1300 bytes from +0x10, connected byte +4, buttons u16 +0xc0, triggers +0x70), and the other slots answer "not connected". */
#define HH_XINPUT_IAT_RVA 0x92b0u
static const unsigned char honk_xinput_thunk[6]={0xff,0x25,0x32,0x1c,0x00,0x00};      /* +0x7678: jmp [rip+0x1c32] -> +0x92b0 */
typedef struct { DWORD packet; WORD buttons; BYTE left_trigger, right_trigger; SHORT lx, ly, rx, ry; } honk_xinput_state_t;
static unsigned char *g_honk_pad_image;
static DWORD WINAPI honk_xinput_state(DWORD user, honk_xinput_state_t *state)
{
    unsigned char *img=g_honk_pad_image, *manager=NULL, *controller=NULL, connected=0, triggers[2]={0,0}; WORD buttons=0; static DWORD packet;
    if(!state) return ERROR_BAD_ARGUMENTS;
    if(user!=0 || !img) return ERROR_DEVICE_NOT_CONNECTED;
    if(!read_game_memory(img+0x365d7c8,&manager,8) || !manager || !honk_code_is(manager,(const unsigned char *)&(void *const){img+0x2e7a2e8},8) ||
       !read_game_memory(manager+0xab28,&controller,8) || !controller ||
       (UINT_PTR)controller<(UINT_PTR)manager+0x10 || (UINT_PTR)controller>=(UINT_PTR)manager+0x10+8*0x1300 || ((UINT_PTR)controller-((UINT_PTR)manager+0x10))%0x1300 ||
       !read_game_memory(controller+4,&connected,1) || connected!=1 || !read_game_memory(controller+0xc0,&buttons,2)) return ERROR_DEVICE_NOT_CONNECTED;
    read_game_memory(controller+0x70,triggers,2);
    state->packet=++packet; state->buttons=buttons; state->left_trigger=triggers[0]; state->right_trigger=triggers[1];
    state->lx=state->ly=state->rx=state->ry=0;
    return ERROR_SUCCESS;
}
static void honk_redirect_pad(unsigned char *hh, const honk_build *b)
{
    HMODULE nt=GetModuleHandleW(L"ntdll.dll"); unsigned char *slot=hh+HH_XINPUT_IAT_RVA, *image; DWORD old; void *target=(void *)honk_xinput_state;
    if(!b || !b->xinput_redirect) return;
    image=find_fh2_image();
    if(!nt || !GetProcAddress(nt,"wine_get_version") || !image) return;
    if(!honk_code_is(hh+0x7678,honk_xinput_thunk,6)) {
        log_begin();log_raw("honk: pad redirect skipped, HonkHonk.dll+0x7678 is not the 0.3.4 XInputGetState thunk\n");
        return;
    }
    if(!VirtualProtect(slot,8,PAGE_READWRITE,&old)) {
        log_begin();log_raw("honk: pad redirect failed, VirtualProtect on HonkHonk.dll+0x92b0 error ");log_dec(GetLastError());log_raw("\n");
        return;
    }
    g_honk_pad_image=image;
    mem_copy(slot,&target,8);
    VirtualProtect(slot,8,old,&old);
    log_begin();log_raw("honk: HonkHonk.dll XInputGetState redirected to the game's active controller (Wine)\n");
}

/* ---- which stick the horn is on: never the one Extra Options' headlight or roof toggle uses ----
 * The launcher moves those toggles to the left stick (launcher/fh2mp_launcher/shim.py swap_stick_bindings); the horn only goes to the right stick
 * when FH2_ExtraOptions.ini really has them off it. The outcome goes to data\bin\fh2mp-controls.txt, from which the launcher puts the toggles back
 * on the right stick when the horn could not be moved. */
#define HH_STICK_LEFT 1u
#define HH_STICK_RIGHT 2u
static wchar_t g_honk_eo_ini[MAX_PATH+40];           /* <Azimuth>\FH2_ExtraOptions.ini, empty: not known */
static wchar_t g_honk_status[MAX_PATH+40];           /* <data\bin>\fh2mp-controls.txt, empty: not written */

/* The sticks one Extra Options binding uses ("LeftStick", "LB+RightStick", the aliases LS/RS, any case). */
static unsigned honk_binding_sticks(const wchar_t *v)
{
    unsigned used=0;
    while(*v) {
        wchar_t token[24]; unsigned n=0;
        while(*v==L' ' || *v==L'\t' || *v==L'+') v++;
        while(*v && *v!=L'+') {if(n<23) token[n++]=*v; v++;}
        while(n && (token[n-1]==L' ' || token[n-1]==L'\t' || token[n-1]==L'\r')) n--;
        token[n]=0;
        if(!lstrcmpiW(token,L"LeftStick") || !lstrcmpiW(token,L"LS")) used|=HH_STICK_LEFT;
        else if(!lstrcmpiW(token,L"RightStick") || !lstrcmpiW(token,L"RS")) used|=HH_STICK_RIGHT;
    }
    return used;
}
/* The sticks Extra Options' headlight and roof toggles use; a key that is not in the file is Extra Options' default, RightStick. */
static unsigned honk_eo_sticks(const wchar_t *ini)
{
    static const wchar_t *const keys[]={L"HeadlightsXbox",L"RoofXbox"};
    unsigned used=0, k;
    for(k=0;k<2;k++) {
        wchar_t v[96];
        GetPrivateProfileStringW(L"Bindings",keys[k],L"RightStick",v,96,ini);
        used|=honk_binding_sticks(v);
    }
    return used;
}
/* The right stick (Forza's own horn button) unless a toggle sits there and the left stick is free. */
static int honk_horn_goes_right(unsigned eo) {return !(eo&HH_STICK_RIGHT) || (eo&HH_STICK_LEFT);}
static void honk_write_status(int right, int movable, const char *why)
{
    HANDLE f; DWORD written; char text[256]; unsigned i=0, k; const char *parts[4];
    if(!g_honk_status[0]) return;
    parts[0]=right?"horn=RightStick\r\n":"horn=LeftStick\r\n"; parts[1]=movable?"movable=1\r\nwhy=":"movable=0\r\nwhy="; parts[2]=why; parts[3]="\r\n";
    for(k=0;k<4;k++) {const char *c=parts[k]; while(*c && i<sizeof text-1) text[i++]=*c++;}
    f=CreateFileW(g_honk_status,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) return;
    WriteFile(f,text,i,&written,NULL);
    CloseHandle(f);
}
/* Once per game: puts HonkHonk's horn button on the right stick when Extra Options' toggles are off it. -> 1 the horn is on the right stick. */
static int honk_place_button(unsigned char *hh, const honk_build *b)
{
    unsigned eo;
    if(b && honk_button_is(hh,b,0x80)) {
        honk_write_status(1,1,"already on the right stick");
        return 1;
    }
    if(!b || !honk_button_is(hh,b,0x40)) {
        unsigned char now[7];
        log_begin();
        if(!b) log_raw("honk: the horn button stays on the left stick, HonkHonk.dll is not a known build (0.4.0, 0.3.4)");
        else {
            log_raw("honk: the horn button stays on the left stick, HonkHonk.dll ");log_raw(b->version);log_raw(" has other bytes at +0x");log_hex(b->button_rva,4);
            log_raw(": ");
            if(read_game_memory(hh+b->button_rva,now,7)) log_hexbytes(now,7); else log_raw("(unreadable)");
        }
        log_raw("; the launcher puts Extra Options' headlights/roof back on the right stick\n");
        honk_write_status(0,0,"unknown HonkHonk build");
        return 0;
    }
    eo=g_honk_eo_ini[0]?honk_eo_sticks(g_honk_eo_ini):HH_STICK_RIGHT;
    if(!honk_horn_goes_right(eo)) {
        log_begin();log_raw("honk: the horn button stays on the left stick, Extra Options' headlights/roof are on the right stick (FH2_ExtraOptions.ini)\n");
        honk_write_status(0,1,"Extra Options uses the right stick");
        return 0;
    }
    if(!honk_move_button(hh,b)) {
        log_begin();log_raw("honk: could not move HonkHonk's horn button to the right stick (error ");log_dec(GetLastError());
        log_raw("), it stays on the left stick; the launcher puts Extra Options' headlights/roof back on the right stick\n");
        honk_write_status(0,0,"could not change HonkHonk's button");
        return 0;
    }
    log_begin();log_raw("honk: the horn button is the right stick");
    if(eo&HH_STICK_RIGHT) log_raw(" (Extra Options' headlights/roof use both sticks, so the right stick also works one of them)");
    log_raw("\n");
    honk_write_status(1,1,"moved");
    return 1;
}
/* -> 1 when HonkHonk.dll is loaded and its horn can be shared (sets g_honk_request, loads the bank), 0 not loaded yet, -1 sharing is off or another
 * build (said once). The horn button is placed on the first call that finds the DLL, whether or not horns are shared. */
static int honk_attach(void)
{
    static int told_other, attached, placed;
    HMODULE m=GetModuleHandleW(L"HonkHonk.dll"); unsigned char *hh=(unsigned char *)m; wchar_t path[MAX_PATH+40]; DWORD n, i, slash=0;
    const honk_build *b;
    if(attached) return 1;
    if(!hh) return 0;
    b=honk_build_at(hh);
    if(!placed) {placed=1; honk_place_button(hh,b); honk_redirect_pad(hh,b);}
    if(!g_honk_enabled) return -1;
    if(!b) {
        if(!told_other) {
            unsigned char now[7];
            told_other=1;
            log_begin();log_raw("honk: unexpected HonkHonk.dll (known builds: 0.4.0, 0.3.4), its horn is not shared; at the 0.4.0 request setter +0x");
            log_hex(honk_builds[0].request_rva,4);log_raw(" found ");
            if(read_game_memory(hh+honk_builds[0].request_rva,now,7)) log_hexbytes(now,7); else log_raw("(unreadable)");
            log_raw("\n");
        }
        return -1;
    }
    g_honk_request=hh+b->request_rva+6+*(const int *)(b->request_code+2);      /* mov [rip+disp32], ecx: next instruction + disp */
    attached=1;
    log_begin();log_raw("honk: HonkHonk.dll ");log_raw(b->version);log_raw(" found, its horn is read from its own state\n");
    n=GetModuleFileNameW(m,path,MAX_PATH);
    if(n && n<MAX_PATH) {
        wchar_t volume[16]; HANDLE f; wchar_t dir[MAX_PATH+40];
        for(i=0;i<n;i++) if(path[i]==L'\\') slash=i;
        for(i=0;i<=slash;i++) dir[i]=path[i];
        dir[slash+1]=0;
        lstrcpyW(path,dir); lstrcatW(path,L"HonkHonk\\HZ2_SFX_HonkHonk.bank");
        f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(f!=INVALID_HANDLE_VALUE) {
            DWORD size=GetFileSize(f,NULL), got=0; unsigned char *buf=NULL;
            if(size!=INVALID_FILE_SIZE && size>60 && size<=64u*1024*1024) buf=HeapAlloc(GetProcessHeap(),0,size);
            if(buf && ReadFile(f,buf,size,&got,NULL) && got==size && honk_parse_bank(buf,size,g_honk_samples)) {
                g_honk_bank=buf; g_honk_bank_size=size;
                InterlockedExchange(&g_honk_bank_ok,1);
                log_begin();log_raw("honk: the horn bank is loaded (");log_dec(HONK_HORNS);log_raw(" sounds)\n");
            } else {
                if(buf) HeapFree(GetProcessHeap(),0,buf);
                log_begin();log_raw("honk: could not read HonkHonk's horn bank (HZ2_SFX_HonkHonk.bank is not the 31 sound bank); other players' horns are not played\n");
            }
            CloseHandle(f);
        } else {
            log_begin();log_raw("honk: could not open HonkHonk\\HZ2_SFX_HonkHonk.bank, error ");log_dec(GetLastError());log_raw("; other players' horns are not played\n");
        }
        lstrcpyW(path,dir); lstrcatW(path,L"HonkHonk.ini");
        GetPrivateProfileStringW(L"HonkHonk",L"HornVolume",L"1.0",volume,16,path);
        g_honk_volume=honk_parse_float(volume);
        if(!(g_honk_volume>=0.0f && g_honk_volume<=4.0f)) g_honk_volume=1.0f;
        g_honk_volume*=0.7f;                                  /* a crowd of horns must not clip */
    }
    return 1;
}
static int honk_local_horn(void)
{
    int v=-1;
    if(!g_honk_request || !read_game_memory(g_honk_request,&v,4) || v<0 || (unsigned)v>=HONK_HORNS) return -1;
    return v;
}

static DWORD WINAPI honk_worker(void *unused)
{
    HMODULE ws=LoadLibraryW(L"ws2_32.dll");
    SOCKET s=INVALID_SOCKET; ULONGLONG seq=0,received=0,generation=0,last_send=0,polls=0;
    SOCKET (WSAAPI *make_socket)(int,int,int);
    int (WSAAPI *nonblocking)(SOCKET,long,u_long *);
    int (WSAAPI *receive)(SOCKET,char *,int,int,struct sockaddr *,int *);
    int (WSAAPI *option)(SOCKET,int,int,const char *,int);
    int (WSAAPI *close_socket)(SOCKET);
    u_long nb=1; int v6only=0, sent_horn=-2, previous=-1, socket_told=0, silent_told=0; unsigned short press=0, sent_press=0; unsigned rejected=0;
    (void)unused;
    if(!g_honk_enabled) {                            /* horns are not shared: only the button is placed */
        while(honk_attach()==0) Sleep(500);
        return 0;
    }
    if(ws) {
        make_socket=(void *)GetProcAddress(ws,"socket"); nonblocking=(void *)GetProcAddress(ws,"ioctlsocket");
        receive=(void *)GetProcAddress(ws,"recvfrom"); option=(void *)GetProcAddress(ws,"setsockopt");
        close_socket=(void *)GetProcAddress(ws,"closesocket");
    } else make_socket=NULL,nonblocking=NULL,receive=NULL,option=NULL,close_socket=NULL;
    if(!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin();log_raw("honk: sharing horns is off, ws2_32.dll could not be used (error ");log_dec(GetLastError());log_raw(")\n");
        ws=NULL;
    }
    while(g_honk_enabled) {
        unsigned char address[28],epoch[16],packet[HONK_LIMIT],car; honk_record records[HONK_MAX];
        int length, i, n, drained, horn; ULONGLONG predicted,current_generation,now=GetTickCount64(); unsigned count=0;
        if(honk_attach()<=0) {Sleep(500);continue;}
        if(!ws || !g_light_enabled) {Sleep(1000);continue;}
        horn=honk_local_horn();
        if(horn!=previous && horn>=0) press++;
        previous=horn;
        AcquireSRWLockShared(&g_light_lock);
        length=g_light.address_len; predicted=g_light.prediction_at; car=g_light.car; current_generation=g_light.generation;
        light_copy(address,g_light.address,(unsigned)length);
        for(i=0;i<16;i++) epoch[i]=g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if(generation!=current_generation) {
            if(s!=INVALID_SOCKET) close_socket(s);
            s=INVALID_SOCKET; received=0; generation=current_generation; polls=0; rejected=0; silent_told=0; sent_horn=-2;
            AcquireSRWLockExclusive(&g_honk_lock);
            g_honk.count=0; g_honk.response_at=0;
            ReleaseSRWLockExclusive(&g_honk_lock);
        }
        if(!length || !predicted || now-predicted>1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100);continue;}
        if(s==INVALID_SOCKET) {
            s=make_socket(*(const unsigned short *)address,SOCK_DGRAM,IPPROTO_UDP);
            if(s==INVALID_SOCKET) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("honk: could not open the poll socket, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                Sleep(500);continue;
            }
            if(*(const unsigned short *)address==AF_INET6) option(s,IPPROTO_IPV6,IPV6_V6ONLY,(const char *)&v6only,sizeof v6only);
            if(nonblocking(s,FIONBIO,&nb)) {
                if(!socket_told) {socket_told=1;log_begin();log_raw("honk: could not make the poll socket non-blocking, error ");log_dec(GetLastError());log_raw(", retrying every 0.5 s\n");}
                close_socket(s);s=INVALID_SOCKET;Sleep(500);continue;
            }
            socket_told=0;
        }
        for(drained=0;drained<32 && (n=receive(s,(char *)packet,sizeof packet,0,NULL,NULL))>0;drained++) {
            if(honk_response(packet,(unsigned)n,g_ticket_key,g_xuid,seq,received,epoch,records,&count)) {
                unsigned k; int first=!received;
                AcquireSRWLockExclusive(&g_honk_lock);
                if(g_light.generation==generation) {
                    for(k=0;k<count;k++) g_honk.records[k]=records[k];
                    g_honk.count=count; g_honk.response_at=GetTickCount64();
                    received=light_u64(packet+28);
                }
                ReleaseSRWLockExclusive(&g_honk_lock);
                if(first && received) {log_begin();log_raw("honk: authenticated horn list received after ");log_dec((unsigned long)polls);log_raw(" polls\n");}
            } else rejected++;
        }
        /* a change goes out at once; otherwise a poll every 50 ms while a horn sounds (ours or theirs), every 100 ms else */
        {
            int busy=horn>=0; ULONGLONG gap;
            AcquireSRWLockShared(&g_honk_lock);
            if(g_honk.count && now-g_honk.response_at<=HH_SNAPSHOT_MS) busy=1;
            ReleaseSRWLockShared(&g_honk_lock);
            gap=busy?50:100;
            if((horn!=sent_horn || press!=sent_press) ? now-last_send>=35 : now-last_send>=gap) {
                WSABUF b; DWORD sent=0;
                honk_request(packet,g_ticket_key,g_xuid,++seq,epoch,car,horn<0?HONK_OFF:(unsigned char)horn,press);
                b.len=HONK_REQUEST; b.buf=(char *)packet;
                g_r_wsasendto(s,&b,1,&sent,0,address,length,NULL,NULL);
                last_send=now; sent_horn=horn; sent_press=press;
                if(++polls%200==0) {
                    if(!received && !silent_told) {
                        silent_told=1;
                        log_begin();log_raw("honk: no horn list reply to ");log_dec((unsigned long)polls);
                        log_raw(" polls, the server does not answer horn polls (server without horns, or UDP replies blocked); other players' horns stay silent\n");
                    }
                    if(rejected) {
                        log_begin();log_raw("honk: ");log_dec(rejected);log_raw(" reply datagram(s) refused (not a valid, authentic, in-sequence horn list for this session)\n");
                        rejected=0;
                    }
                }
            }
        }
        Sleep(20);
    }
    if(s!=INVALID_SOCKET && close_socket) close_socket(s);
    return 0;
}
/* A function start that another mod hooked through the platform: MinHook's jmp rel32 or jmp [rip+0] to code outside the game image. The game reaches
 * it the same way, so it is called through that hook (Horizon Wave Radio hooks createSound and passes every sound but its own to the original). */
static int honk_hooked_by_mod(const unsigned char *img, SIZE_T image_size, const unsigned char *at)
{
    const unsigned char *to;
    if(at[0]==0xe9) to=at+5+*(const int *)(at+1);
    else if(at[0]==0xff && at[1]==0x25 && !at[2] && !at[3] && !at[4] && !at[5]) to=*(const unsigned char *const *)(at+6);
    else return 0;
    return to<img || to>=img+image_size;
}
/* -> 1 the function starts with `want`, 2 another mod's hook is in front of it, 0 something else */
static int honk_entry_ok(const unsigned char *img, SIZE_T image_size, unsigned rva, const unsigned char want[8])
{
    unsigned k;
    for(k=0;k<8 && img[rva+k]==want[k];k++) {}
    if(k==8) return 1;
    return honk_hooked_by_mod(img,image_size,img+rva)?2:0;
}
static void honk_paths(const wchar_t *azimuth_dir, const wchar_t *bin_dir)
{
    g_honk_eo_ini[0]=g_honk_status[0]=0;
    if(azimuth_dir && lstrlenW(azimuth_dir)<MAX_PATH) {lstrcpyW(g_honk_eo_ini,azimuth_dir); lstrcatW(g_honk_eo_ini,L"\\FH2_ExtraOptions.ini");}
    if(bin_dir && lstrlenW(bin_dir)<MAX_PATH) {lstrcpyW(g_honk_status,bin_dir); lstrcatW(g_honk_status,L"\\fh2mp-controls.txt"); DeleteFileW(g_honk_status);}
}
/* FH2MP_NO_HONK: the button is not moved; the launcher reads that and leaves Extra Options' toggles on the right stick. */
static void honk_switched_off(const wchar_t *azimuth_dir, const wchar_t *bin_dir)
{
    honk_paths(azimuth_dir,bin_dir);
    honk_write_status(0,0,"FH2MP_NO_HONK");
}
/* The horn button is placed in every case (its own step); sharing horns needs the FMOD functions and the audio update. -> 1 horns are shared */
static int patch_honk(const wchar_t *azimuth_dir, const wchar_t *bin_dir)
{
    static const struct { unsigned rva; unsigned char bytes[8]; const char *what; } checks[]={
        {0x10140d0,{0x48,0x8b,0x89,0xf0,0x07,0x00,0x00,0xe9},"the audio engine update"},
        {0x1a98080,{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74},"FMOD Studio getLowLevelSystem"},
        {0x1997740,{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41},"FMOD createSound"},
        {0x199b0f0,{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41},"FMOD playSound"},
        {0x1997c00,{0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41},"FMOD get3DListenerAttributes"},
        {0x19a3f50,{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41},"FMOD set3DAttributes"},
        {0x19a4bc0,{0x48,0x8b,0xc4,0x55,0x56,0x57,0x48,0x81},"FMOD set3DMinMaxDistance"},
        {0x19a6470,{0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x80},"FMOD setVolume"},
        {0x19a5e50,{0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x70},"FMOD setPaused"},
        {0x19a6730,{0x40,0x57,0x48,0x81,0xec,0x70,0x01,0x00},"FMOD stop"},
        {0x19a0960,{0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41},"FMOD setLoopPoints"},
    };
    unsigned char *img=find_fh2_image(); unsigned i; HANDLE thread; int share=img!=NULL;
    honk_paths(azimuth_dir,bin_dir);
    if(!img) {log_begin();log_raw("honk: other players' horns are not shared (the game is not FH2 1.0.0.64); the horn button is still placed\n");}
    for(i=0;share && i<sizeof checks/sizeof checks[0];i++) {
        int ok=honk_entry_ok(img,FH2_SIZE_OF_IMAGE,checks[i].rva,checks[i].bytes);
        if(ok==2) {
            log_begin();log_raw("honk: ");log_raw(checks[i].what);log_raw(" at game+0x");log_hex(checks[i].rva,8);
            log_raw(" is hooked by another mod; it is used through that hook, as the game uses it\n");
        } else if(!ok) {
            log_begin();log_raw("honk: unexpected bytes in ");log_raw(checks[i].what);log_raw(" at game+0x");log_hex(checks[i].rva,8);
            log_raw(", other players' horns are not shared; the horn button is still placed\n");
            share=0;
        }
    }
    if(share) {
        g_honk_fmod.get_low_level=(void *)(img+0x1a98080); g_honk_fmod.create_sound=(void *)(img+0x1997740); g_honk_fmod.play_sound=(void *)(img+0x199b0f0);
        g_honk_fmod.get_listener=(void *)(img+0x1997c00); g_honk_fmod.set_3d=(void *)(img+0x19a3f50); g_honk_fmod.set_min_max=(void *)(img+0x19a4bc0);
        g_honk_fmod.set_volume=(void *)(img+0x19a6470); g_honk_fmod.set_paused=(void *)(img+0x19a5e50); g_honk_fmod.stop=(void *)(img+0x19a6730);
        g_honk_fmod.set_loop_points=(void *)(img+0x19a0960);
        InterlockedExchange(&g_honk_enabled,1);
        if(!plat_hook("honk: audio update",img+HH_SYSTEM_RVA,(void *)honk_audio_update,(void **)&g_honk_update_original)) {
            log_begin();log_raw("honk: other players' horns cannot be played, the audio update could not be hooked (the horn button and your own horn are still handled)\n");
        }
    }
    thread=CreateThread(NULL,0,honk_worker,NULL,0,NULL);
    if(!thread) {
        DWORD err=GetLastError();
        InterlockedExchange(&g_honk_enabled,0);
        log_begin();log_raw("honk: not enabled and the horn button stays on the left stick, the thread could not be created, error ");log_dec(err);log_raw("\n");
        honk_write_status(0,0,"no thread");
        return 0;
    }
    CloseHandle(thread);
    return share;
}
