/* Isolated tests of src/carsel_wire.h and src/carsel_sync.inc (the wire, the hold decision, the group deadline rules and the detours): no game is loaded, no
 * network calls are made. The game's memory (network manager, clock, loading message component) is faked in this process. */
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
static int code_is(const char *tag,const char *what,const unsigned char *img,unsigned rva,const void *expect,unsigned n) {(void)tag;(void)what;return !memcmp(img+rva,expect,n);}
static void mem_copy(void *to,const void *from,SIZE_T n) {memcpy(to,from,n);}
static char g_log[8192];
static void log_begin(void) {strcat(g_log,"|");}
static void log_raw(const char *s) {strcat(g_log,s);}
static void log_hex(unsigned long long value,int digits) {(void)value;(void)digits;}
static void log_hexbytes(const unsigned char *p,unsigned n) {(void)p;(void)n;}
static void log_dec(unsigned long value) {char b[24]; sprintf(b,"%lu",value); strcat(g_log,b);}
static ULONGLONG g_fake_now;
#define CARSEL_NOW() g_fake_now
#include "src/lighting_sync.inc"
#include "src/honk_sync.inc"
#include "src/carsel_sync.inc"

static void resign(unsigned char *data,unsigned n) {hmac_sha256(g_ticket_key,32,data,n-32,data+n-32);}
static int hex_value(int c) {return c>='a' ? c-'a'+10 : c>='A' ? c-'A'+10 : c-'0';}
static unsigned read_hex(const char *path,unsigned char *out,unsigned max)
{
    FILE *f=fopen(path,"r"); char line[2048]; unsigned n=0,i;
    assert(f && fgets(line,sizeof line,f)); fclose(f);
    for(i=0;line[i] && line[i+1] && line[i]!='\n' && n<max;i+=2) out[n++]=(unsigned char)(hex_value(line[i])*16+hex_value(line[i+1]));
    return n;
}
static int g_original_calls;
static void fake_original(void *self,ULONGLONG a,ULONGLONG b,ULONGLONG c) {(void)self;(void)a;(void)b;(void)c;g_original_calls++;}

/* ---- the faked game ---- */
static unsigned char g_fake_manager[0x100];
static unsigned char *g_fake_manager_slot=g_fake_manager;           /* DAT_1434d6348 */
static unsigned g_fake_clock_ms;
static unsigned fake_clock(void *obj) {(void)obj;return g_fake_clock_ms;}
static void *g_fake_vtable[8]={0,0,0,0,0,0,0,(void *)fake_clock};
static void *g_fake_clock_object[1]={g_fake_vtable};
static void *g_fake_clock_slot=g_fake_clock_object;                   /* DAT_1436e2450 */
static unsigned char g_fake_comp[0x100];
static int g_wait_message_calls,g_final_calls,g_count_calls;
static unsigned *fake_u32(unsigned char *base,unsigned offset) {return (unsigned *)(base+offset);}
static void fake_wait_message(void *comp) {g_wait_message_calls++;*fake_u32((unsigned char *)comp,CS_MSG_MODE_OFFSET)=2;}
static void fake_final_original(void *comp) {g_final_calls++;*fake_u32((unsigned char *)comp,CS_MSG_MODE_OFFSET)=3;}
static void fake_count_original(void *self) {(void)self;g_count_calls++;}
static void game_reset(void)
{
    memset(g_fake_manager,0,sizeof g_fake_manager); memset(g_fake_comp,0,sizeof g_fake_comp);
    g_cs_mgr_slot=(unsigned char *)&g_fake_manager_slot; g_cs_clock_slot=(unsigned char *)&g_fake_clock_slot;
    g_cs_fn_lo=(unsigned char *)((ULONG_PTR)fake_clock-64); g_cs_fn_hi=(unsigned char *)((ULONG_PTR)fake_clock+64);
    g_cs_wait_message=fake_wait_message; g_cs_final_original=fake_final_original; g_cs_count_original=fake_count_original; g_cs_original=fake_original;
    g_cs_comp=NULL; g_cs_comp_at=0; memset(&g_cs_wait,0,sizeof g_cs_wait); memset(&g_cs_msg,0,sizeof g_cs_msg);
    g_cs_saved_invite_valid=0; g_cs_saved_invite=0; g_cs_count_last=0; g_cs_count_logged=0;
    memset(&g_cs,0,sizeof g_cs); g_cs.remaining=CARSEL_NO_DEADLINE; g_log[0]=0;
    g_wait_message_calls=g_final_calls=g_count_calls=g_original_calls=0; g_fake_clock_ms=1000;
    InterlockedExchange(&g_cs_enabled,1);
}
static void answer(int hold,ULONGLONG at,unsigned remaining)
{
    AcquireSRWLockExclusive(&g_cs_lock); g_cs.valid=1; g_cs.hold=(unsigned char)hold; g_cs.at=at; g_cs.remaining=remaining; ReleaseSRWLockExclusive(&g_cs_lock);
}
static unsigned manager_u32(unsigned offset) {return *fake_u32(g_fake_manager,offset);}

/* one call of carsel_decide with the feature on */
static int step(carsel_wait *w,ULONGLONG now,unsigned char hold,ULONGLONG answer_at,int *event) {return carsel_decide(w,now,1,hold,answer_at,event);}

int main(int argc,char **argv)
{
    unsigned char packet[CARSEL_REQUEST+8],epoch[16],other[16],hold=9,reply[CARSEL_REQUEST]; unsigned i,remaining=7; int event;
    for(i=0;i<32;i++)g_ticket_key[i]=(unsigned char)i;
    for(i=0;i<16;i++)epoch[i]=(unsigned char)(16+i);
    for(i=0;i<16;i++)other[i]=(unsigned char)(64+i);
    g_xuid=0x9000123456789ull;
    carsel_request(packet,g_ticket_key,g_xuid,7,epoch,1);
    if(argc>1 && !strcmp(argv[1],"--vector")) {for(i=0;i<CARSEL_REQUEST;i++)printf("%02x",packet[i]);puts("");return 0;}
    if(argc>2 && !strcmp(argv[1],"--reply")) {
        /* a reply built by server/carsel.py to the request above; argv[3] = the hold flag, argv[4] = the ms left ("none" = no deadline) the server was told to answer */
        unsigned n=read_hex(argv[2],packet,sizeof packet),want;
        assert(argc>4);
        want=!strcmp(argv[4],"none") ? CARSEL_NO_DEADLINE : (unsigned)strtoul(argv[4],NULL,10);
        assert(carsel_response(packet,n,g_ticket_key,g_xuid,7,0,epoch,&hold,&remaining) && hold==(unsigned char)atoi(argv[3]) && remaining==want);
        puts("C/Python carsel reply: PASS");
        return 0;
    }
    /* ---- wire ---- */
    assert(CARSEL_REQUEST==90 && packet[9]==90 && packet[10]==0 && packet[19]==1 && packet[52]==1 && packet[53]==0 && light_u64(packet+20)==g_xuid && light_u64(packet+28)==7);
    assert(!memcmp(packet+11,"FH2MPCS2",8) && !memcmp(packet+36,epoch,16) && carsel_get32(packet+54)==0);
    carsel_request(packet,g_ticket_key,g_xuid,8,epoch,0);
    assert(packet[52]==0 && light_u64(packet+28)==8);
    carsel_request(packet,g_ticket_key,g_xuid,8,epoch,3); assert(packet[52]==3);
    carsel_request(packet,g_ticket_key,g_xuid,8,epoch,7); assert(packet[52]==3);                           /* only two state bits */
    carsel_request(packet,g_ticket_key,g_xuid,8,epoch,0);
    assert(!carsel_response(packet,CARSEL_REQUEST,g_ticket_key,g_xuid,8,0,epoch,&hold,&remaining));       /* the reflected request: type 1 */
    memcpy(reply,packet,CARSEL_BASE); reply[19]=2; reply[52]=1; carsel_put32(reply+54,12345); resign(reply,CARSEL_REQUEST);
    assert(carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,8,0,epoch,&hold,&remaining) && hold==1 && remaining==12345);
    reply[52]=0; carsel_put32(reply+54,CARSEL_NO_DEADLINE); resign(reply,CARSEL_REQUEST);
    assert(carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,8,7,epoch,&hold,&remaining) && hold==0 && remaining==CARSEL_NO_DEADLINE);   /* newer than the last received */
    carsel_put32(reply+54,0); resign(reply,CARSEL_REQUEST);
    assert(carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,8,7,epoch,&hold,&remaining) && remaining==0);                                 /* 0 ms left is a deadline */
    assert(!carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,8,8,epoch,&hold,&remaining));        /* seq not new */
    assert(!carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,7,0,epoch,&hold,&remaining));        /* seq in the future */
    carsel_request(packet,g_ticket_key,g_xuid,50,epoch,1); memcpy(reply,packet,CARSEL_BASE); reply[19]=2; resign(reply,CARSEL_REQUEST);
    assert(carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,90,0,epoch,&hold,&remaining));        /* 40 behind: still recent */
    assert(!carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,91,0,epoch,&hold,&remaining));       /* 41 behind: stale */
    carsel_request(packet,g_ticket_key,g_xuid,8,epoch,1); memcpy(reply,packet,CARSEL_BASE); reply[19]=2; resign(reply,CARSEL_REQUEST);
    assert(!carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid+1,8,0,epoch,&hold,&remaining));      /* another player */
    assert(!carsel_response(reply,CARSEL_REQUEST,g_ticket_key,g_xuid,8,0,other,&hold,&remaining));        /* another session's epoch */
    assert(!carsel_response(reply,CARSEL_REQUEST-1,g_ticket_key,g_xuid,8,0,epoch,&hold,&remaining));      /* short */
    {
        unsigned char bad[CARSEL_REQUEST]; memcpy(bad,reply,sizeof bad);
#define REFUSED() assert(!carsel_response(bad,CARSEL_REQUEST,g_ticket_key,g_xuid,8,0,epoch,&hold,&remaining))
        bad[CARSEL_REQUEST-1]^=1; REFUSED();                                                              /* MAC */
        memcpy(bad,reply,sizeof bad); bad[40]^=1; REFUSED();
        memcpy(bad,reply,sizeof bad); bad[54]^=1; REFUSED();                                              /* the deadline is signed too */
        memcpy(bad,reply,sizeof bad); bad[52]=2; resign(bad,CARSEL_REQUEST); REFUSED();                   /* hold > 1 */
        memcpy(bad,reply,sizeof bad); bad[53]=1; resign(bad,CARSEL_REQUEST); REFUSED();                   /* reserved */
        memcpy(bad,reply,sizeof bad); bad[16]='X'; resign(bad,CARSEL_REQUEST); REFUSED();                 /* magic */
        memcpy(bad,reply,sizeof bad); bad[18]='1'; resign(bad,CARSEL_REQUEST); REFUSED();                 /* the v1 magic */
        memcpy(bad,reply,sizeof bad); bad[9]=85; resign(bad,CARSEL_REQUEST); REFUSED();                   /* length field */
    }
    /* ---- client reports (type 3): wire, queue, rate limits, the long-wait report ---- */
    {
        unsigned char rep[CARSEL_REPORT_MAX],mac[32],bad[CARSEL_REPORT_MAX]; unsigned n,k; char longtext[400]; carsel_report_slot due; ULONGLONG s,t=900000;
        n=carsel_report(rep,g_ticket_key,g_xuid,9,epoch,1,25,"waited 25 s");
        assert(n==CARSEL_BASE+11+CARSEL_MAC && rep[9]==n && rep[10]==0 && rep[19]==3 && rep[52]==1 && rep[53]==0 && carsel_get32(rep+54)==25);
        assert(light_u64(rep+20)==g_xuid && light_u64(rep+28)==9 && !memcmp(rep+11,"FH2MPCS2",8) && !memcmp(rep+36,epoch,16) && !memcmp(rep+CARSEL_BASE,"waited 25 s",11));
        hmac_sha256(g_ticket_key,32,rep,n-CARSEL_MAC,mac); assert(!memcmp(mac,rep+n-CARSEL_MAC,CARSEL_MAC));
        assert(!carsel_response(rep,n,g_ticket_key,g_xuid,9,0,epoch,&hold,&remaining));                      /* a report is not a reply */
        n=carsel_report(rep,g_ticket_key,g_xuid,9,epoch,5,0,"a\nb\x01\xc3\xa9"); assert(n==CARSEL_BASE+6+CARSEL_MAC && !memcmp(rep+CARSEL_BASE,"a?b???",6));   /* printable ASCII only */
        for(k=0;k<sizeof longtext-1;k++) longtext[k]=(char)('a'+k%26);
        longtext[sizeof longtext-1]=0;
        n=carsel_report(bad,g_ticket_key,g_xuid,9,epoch,5,0,longtext); assert(n==CARSEL_REPORT_MAX && bad[9]==(unsigned char)n && bad[10]==(unsigned char)(n>>8));   /* cut at the limit */
        n=carsel_report(bad,g_ticket_key,g_xuid,9,epoch,5,0,NULL); assert(n==CARSEL_BASE+CARSEL_MAC);
        /* the queue */
        game_reset(); memset(g_cs_reports,0,sizeof g_cs_reports); memset(g_cs_report_last,0,sizeof g_cs_report_last); g_cs_report_total=0; g_fake_now=t; s=100;
        assert(carsel_report_post(CS_RPT_CARS_LOADED,25,"first") && strstr(g_log,"report: sent to the server, code 1: first"));
        assert(!carsel_report_post(CS_RPT_CARS_LOADED,26,"again"));                                           /* the same code within 8 s */
        assert(carsel_report_post(CS_RPT_CAR_CACHE,20,"cache"));
        InterlockedExchange(&g_cs_enabled,0); assert(!carsel_report_post(CS_RPT_OTHER,0,"off")); InterlockedExchange(&g_cs_enabled,1);   /* feature off: nothing queued */
        assert(carsel_report_take(t,&s,&due) && due.code==1 && due.seq==101 && due.detail==25 && !strcmp(due.text,"first") && s==101);
        assert(carsel_report_take(t,&s,&due) && due.code==2 && due.seq==102);                                 /* the second slot, in the same instant */
        assert(!carsel_report_take(t+399,&s,&due));                                                           /* resent every 400 ms */
        assert(carsel_report_take(t+400,&s,&due) && due.code==1 && due.seq==101);                             /* the same sequence number again */
        assert(carsel_report_take(t+400,&s,&due) && due.code==2 && due.seq==102);
        assert(carsel_report_take(t+800,&s,&due) && due.seq==101 && carsel_report_take(t+800,&s,&due) && due.seq==102);
        assert(!carsel_report_take(t+1200,&s,&due));                                                          /* three sends, then gone */
        assert(carsel_report_post(CS_RPT_STALL,0,"x") == 1 && carsel_report_post(CS_RPT_STALL_END,0,"y") && carsel_report_post(CS_RPT_OTHER,0,"z") && carsel_report_post(CS_RPT_CARS_LOADED,1,"q") == 0);   /* too soon for code 1 */
        g_fake_now=t+CS_REPORT_SAME_MS; assert(carsel_report_post(CS_RPT_CARS_LOADED,1,"later"));
        assert(!carsel_report_post(CS_RPT_CAR_CACHE,1,"full"));        /* four slots are in use: the fifth waits for a free one */
        assert(!carsel_report_take(t+CS_REPORT_EXPIRE_MS+CS_REPORT_SAME_MS+1,&s,&due));                      /* old reports are never sent */
        /* the session limit */
        memset(g_cs_reports,0,sizeof g_cs_reports); memset(g_cs_report_last,0,sizeof g_cs_report_last); g_cs_report_total=0; g_fake_now=t;
        for(k=0;k<CS_REPORT_MAX_TOTAL+10;k++) {
            g_fake_now+=CS_REPORT_SAME_MS; memset(g_cs_reports,0,sizeof g_cs_reports);
            if(k<CS_REPORT_MAX_TOTAL) assert(carsel_report_post(CS_RPT_OTHER,k,"n")); else assert(!carsel_report_post(CS_RPT_OTHER,k,"n"));
        }
        /* a cars-loaded wait that reaches 20 s says so once, with what the hold looked like */
        game_reset(); memset(g_cs_reports,0,sizeof g_cs_reports); memset(g_cs_report_last,0,sizeof g_cs_report_last); g_cs_report_total=0;
        g_fake_now=t; answer(1,t,5000); carsel_update(NULL,0,0,0);
        for(k=1;k*1000<CS_REPORT_WAIT_MS;k++) {g_fake_now=t+k*1000; answer(1,g_fake_now-10,3000); carsel_update(NULL,0,0,0);}
        assert(!g_cs_report_total);
        g_fake_now=t+CS_REPORT_WAIT_MS; answer(1,g_fake_now-10,3000); carsel_update(NULL,0,0,0);
        assert(g_cs_report_total==1 && strstr(g_log,"report: sent to the server, code 1: the cars-loaded wait of this event has lasted 20 s; held by the server for") && strstr(g_log,"hold=1, group deadline in 3000 ms"));
        g_fake_now+=16; answer(1,g_fake_now-10,2900); carsel_update(NULL,0,0,0); assert(g_cs_report_total==1);   /* once per wait */
        g_fake_now+=5000; carsel_update(NULL,0,0,0); assert(g_cs_report_total==1);
        InterlockedExchange(&g_cs_enabled,0);
    }
    /* ---- the decision ---- */
    {
        carsel_wait w; ULONGLONG t=100000;
        /* no server answer ever (an old server): never held */
        memset(&w,0,sizeof w);
        for(i=0;i<50;i++) {assert(!step(&w,t+i*16,1,0,&event) && event==CS_NONE);}
        /* a server that has never answered after the wait began is not waited for beyond the pending second */
        memset(&w,0,sizeof w);
        assert(step(&w,t+2000,0,t+2000,&event) == 0);                                                  /* the answer is from this wait: "release" passes */
    }
    {
        carsel_wait w; ULONGLONG t=200000;
        /* a wait begins; the newest answer (hold=0) is from before it: held while the first answer of this wait is awaited */
        memset(&w,0,sizeof w);
        assert(step(&w,t,0,t-300,&event) && event==CS_NONE);
        assert(step(&w,t+16,0,t-300,&event) && event==CS_NONE);
        assert(step(&w,t+120,1,t+110,&event) && event==CS_STARTED && w.holding);                         /* the first answer of the wait says wait */
        for(i=1;i<=100;i++) assert(step(&w,t+120+i*16,1,t+110+(i/7)*100,&event) && event==CS_NONE);   /* answers keep coming every 100 ms */
        assert(!step(&w,t+2000,0,t+1990,&event) && event==CS_RELEASED && !w.holding);                    /* the server releases */
        assert(!step(&w,t+2016,0,t+1990,&event) && event==CS_NONE);
    }
    {
        carsel_wait w; ULONGLONG t=300000;
        /* pending ends after 1 s without an answer of this wait */
        memset(&w,0,sizeof w);
        assert(step(&w,t,0,t-200,&event));
        assert(step(&w,t+900,0,t-200,&event));
        assert(!step(&w,t+1000,0,t-200,&event) && event==CS_NONE);
        /* holding, then the answers stop: the hold ends after 2.5 s without one */
        memset(&w,0,sizeof w);
        assert(step(&w,t,0,t-100,&event));
        assert(step(&w,t+100,1,t+100,&event) && event==CS_STARTED);
        for(i=1;i<5;i++) assert(step(&w,t+100+i*500,1,t+100,&event) && event==CS_NONE);                 /* frames go on, no new answer */
        assert(step(&w,t+2600,1,t+100,&event) && event==CS_NONE);                                      /* 2.5 s old: still fresh */
        assert(!step(&w,t+2601,1,t+100,&event) && event==CS_NO_ANSWER && !w.holding);
        assert(!step(&w,t+2617,1,t+100,&event) && event==CS_NONE);
    }
    {
        carsel_wait w; ULONGLONG t=400000;
        /* 30 s limit: the game goes on and is not held again in that wait, however the server answers */
        memset(&w,0,sizeof w);
        assert(step(&w,t,1,t-50,&event));
        for(i=1;i<1800;i++) {ULONGLONG now=t+i*16; if(now-t>=CS_MAX_HOLD_MS) break; assert(step(&w,now,1,now-10,&event));}
        assert(!step(&w,t+CS_MAX_HOLD_MS,1,t+CS_MAX_HOLD_MS-10,&event) && event==CS_LIMIT && w.expired);
        assert(!step(&w,t+CS_MAX_HOLD_MS+16,1,t+CS_MAX_HOLD_MS,&event) && event==CS_NONE);
        /* a gap of more than 1.5 s is a new wait: held again */
        assert(step(&w,t+CS_MAX_HOLD_MS+3000,1,t+CS_MAX_HOLD_MS+2990,&event) && event==CS_NONE && !w.expired);        /* pending: the answer predates the wait */
        assert(step(&w,t+CS_MAX_HOLD_MS+3100,1,t+CS_MAX_HOLD_MS+3090,&event) && event==CS_STARTED);
        /* disabled: never */
        memset(&w,0,sizeof w);
        assert(!carsel_decide(&w,t,0,1,t-10,&event) && !carsel_decide(&w,t+16,0,1,t+6,&event) && event==CS_NONE);
    }
    /* ---- the group deadline rules (pure) ---- */
    {
        unsigned left=77,end=0;
        /* the time left at `now` from an answer of tick `at` */
        assert(carsel_remaining(15000,1000,1000,&left) && left==15000);
        assert(carsel_remaining(15000,1000,1700,&left) && left==14300);                                 /* counts down with the answer's age */
        assert(carsel_remaining(15000,1000,3500,&left) && left==12500);                                 /* 2500 ms old: still used */
        assert(!carsel_remaining(15000,1000,3501,&left));                                               /* stale */
        assert(carsel_remaining(500,1000,2000,&left) && left==0);                                       /* the deadline has come */
        assert(carsel_remaining(0,1000,1000,&left) && left==0);
        assert(!carsel_remaining(CARSEL_NO_DEADLINE,1000,1100,&left));                                  /* no deadline */
        assert(!carsel_remaining(15000,0,1100,&left));                                                  /* no answer yet */
        assert(!carsel_remaining(15000,2000,1000,&left));                                               /* an answer from the future */
        /* the car select timer: moved to clock+left when it differs by more than 200 ms */
        assert(carsel_timer_target(1000,21000,15000,&end) && end==16000);                               /* own timer 20 s, group 15 s */
        assert(carsel_timer_target(1000,12000,15000,&end) && end==16000);                               /* own timer shorter: lengthened to the group's */
        assert(!carsel_timer_target(1000,16200,15000,&end));                                            /* 200 ms apart: left alone */
        assert(carsel_timer_target(1000,16201,15000,&end) && end==16000);
        assert(!carsel_timer_target(1000,15800,15000,&end));
        assert(carsel_timer_target(1000,15799,15000,&end));
        assert(!carsel_timer_target(1000,0,15000,&end));                                                /* no countdown running */
        assert(!carsel_timer_target(1000,21000,0,&end));                                                /* nothing left */
        assert(!carsel_timer_target(1000,21000,60001,&end));                                            /* not believed */
        assert(carsel_timer_target(1000,200000,60000,&end) && end==61000);
        /* u32 clock wrap */
        assert(carsel_timer_target(0xFFFFFF00u,0xFFFFFF00u+20000u,15000,&end) && end==0xFFFFFF00u+15000u && end==14744u);
        assert(!carsel_timer_target(0xFFFFFF00u,0xFFFFFF00u+15100u,15000,&end));                       /* within the tolerance across the wrap */
        assert(carsel_timer_target(0xFFFFFF00u,0xFFFFFF00u+15500u,15000,&end));
        /* the component belongs to the wait */
        assert(carsel_comp_ok(g_fake_comp,10000,10500,10600));                                          /* seen just before the wait */
        assert(carsel_comp_ok(g_fake_comp,7500,10500,10600));                                           /* 3000 ms before it */
        assert(!carsel_comp_ok(g_fake_comp,7499,10500,10600));                                          /* an older wait's component */
        assert(!carsel_comp_ok(NULL,10000,10500,10600));
        assert(!carsel_comp_ok(g_fake_comp,10000,10500,10000+CS_COMP_MAX_AGE_MS+1));
        assert(!carsel_comp_ok(g_fake_comp,10000,10500,9000));
        /* the request's state bits */
        assert(carsel_state_bits(10000,0,0)==0 && carsel_state_bits(10000,9500,0)==1 && carsel_state_bits(10000,0,9700)==2 && carsel_state_bits(10000,9500,9700)==3);
        assert(carsel_state_bits(10000,9399,0)==0 && carsel_state_bits(10000,9400,0)==1);               /* 600 ms */
    }
    /* ---- the loading message: each call is made once per wait ---- */
    {
        carsel_msg m; memset(&m,0,sizeof m);
        assert(carsel_msg_step(&m,0,1,5000,1)==CS_MSG_NONE);                                            /* not holding */
        assert(carsel_msg_step(&m,1,0,0,1)==CS_MSG_NONE);                                               /* no deadline: nothing to show */
        assert(carsel_msg_step(&m,1,1,0,1)==CS_MSG_NONE);                                               /* deadline already come */
        assert(carsel_msg_step(&m,1,1,5000,1)==CS_MSG_SHOW && m.shown);
        for(i=0;i<20;i++) assert(carsel_msg_step(&m,1,1,5000-i*100,1)==CS_MSG_NONE);                    /* once */
        assert(carsel_msg_step(&m,0,1,3000,1)==CS_MSG_RESTORE && !m.shown && m.ended);                  /* the hold ended */
        assert(carsel_msg_step(&m,0,1,3000,1)==CS_MSG_NONE);
        assert(carsel_msg_step(&m,1,1,4000,1)==CS_MSG_NONE);                                            /* not shown again in this wait */
        memset(&m,0,sizeof m);
        assert(carsel_msg_step(&m,1,1,900,1)==CS_MSG_SHOW);
        assert(carsel_msg_step(&m,1,1,0,1)==CS_MSG_RESTORE);                                            /* the deadline reached while holding */
        assert(carsel_msg_step(&m,1,1,0,1)==CS_MSG_NONE);
        memset(&m,0,sizeof m);
        assert(carsel_msg_step(&m,1,1,900,1)==CS_MSG_SHOW);
        assert(carsel_msg_step(&m,1,0,0,1)==CS_MSG_RESTORE);                                            /* the deadline went away */
        memset(&m,0,sizeof m);
        assert(carsel_msg_step(&m,1,1,5000,0)==CS_MSG_MISSING && m.noted && !m.shown);                  /* no component: said once, no call */
        assert(carsel_msg_step(&m,1,1,5000,0)==CS_MSG_NONE);
        assert(carsel_msg_step(&m,1,1,5000,1)==CS_MSG_SHOW);                                            /* it turned up later */
        assert(carsel_msg_step(&m,0,0,0,0)==CS_MSG_RESTORE);
    }
    /* ---- hook A: the hold skips the game's update; the screen shows the group's time left ---- */
    {
        ULONGLONG t=500000;
        game_reset(); g_fake_now=t;
        carsel_update(NULL,0,0,0); assert(g_original_calls==1);                                         /* no answer yet: game runs */
        g_fake_now=t+3000; answer(0,t+2950,CARSEL_NO_DEADLINE);
        carsel_update(NULL,0,0,0); assert(g_original_calls==1);                                         /* a new wait (gap): pending hold */
        g_fake_now=t+3100; answer(1,t+3090,CARSEL_NO_DEADLINE);
        carsel_update(NULL,0,0,0); assert(g_original_calls==1 && strstr(g_log,"carsel: holding"));
        assert((ULONGLONG)g_cs_armed_at==t+3100);
        assert(g_wait_message_calls==0 && g_final_calls==0 && manager_u32(CS_INVITE_END_OFFSET)==0);    /* no deadline from the server: the screen is not touched */
        g_fake_now=t+3300; answer(0,t+3290,CARSEL_NO_DEADLINE);
        carsel_update(NULL,0,0,0); assert(g_original_calls==2 && strstr(g_log,"hold over after 200 ms: the server released it"));
        assert(g_wait_message_calls==0 && g_final_calls==0 && !strstr(g_log,"loading screen"));
        InterlockedExchange(&g_cs_enabled,0);
    }
    {
        ULONGLONG t=600000; unsigned saved=0x12345;
        game_reset();
        /* hook D: the component of the 'Setting Up Event' handler is remembered, the game's handler still runs */
        g_fake_now=t-100; carsel_final_message(g_fake_comp);
        assert(g_cs_comp==g_fake_comp && g_cs_comp_at==t-100 && g_final_calls==1 && manager_u32(CS_MSG_MODE_OFFSET)==0);
        g_final_calls=0; *fake_u32(g_fake_manager,CS_INVITE_END_OFFSET)=saved; g_fake_clock_ms=50000;
        /* the hold begins with a deadline of 9000 ms: Waiting for Players is shown once, the invite countdown follows the deadline */
        g_fake_now=t; answer(0,t-200,CARSEL_NO_DEADLINE);
        carsel_update(NULL,0,0,0); assert(g_original_calls==0 && g_wait_message_calls==0);              /* the first frame of a wait: pending, nothing shown */
        g_fake_now=t+100; answer(1,t+100,9000);
        carsel_update(NULL,0,0,0);
        assert(g_original_calls==0 && g_wait_message_calls==1 && g_final_calls==0 && *fake_u32(g_fake_comp,CS_MSG_MODE_OFFSET)==2);
        assert(manager_u32(CS_INVITE_END_OFFSET)==50000u+9000u-0u && strstr(g_log,"loading screen shows Waiting for Players (9000 ms left)"));
        g_fake_now=t+600; g_fake_clock_ms=50500; answer(1,t+600,8500);
        carsel_update(NULL,0,0,0);
        assert(g_wait_message_calls==1 && manager_u32(CS_INVITE_END_OFFSET)==50500u+8500u);              /* tracks the deadline, no second call */
        for(i=0;i<30;i++) {g_fake_now+=16; carsel_update(NULL,0,0,0);}                                   /* frames in between: the answer ages */
        assert(g_wait_message_calls==1 && g_final_calls==0 && g_original_calls==0);
        /* the server releases: Setting Up Event again, [+0x44] back where it was, the game's update runs */
        g_fake_now+=100; g_fake_clock_ms+=100; answer(0,g_fake_now-10,CARSEL_NO_DEADLINE);
        carsel_update(NULL,0,0,0);
        assert(g_original_calls==1 && g_final_calls==1 && g_wait_message_calls==1 && *fake_u32(g_fake_comp,CS_MSG_MODE_OFFSET)==3);
        assert(manager_u32(CS_INVITE_END_OFFSET)==saved && strstr(g_log,"hold over after") && strstr(g_log,"loading screen back to Setting Up Event"));
        carsel_update(NULL,0,0,0); assert(g_final_calls==1 && g_original_calls==2);                      /* once */
        InterlockedExchange(&g_cs_enabled,0);
    }
    {
        ULONGLONG t=700000;
        game_reset();
        /* no component seen in this wait: no call, said once; the hold itself still works */
        g_fake_now=t; answer(0,t-100,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        g_fake_now=t+100; answer(1,t+90,9000); carsel_update(NULL,0,0,0); carsel_update(NULL,0,0,0);
        assert(g_original_calls==0 && g_wait_message_calls==0 && strstr(g_log,"loading message of this wait was not seen"));
        assert(strstr(strstr(g_log,"was not seen")+10,"was not seen")==NULL);
        /* a component from an older wait does not count */
        game_reset(); g_fake_now=t; g_cs_comp=g_fake_comp; g_cs_comp_at=t-20000;
        answer(0,t-100,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        g_fake_now=t+100; answer(1,t+90,9000); carsel_update(NULL,0,0,0);
        assert(g_wait_message_calls==0 && strstr(g_log,"was not seen"));
        /* the deadline reaches 0 while holding: back to Setting Up Event, the hold goes on */
        game_reset(); g_fake_now=t-100; carsel_final_message(g_fake_comp); g_final_calls=0; g_fake_clock_ms=1000;
        g_fake_now=t; answer(0,t-100,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        g_fake_now=t+100; answer(1,t+90,500); carsel_update(NULL,0,0,0);
        assert(g_wait_message_calls==1);
        g_fake_now=t+700; answer(1,t+690,0); carsel_update(NULL,0,0,0);
        assert(g_final_calls==1 && g_original_calls==0);                                                 /* still held, message back to Setting Up Event */
        g_fake_now=t+800; answer(1,t+790,0); carsel_update(NULL,0,0,0); assert(g_final_calls==1 && g_wait_message_calls==1);
        /* the player leaves mid-hold: the next wait starts clean and [+0x44] is put back */
        game_reset(); *fake_u32(g_fake_manager,CS_INVITE_END_OFFSET)=777; g_fake_clock_ms=2000;
        g_fake_now=t-100; carsel_final_message(g_fake_comp);
        g_fake_now=t; answer(0,t-100,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        g_fake_now=t+100; answer(1,t+100,9000); carsel_update(NULL,0,0,0);
        assert(manager_u32(CS_INVITE_END_OFFSET)==2000u+9000u);
        g_fake_now=t+10000; answer(0,t+9990,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        assert(manager_u32(CS_INVITE_END_OFFSET)==777 && !g_cs_msg.shown);
        /* the fake component is not touched when the manager or the clock cannot be read */
        game_reset(); g_cs_mgr_slot=NULL; g_fake_now=t-100; carsel_final_message(g_fake_comp);
        g_fake_now=t; answer(0,t-100,CARSEL_NO_DEADLINE); carsel_update(NULL,0,0,0);
        g_fake_now=t+100; answer(1,t+90,9000); carsel_update(NULL,0,0,0);
        assert(manager_u32(CS_INVITE_END_OFFSET)==0);
        game_reset(); g_cs_fn_lo=(unsigned char *)((ULONG_PTR)fake_clock+1000); g_cs_fn_hi=g_cs_fn_lo+10;               /* a clock function outside the image */
        assert(!carsel_clock(&i));
        g_cs_fn_lo=(unsigned char *)((ULONG_PTR)fake_clock-64); g_cs_fn_hi=(unsigned char *)((ULONG_PTR)fake_clock+64); g_fake_clock_ms=4242;
        assert(carsel_clock(&i) && i==4242);
        InterlockedExchange(&g_cs_enabled,0);
    }
    /* ---- hook C: the car select timer follows the group's deadline ---- */
    {
        ULONGLONG t=800000;
        game_reset(); g_fake_now=t; g_fake_clock_ms=1000;
        *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=21000;
        /* no answer: untouched, the game's update runs */
        carsel_countdown(NULL); assert(g_count_calls==1 && manager_u32(CS_LOCAL_END_OFFSET)==21000 && g_log[0]==0);
        assert((ULONGLONG)g_cs_count_armed_at==t);
        /* an answer without a deadline: untouched */
        answer(1,t,CARSEL_NO_DEADLINE); carsel_countdown(NULL); assert(manager_u32(CS_LOCAL_END_OFFSET)==21000);
        /* a deadline 15 s away while the own timer says 20 s: moved, logged once */
        g_fake_now=t+16; answer(1,t+16,15000); carsel_countdown(NULL);
        assert(g_count_calls==3 && manager_u32(CS_LOCAL_END_OFFSET)==16000 &&
               strstr(g_log,"carsel: car select timer set to the group deadline, 15000 ms left (own timer had 20000)"));
        g_log[0]=0; g_fake_now=t+32; answer(1,t+32,14984); g_fake_clock_ms=1016; *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=21000;
        carsel_countdown(NULL); assert(manager_u32(CS_LOCAL_END_OFFSET)==16000 && g_log[0]==0);          /* written again, logged once per wait */
        /* within the tolerance: not written */
        *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=16100; g_fake_now=t+48; carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==16100);
        /* the answer goes stale: not written */
        *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=21000; g_fake_now=t+32+CS_DEADLINE_STALE_MS+1; carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==21000);
        /* the own countdown is not running (0): not started by us */
        *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=0; g_fake_now=t+3100; answer(1,t+3100,15000); carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==0);
        /* a new wait (gap) logs again; clock wrap */
        g_log[0]=0; g_fake_now=t+10000; g_fake_clock_ms=0xFFFFFF00u; answer(1,t+10000,15000); *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=0xFFFFFF00u+20000u;
        carsel_countdown(NULL); assert(manager_u32(CS_LOCAL_END_OFFSET)==0xFFFFFF00u+15000u && strstr(g_log,"own timer had 20000"));
        /* a remaining time that is no answer-time of 60 s: not believed */
        *fake_u32(g_fake_manager,CS_LOCAL_END_OFFSET)=0xFFFFFF00u+20000u; g_fake_now+=16; answer(1,g_fake_now,70000); carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==0xFFFFFF00u+20000u);
        /* switched off: untouched, the game's update runs */
        InterlockedExchange(&g_cs_enabled,0); g_fake_now+=16; answer(1,g_fake_now,15000); i=g_count_calls; carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==0xFFFFFF00u+20000u && g_count_calls==(int)i+1);
        /* the manager cannot be read: untouched */
        InterlockedExchange(&g_cs_enabled,1); g_cs_mgr_slot=NULL; g_fake_now+=16; answer(1,g_fake_now,15000); carsel_countdown(NULL);
        assert(manager_u32(CS_LOCAL_END_OFFSET)==0xFFFFFF00u+20000u);
        InterlockedExchange(&g_cs_enabled,0);
    }
    puts("carsel: PASS");
    return 0;
}
