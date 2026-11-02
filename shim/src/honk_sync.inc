/* Horns of other players (issue 293). HonkHonk (suderek22, fh2-mods\HonkHonk.dll 0.3.4, a required add-on) plays the local horn through the game's
 * own FMOD; this file reads which horn the player is holding out of HonkHonk's memory, tells the server (a side channel like lighting, server/honk.py,
 * its own magic FH2MPHN1) and plays the horns the server lists at those cars with the same FMOD calls and the same bank. HonkHonk itself is not changed
 * on disk; the addresses below are for the pinned DLL (sha256 4ed6ba03...) and are checked byte for byte, another build is left alone.
 *
 * Controls: HonkHonk honks on the left stick (L3, XInput bit 0x40, one `test byte [rsp+0x2c], 0x40` in its input loop). Forza honks on the right
 * stick and Extra Options' headlights are moved to the left stick by the launcher, so the immediate is changed to 0x80 in memory.
 *
 * Remote voices run on the game's audio update (AudioEngineFmod::vfunc_5, 0x10140d0: Studio::System::update(*(this+0x7f0)), the call HonkHonk hooks
 * one level down), so every FMOD call is made on the thread HonkHonk's own calls use. Three limits keep a voice from sticking: the server's 0.5 s
 * lease (a horn nobody refreshes is gone from the list), 10 s per press (server and here), and a voice whose owner is missing from the list for 0.3 s
 * stops. The network thread never touches FMOD or a car. Needs the lighting hooks (the server address, ticket epoch and car id are theirs). */
#include "honk_wire.h"

#define HH_REQUEST_RVA 0x6840u        /* fmodhorn::request(int): mov [rip+disp32], ecx ; ret, the horn index or -1 */
#define HH_BUTTON_RVA 0x222du         /* test byte [rsp+0x2c], 0x40 ; jne +5 : XINPUT_GAMEPAD_LEFT_THUMB */
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
    g_honk_update_original(engine);
    if(g_honk_enabled) honk_audio_tick(engine);
}

/* ---- HonkHonk.dll: the horn it holds, its button, its bank ---- */
static const unsigned char honk_request_code[7]={0x89,0x0d,0xfe,0x77,0x00,0x00,0xc3};
static const unsigned char honk_button_code[7]={0xf6,0x44,0x24,0x2c,0x40,0x75,0x05};
static const unsigned char honk_moved_code[7]={0xf6,0x44,0x24,0x2c,0x80,0x75,0x05};
static unsigned char *g_honk_request;                /* HonkHonk's request variable, NULL until the pinned build is recognised */

static int honk_code_is(unsigned char *at, const unsigned char *code, unsigned n)
{
    unsigned char now[8]; unsigned i;
    if(!read_game_memory(at,now,n)) return 0;
    for(i=0;i<n;i++) if(now[i]!=code[i]) return 0;
    return 1;
}
/* Left stick -> right stick in HonkHonk's input loop (the immediate of the one `test byte [rsp+0x2c], imm8`). */
static int honk_move_button(unsigned char *hh)
{
    unsigned char *imm=hh+HH_BUTTON_RVA+4; DWORD old;
    if(!VirtualProtect(imm,1,PAGE_EXECUTE_READWRITE,&old)) return 0;
    *imm=0x80;
    FlushInstructionCache(GetCurrentProcess(),imm,1);
    VirtualProtect(imm,1,old,&old);
    return 1;
}
/* -> 1 when HonkHonk.dll is loaded and recognised (sets g_honk_request, moves the button, loads the bank), 0 not loaded yet, -1 another build (said once) */
static int honk_attach(void)
{
    static int told_other, attached;
    HMODULE m=GetModuleHandleW(L"HonkHonk.dll"); unsigned char *hh=(unsigned char *)m; wchar_t path[MAX_PATH+40]; DWORD n, i, slash=0;
    if(attached) return 1;
    if(!hh) return 0;
    if(!honk_code_is(hh+HH_REQUEST_RVA,honk_request_code,7) ||
       (!honk_code_is(hh+HH_BUTTON_RVA,honk_button_code,7) && !honk_code_is(hh+HH_BUTTON_RVA,honk_moved_code,7))) {
        if(!told_other) {
            unsigned char now[7];
            told_other=1;
            log_begin();log_raw("honk: unexpected HonkHonk.dll (only 0.3.4 is known), its horn is not shared and its button stays where it is; at +0x");
            log_hex(HH_BUTTON_RVA,4);log_raw(" found ");
            if(read_game_memory(hh+HH_BUTTON_RVA,now,7)) log_hexbytes(now,7); else log_raw("(unreadable)");
            log_raw("\n");
        }
        return -1;
    }
    if(honk_code_is(hh+HH_BUTTON_RVA,honk_button_code,7) && !honk_move_button(hh)) {
        if(!told_other) {told_other=1;log_begin();log_raw("honk: could not move HonkHonk's horn button to the right stick\n");}
        return -1;
    }
    g_honk_request=hh+HH_REQUEST_RVA+6+*(const int *)(honk_request_code+2);      /* mov [rip+disp32], ecx: next instruction + disp */
    attached=1;
    log_begin();log_raw("honk: HonkHonk.dll found, its horn is read from its own state and its button is the right stick\n");
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
static int patch_honk(void)
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
    unsigned char *img=find_fh2_image(); unsigned i; HANDLE thread;
    if(!img) return 0;
    for(i=0;i<sizeof checks/sizeof checks[0];i++) {
        unsigned k;
        for(k=0;k<8 && img[checks[i].rva+k]==checks[i].bytes[k];k++) {}
        if(k<8) {
            log_begin();log_raw("honk: unexpected bytes in ");log_raw(checks[i].what);log_raw(" at game+0x");log_hex(checks[i].rva+k,8);
            log_raw(", horns are not shared; not enabled\n");
            return 0;
        }
    }
    g_honk_fmod.get_low_level=(void *)(img+0x1a98080); g_honk_fmod.create_sound=(void *)(img+0x1997740); g_honk_fmod.play_sound=(void *)(img+0x199b0f0);
    g_honk_fmod.get_listener=(void *)(img+0x1997c00); g_honk_fmod.set_3d=(void *)(img+0x19a3f50); g_honk_fmod.set_min_max=(void *)(img+0x19a4bc0);
    g_honk_fmod.set_volume=(void *)(img+0x19a6470); g_honk_fmod.set_paused=(void *)(img+0x19a5e50); g_honk_fmod.stop=(void *)(img+0x19a6730);
    g_honk_fmod.set_loop_points=(void *)(img+0x19a0960);
    InterlockedExchange(&g_honk_enabled,1);
    if(!plat_hook("honk: audio update",img+HH_SYSTEM_RVA,(void *)honk_audio_update,(void **)&g_honk_update_original)) {
        log_begin();log_raw("honk: other players' horns cannot be played, the audio update could not be hooked (the horn button and your own horn are still handled)\n");
    }
    thread=CreateThread(NULL,0,honk_worker,NULL,0,NULL);
    if(!thread) {
        DWORD err=GetLastError();
        InterlockedExchange(&g_honk_enabled,0);
        log_begin();log_raw("honk: not enabled, the thread could not be created, error ");log_dec(err);log_raw("\n");
        return 0;
    }
    CloseHandle(thread);
    return 1;
}
