/* Isolated tests of src/honk_wire.h and src/honk_sync.inc: no game is loaded, no network calls are made, FMOD is a recording fake. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/sha256.inc"
static unsigned char g_ticket_key[32];
static int g_ticket_ready;
static unsigned long long g_xuid;
#define FH2_SIZE_OF_IMAGE 0x3a18000u
static int (WINAPI *g_r_wsasendto)(UINT_PTR,void *,DWORD,DWORD *,DWORD,const void *,int,void *,void *);
static int read_game_memory(const void *p,void *out,SIZE_T n) { if(!p)return 0;memcpy(out,p,n);return 1; }
static unsigned char *find_fh2_image(void) {return NULL;}
static int plat_hook(const char *name,void *target,void *detour,void **original) {(void)name;(void)target;(void)detour;(void)original;return 0;}
static void log_begin(void) {}
static void log_raw(const char *s) {(void)s;}
static void log_hex(unsigned long long value,int digits) {(void)value;(void)digits;}
static void log_hexbytes(const unsigned char *p,unsigned n) {(void)p;(void)n;}
static void log_dec(unsigned long value) {(void)value;}
#include "src/lighting_sync.inc"
#include "src/honk_sync.inc"

static void resign(unsigned char *data,unsigned n) {hmac_sha256(g_ticket_key,32,data,n-32,data+n-32);}
static int hex_value(int c) {return c>='a' ? c-'a'+10 : c>='A' ? c-'A'+10 : c-'0';}
static unsigned read_hex(const char *path,unsigned char *out,unsigned max)
{
    FILE *f=fopen(path,"r"); char line[2048]; unsigned n=0,i;
    assert(f && fgets(line,sizeof line,f)); fclose(f);
    for(i=0;line[i] && line[i+1] && line[i]!='\n' && n<max;i+=2) out[n++]=(unsigned char)(hex_value(line[i])*16+hex_value(line[i+1]));
    return n;
}

/* ---- FMOD fake ---- */
static struct { int created, played, stopped, set3d, volume, minmax, unpaused, loops; unsigned last_mode, last_length; float pos[3], vel[3], vol; unsigned fail_play; } fm;
static int fake_sound_token[HONK_HORNS], fake_channel_token[64];
static int fake_create(void *core,const void *data,unsigned mode,void *info,void **sound)
{
    (void)core;
    fm.last_mode=mode; fm.last_length=*(unsigned *)((unsigned char *)info+4);
    assert(*(unsigned *)info==0xd8 && !memcmp(data,"RIFF",4) && !memcmp((const char *)data+8,"WAVEfmt ",8));
    *sound=&fake_sound_token[fm.created%HONK_HORNS]; fm.created++; return 0;
}
static int fake_play(void *core,void *sound,void *group,int paused,void **channel)
{
    (void)core;(void)sound;(void)group; assert(paused);
    if(fm.fail_play) return 1;
    *channel=&fake_channel_token[fm.played%64]; fm.played++; return 0;
}
static int fake_listener(void *core,int i,float *p,float *v,float *f,float *u)
{
    (void)core;(void)i;(void)p;(void)f;(void)u; v[0]=1;v[1]=2;v[2]=3; return 0;
}
static int fake_3d(void *ch,const float *p,const float *v,const float *a)
{
    (void)ch;(void)a; fm.set3d++; memcpy(fm.pos,p,12); memcpy(fm.vel,v,12); return 0;
}
static int fake_minmax(void *ch,float a,float b) {(void)ch; assert(a==5.0f && b==150.0f); fm.minmax++; return 0;}
static int fake_volume(void *ch,float v) {(void)ch; fm.volume++; fm.vol=v; return 0;}
static int fake_paused(void *ch,int p) {(void)ch; if(!p) fm.unpaused++; return 0;}
static int fake_stop(void *ch) {(void)ch; fm.stopped++; return 0;}
static int fake_loop(void *s,unsigned a,unsigned at,unsigned b,unsigned bt) {(void)s;(void)at;(void)bt; assert(a==100 && b==999); fm.loops++; return 0;}

/* ---- a synthetic 31 sample FSB5: sample i has 1000+i frames; sample 3 has a loop chunk 100..1000 ---- */
static unsigned char *make_bank(DWORD *size)
{
    unsigned i, shs=0, ds=0; unsigned char *b; unsigned long long off=0; unsigned p=60;
    unsigned frames[HONK_HORNS];
    for(i=0;i<HONK_HORNS;i++) {frames[i]=1000+i; shs+=8+(i==3?4+8:0); ds+=((frames[i]*2+31)/32)*32;}
    b=calloc(60+shs+ds,1); memcpy(b,"FSB5",4);
    {unsigned h[6]={1,HONK_HORNS,shs,0,ds,2}; memcpy(b+4,h,sizeof h);}
    for(i=0;i<HONK_HORNS;i++) {
        unsigned long long v=((unsigned long long)frames[i]<<34) | ((off/32)<<7) | (i==3?1:0);
        memcpy(b+p,&v,8); p+=8;
        if(i==3) {unsigned c=(8u<<1)|(3u<<25); unsigned loop[2]={100,1000}; memcpy(b+p,&c,4); p+=4; memcpy(b+p,loop,8); p+=8;}
        {unsigned o; for(o=0;o<frames[i]*2;o++) b[60+shs+off+o]=(unsigned char)(i+o);}
        off+=((frames[i]*2+31)/32)*32;
    }
    *size=60+shs+ds; return b;
}
static void reset_voices(void)
{
    unsigned i;
    memset(&fm,0,sizeof fm);
    memset(g_honk_voices,0,sizeof g_honk_voices); memset(g_honk_capped_set,0,sizeof g_honk_capped_set); memset(g_honk_sounds,0,sizeof g_honk_sounds);
    g_honk_sound_failed=0; g_honk.count=0; g_honk.response_at=0;
    for(i=0;i<32;i++) g_light_cars[i].car=NULL;
}
static void offer(ULONGLONG now,unsigned n,const honk_record *r)
{
    unsigned i; for(i=0;i<n;i++) g_honk.records[i]=r[i]; g_honk.count=n; g_honk.response_at=now;
}
static void write_file(const wchar_t *path,const char *text)
{
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); DWORD n;
    assert(f!=INVALID_HANDLE_VALUE); WriteFile(f,text,(DWORD)strlen(text),&n,NULL); CloseHandle(f);
}
static int read_status(char *out,unsigned max)
{
    HANDLE f=CreateFileW(g_honk_status,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL); DWORD n=0;
    if(f==INVALID_HANDLE_VALUE) return 0;
    ReadFile(f,out,max-1,&n,NULL); CloseHandle(f); out[n]=0; return 1;
}
static honk_record rec(unsigned owner,unsigned horn,unsigned press,unsigned age,float x)
{
    honk_record r; memset(&r,0,sizeof r); r.owner=(unsigned char)owner; r.car=5; r.horn=(unsigned char)horn; r.flags=1; r.press=(unsigned short)press; r.age=(unsigned short)age;
    r.pos[0]=x; r.pos[1]=1; r.pos[2]=2; return r;
}
int main(int argc,char **argv)
{
    unsigned char packet[HONK_LIMIT],epoch[16],copy[HONK_LIMIT]; unsigned i,n,count=0; honk_record out[HONK_MAX];
    for(i=0;i<32;i++)g_ticket_key[i]=(unsigned char)i;
    for(i=0;i<16;i++)epoch[i]=(unsigned char)(16+i);
    g_xuid=0x9000123456789ull;
    honk_request(packet,g_ticket_key,g_xuid,7,epoch,5,HONK_OFF,0);
    if(argc>1 && !strcmp(argv[1],"--vector")) {for(i=0;i<HONK_REQUEST;i++)printf("%02x",packet[i]);puts("");return 0;}
    if(argc>2 && !strcmp(argv[1],"--reply")) {
        /* a reply built by server/honk.py to the request above: the other player (owner 10, car 6) holds horn 7, press 300, at (10, 20, -30) */
        n=read_hex(argv[2],packet,sizeof packet);
        assert(honk_response(packet,n,g_ticket_key,g_xuid,7,0,epoch,out,&count));
        assert(count==1 && out[0].owner==10 && out[0].car==6 && out[0].horn==7 && out[0].flags==1 && out[0].press==300 && out[0].age<=0xffff);
        assert(out[0].pos[0]==10.0f && out[0].pos[1]==20.0f && out[0].pos[2]==-30.0f);
        puts("C/Python horn reply: PASS");
        return 0;
    }
    /* ---- wire ---- */
    assert(packet[19]==1 && packet[53]==HONK_OFF && packet[55]==0);
    honk_request(packet,g_ticket_key,g_xuid,8,epoch,5,7,0x1234);
    assert(packet[52]==5 && packet[53]==7 && packet[55]==0x34 && packet[56]==0x12 && light_u64(packet+28)==8);
    {
        unsigned char reply[HONK_LIMIT]; unsigned len;
        memcpy(reply,packet,HONK_BASE); reply[19]=2; reply[54]=1;
        memset(reply+HONK_BASE,0,HONK_RECORD);
        reply[HONK_BASE]=10; reply[HONK_BASE+1]=6; reply[HONK_BASE+2]=7; reply[HONK_BASE+3]=1; reply[HONK_BASE+4]=44; reply[HONK_BASE+5]=1;
        {float p[3]={1.5f,-2.5f,3.5f}; memcpy(reply+HONK_BASE+8,p,12);}
        len=HONK_REQUEST+HONK_RECORD; reply[9]=(unsigned char)len; reply[10]=(unsigned char)(len>>8); resign(reply,len);
        assert(!honk_response(packet,HONK_REQUEST,g_ticket_key,g_xuid,8,0,epoch,out,&count));             /* the reflected request */
        assert(honk_response(reply,len,g_ticket_key,g_xuid,8,0,epoch,out,&count) && count==1);
        assert(out[0].owner==10 && out[0].press==300 && out[0].pos[1]==-2.5f);
        assert(honk_response(reply,len,g_ticket_key,g_xuid,30,7,epoch,out,&count));                         /* a delayed reply */
        assert(!honk_response(reply,len,g_ticket_key,g_xuid,8,8,epoch,out,&count));                        /* replay */
        assert(!honk_response(reply,len,g_ticket_key,g_xuid,7,0,epoch,out,&count));                        /* from the future */
        assert(!honk_response(reply,len,g_ticket_key,g_xuid,60,0,epoch,out,&count));                       /* too old */
        assert(!honk_response(reply,len-1,g_ticket_key,g_xuid,8,0,epoch,out,&count));
        assert(!honk_response(reply,len,g_ticket_key,g_xuid+1,8,0,epoch,out,&count));
        epoch[0]^=1; assert(!honk_response(reply,len,g_ticket_key,g_xuid,8,0,epoch,out,&count)); epoch[0]^=1;
        memcpy(copy,reply,len); copy[HONK_BASE+8]^=1; assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));
        memcpy(copy,reply,len); copy[HONK_BASE+2]=31; resign(copy,len); assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));   /* no such horn */
        memcpy(copy,reply,len); copy[HONK_BASE]=0; resign(copy,len); assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));      /* owner 0 */
        memcpy(copy,reply,len); copy[HONK_BASE+3]=2; resign(copy,len); assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));
        memcpy(copy,reply,len); {unsigned char nan[4]={0,0,0xc0,0x7f}; memcpy(copy+HONK_BASE+8,nan,4);} resign(copy,len);
        assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));                         /* not a finite position */
        memcpy(copy,reply,len); copy[54]=2; memcpy(copy+HONK_BASE+HONK_RECORD,copy+HONK_BASE,HONK_RECORD); len+=HONK_RECORD; copy[9]=(unsigned char)len; copy[10]=(unsigned char)(len>>8);
        resign(copy,len); assert(!honk_response(copy,len,g_ticket_key,g_xuid,8,0,epoch,out,&count));       /* the same owner twice */
    }
    /* ---- the bank ---- */
    {
        DWORD size; unsigned char *bank=make_bank(&size); honk_sample s[HONK_HORNS];
        assert(honk_parse_bank(bank,size,s));
        assert(s[0].bytes==2000 && s[30].bytes==2060 && s[3].loop_start==100 && s[3].loop_end==1000 && s[2].loop_end==0);
        assert(bank[s[5].off]==5 && bank[s[5].off+1]==6);
        bank[0]='X'; assert(!honk_parse_bank(bank,size,s)); bank[0]='F';
        assert(!honk_parse_bank(bank,size-1,s));                                                           /* data cut short */
        {unsigned h=30; memcpy(bank+8,&h,4);} assert(!honk_parse_bank(bank,size,s));                        /* 30 sounds: not the pinned bank */
        {unsigned h=HONK_HORNS; memcpy(bank+8,&h,4);}
        {unsigned m=3; memcpy(bank+24,&m,4);} assert(!honk_parse_bank(bank,size,s));                        /* not PCM16 */
        {unsigned m=2; memcpy(bank+24,&m,4);}
        assert(honk_parse_bank(bank,size,g_honk_samples)); g_honk_bank=bank; g_honk_bank_size=size; g_honk_bank_ok=1;
        {
            DWORD len; unsigned char *w=honk_wav(5,&len);
            assert(w && len==44+2000+10 && !memcmp(w,"RIFF",4) && *(unsigned *)(w+4)==len-8 && *(unsigned *)(w+40)==2010 && w[22]==1 && w[34]==16 && *(unsigned *)(w+24)==44100);
            assert(w[44]==5 && w[45]==6);
            HeapFree(GetProcessHeap(),0,w);
        }
        assert(!honk_wav(31,&size));
    }
    /* ---- voices ---- */
    g_honk_fmod.create_sound=fake_create; g_honk_fmod.play_sound=fake_play; g_honk_fmod.get_listener=fake_listener; g_honk_fmod.set_3d=fake_3d;
    g_honk_fmod.set_min_max=fake_minmax; g_honk_fmod.set_volume=fake_volume; g_honk_fmod.set_paused=fake_paused; g_honk_fmod.stop=fake_stop; g_honk_fmod.set_loop_points=fake_loop;
    g_honk_core=&fm; g_honk_volume=0.7f;
    {
        ULONGLONG t=100000; honk_record r=rec(10,3,1,50,40.0f);
        reset_voices();
        offer(t,1,&r); honk_voices_step(t);                                                                 /* starts, from the server's position, with the loop of horn 3 */
        assert(fm.created==1 && fm.played==1 && fm.unpaused==1 && fm.loops==1 && fm.minmax==1 && fm.volume==1 && fm.vol==0.7f);
        assert(fm.last_mode==(0x800|0x100|0x10|0x2|0x80000|0x100000) && fm.pos[0]==40.0f && fm.vel[0]==1 && fm.vel[2]==3);
        t+=50; offer(t,1,&r); r.pos[0]=41.0f; offer(t,1,&r); honk_voices_step(t);                          /* the same press: only moves */
        assert(fm.played==1 && fm.pos[0]==41.0f);
        t+=250; honk_voices_step(t); assert(fm.stopped==0);                                                  /* the list is 250 ms old: still sounding */
        t+=100; honk_voices_step(t); assert(fm.stopped==1 && !g_honk_voices[0].channel);                     /* missing for more than 300 ms: stopped */
        reset_voices(); offer(t,1,&r); honk_voices_step(t);
        t+=100; r.press=2; offer(t,1,&r); honk_voices_step(t);                                               /* a new press: the old voice ends, a new one starts, same sound */
        assert(fm.played==2 && fm.stopped==1 && fm.created==1);
        t+=100; r.horn=4; offer(t,1,&r); honk_voices_step(t); assert(fm.played==3 && fm.created==2);        /* another horn of the same player */
        t+=HH_SNAPSHOT_MS+1; honk_voices_step(t); assert(fm.stopped==3 || fm.stopped==2);                    /* a snapshot older than 0.6 s counts as an empty one */
        for(i=0;i<HH_VOICES;i++) assert(!g_honk_voices[i].channel);
    }
    {   /* the 10 s cap: the voice ends and the same press does not restart, a new one does */
        ULONGLONG t=200000; honk_record r=rec(11,1,7,0,0);
        reset_voices(); offer(t,1,&r); honk_voices_step(t);
        for(i=0;i<110;i++) {t+=100; r.age=(unsigned short)(i*100+100); offer(t,1,&r); honk_voices_step(t);}
        assert(fm.stopped==1 && fm.played==1);
        t+=100; offer(t,1,&r); honk_voices_step(t); assert(fm.played==1);
        r.press=8; r.age=0; offer(t,1,&r); honk_voices_step(t); assert(fm.played==2);
        reset_voices(); r=rec(11,1,9,HONK_CAP_MS,0); offer(t,1,&r); honk_voices_step(t); assert(fm.played==0);   /* already past the cap when it arrives */
    }
    {   /* position: the car's own when it agrees with the server within 100 m, the server's otherwise, nothing without either */
        ULONGLONG t=300000; honk_record r=rec(12,2,1,0,50.0f); static unsigned char car[0x5000], state[0x1000];
        reset_voices();
        *(void **)(car+0x4c08)=state; {float p[3]={52,1,2}; memcpy(state+0xef0,p,12);}
        g_light_cars[0]=(light_remote_car){car,NULL,5,12,GetTickCount64()};
        offer(t,1,&r); honk_voices_step(t); assert(fm.pos[0]==52.0f);
        {float p[3]={500,1,2}; memcpy(state+0xef0,p,12);} t+=50; offer(t,1,&r); honk_voices_step(t); assert(fm.pos[0]==50.0f);
        reset_voices(); r.flags=0; offer(t,1,&r); honk_voices_step(t); assert(fm.played==0);                  /* no server position, the car's is far from nothing: wait */
        {float p[3]={9,9,9}; memcpy(state+0xef0,p,12);} g_light_cars[0]=(light_remote_car){car,NULL,5,12,GetTickCount64()};
        offer(t,1,&r); honk_voices_step(t); assert(fm.played==1 && fm.pos[0]==9.0f);
    }
    {   /* eight voices at most; a failing playSound starts nothing and counts nothing */
        ULONGLONG t=400000; honk_record rs[10]; reset_voices();
        for(i=0;i<10;i++) rs[i]=rec(20+i,i,1,0,(float)i);
        offer(t,HONK_MAX<10?HONK_MAX:10,rs); honk_voices_step(t); assert(fm.played==HH_VOICES);
        reset_voices(); fm.fail_play=1; offer(t,1,rs); honk_voices_step(t); assert(fm.played==0 && !g_honk_voices[0].channel);
    }
    /* ---- HonkHonk.dll: the horn it holds ---- */
    {
        int held;
        /* HonkHonk's request variable, as honk_attach derives it from the pinned instruction */
        assert((HH_REQUEST_RVA+6+*(const int *)(honk_request_code+2))==0xe044u);
        held=5; g_honk_request=(unsigned char *)&held; assert(honk_local_horn()==5);
        held=-1; assert(honk_local_horn()==-1);
        held=31; assert(honk_local_horn()==-1);
        g_honk_request=NULL; assert(honk_local_horn()==-1);
    }
    {   /* the button: the left stick test becomes the right stick test, in a copy of the pinned instruction bytes at image base + 0x222d */
        unsigned char *base=VirtualAlloc(NULL,0x10000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); DWORD old;
        assert(base);
        memcpy(base+HH_BUTTON_RVA,honk_button_code,7);
        VirtualProtect(base,0x10000,PAGE_EXECUTE_READ,&old);
        assert(honk_move_button(base) && base[HH_BUTTON_RVA+4]==0x80 && honk_code_is(base+HH_BUTTON_RVA,honk_moved_code,7));
        assert(!honk_code_is(base+HH_BUTTON_RVA,honk_button_code,7));
        VirtualFree(base,0,MEM_RELEASE);
    }
    {   /* which stick the horn goes on: never the one Extra Options' headlight/roof toggle uses, and the launcher is told */
        wchar_t dir[MAX_PATH], ini[MAX_PATH+40]; char st[256]; unsigned char *base; DWORD old;
        static const char swapped[]="; Extra Options\r\n[Bindings]\r\nHeadlightsXbox=LeftStick\r\nHeadlightsKeyboard=F3\r\nRoofXbox = leftstick \r\n";
        static const char stock[]="[Bindings]\r\nHeadlightsXbox=RightStick\r\nRoofXbox=RightStick\r\n";
        assert(honk_binding_sticks(L"LeftStick")==HH_STICK_LEFT && honk_binding_sticks(L"rightstick")==HH_STICK_RIGHT);
        assert(honk_binding_sticks(L"LB+RS")==HH_STICK_RIGHT && honk_binding_sticks(L" LS + RightStick ")==(HH_STICK_LEFT|HH_STICK_RIGHT));
        assert(honk_binding_sticks(L"None")==0 && honk_binding_sticks(L"")==0 && honk_binding_sticks(L"DPadDown")==0 && honk_binding_sticks(L"RightStickX")==0);
        assert(honk_horn_goes_right(HH_STICK_LEFT) && honk_horn_goes_right(0) && !honk_horn_goes_right(HH_STICK_RIGHT));
        assert(honk_horn_goes_right(HH_STICK_LEFT|HH_STICK_RIGHT));                                        /* toggles on both: Forza's own horn stick */
        GetTempPathW(MAX_PATH,dir); lstrcatW(dir,L"fh2mp-honk-test"); CreateDirectoryW(dir,NULL);
        honk_paths(dir,dir); lstrcpyW(ini,g_honk_eo_ini);
        assert(!read_status(st,sizeof st));
        assert(honk_eo_sticks(ini)==HH_STICK_RIGHT);                                                        /* no ini: Extra Options' default */
        write_file(ini,swapped); assert(honk_eo_sticks(ini)==HH_STICK_LEFT);
        write_file(ini,"[Bindings]\r\nHeadlightsXbox=LeftStick\r\n"); assert(honk_eo_sticks(ini)==(HH_STICK_LEFT|HH_STICK_RIGHT));   /* RoofXbox missing */
        base=VirtualAlloc(NULL,0x10000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(base);
        /* swapped toggles: the horn goes right */
        write_file(ini,swapped); memcpy(base+HH_BUTTON_RVA,honk_button_code,7); VirtualProtect(base,0x10000,PAGE_EXECUTE_READ,&old);
        assert(honk_place_button(base)==1 && base[HH_BUTTON_RVA+4]==0x80);
        assert(read_status(st,sizeof st) && !strncmp(st,"horn=RightStick\r\nmovable=1\r\n",28));
        assert(honk_place_button(base)==1);                                                                 /* already moved */
        /* the stock toggles (the launcher did not or could not move them): the horn stays left, no conflict either way */
        VirtualProtect(base,0x10000,PAGE_READWRITE,&old); memcpy(base+HH_BUTTON_RVA,honk_button_code,7);
        write_file(ini,stock); assert(honk_place_button(base)==0 && base[HH_BUTTON_RVA+4]==0x40);
        assert(read_status(st,sizeof st) && !strncmp(st,"horn=LeftStick\r\nmovable=1\r\n",27));
        /* another HonkHonk build: nothing changed, and the launcher learns the horn cannot be moved */
        write_file(ini,swapped); base[HH_BUTTON_RVA+4]=0x20;
        assert(honk_place_button(base)==0 && base[HH_BUTTON_RVA+4]==0x20);
        assert(read_status(st,sizeof st) && !strncmp(st,"horn=LeftStick\r\nmovable=0\r\n",27));
        honk_switched_off(dir,dir); assert(read_status(st,sizeof st) && strstr(st,"movable=0") && strstr(st,"FH2MP_NO_HONK"));
        honk_paths(dir,dir); assert(!read_status(st,sizeof st));                                           /* a new game drops the last game's note */
        VirtualFree(base,0,MEM_RELEASE); DeleteFileW(ini); RemoveDirectoryW(dir);
        g_honk_eo_ini[0]=g_honk_status[0]=0;
    }
    {   /* a function start: its own bytes, another mod's platform hook in front of it (called through), or something else */
        static const unsigned char want[8]={0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41};
        unsigned char *img=VirtualAlloc(NULL,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE), *far_away=VirtualAlloc(NULL,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        int rel; assert(img && far_away);
        memcpy(img+0x100,want,8); assert(honk_entry_ok(img,0x1000,0x100,want)==1);
        /* MinHook's jmp rel32 to its relay outside the image (what Horizon Wave Radio's createSound hook leaves) */
        img[0x100]=0xe9; rel=(int)(0x800-0x105); memcpy(img+0x101,&rel,4); assert(honk_entry_ok(img,0x400,0x100,want)==2);
        assert(honk_entry_ok(img,0x1000,0x100,want)==0);                                                     /* a jump inside the game: not a hook */
        memset(img+0x100,0,16); img[0x100]=0xff; img[0x101]=0x25; memcpy(img+0x106,&far_away,8); assert(honk_entry_ok(img,0x1000,0x100,want)==2);
        memcpy(img+0x106,&img,8); assert(honk_entry_ok(img,0x1000,0x100,want)==0);
        img[0x102]=1; memcpy(img+0x106,&far_away,8); assert(honk_entry_ok(img,0x1000,0x100,want)==0);       /* jmp [rip+1]: not MinHook's form */
        memcpy(img+0x100,want,8); img[0x107]^=1; assert(honk_entry_ok(img,0x1000,0x100,want)==0);            /* other code */
        VirtualFree(img,0,MEM_RELEASE); VirtualFree(far_away,0,MEM_RELEASE);
    }
    puts("honk: PASS");
    return 0;
}
