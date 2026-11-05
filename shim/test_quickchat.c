/* Isolated tests of src/quickchat_wire.h and src/quickchat_sync.inc: no game is loaded, no network calls are made, the platform and
 * QuickChat are fakes. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fh2mod.h"
#include "src/sha256.inc"
static unsigned char g_ticket_key[32];
static int g_ticket_ready;
static unsigned long long g_xuid;
static const FH2ModApi *g_api;
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
#include "src/quickchat_sync.inc"

static void resign(unsigned char *data,unsigned n) {hmac_sha256(g_ticket_key,32,data,n-32,data+n-32);}
static int hex_value(int c) {return c>='a' ? c-'a'+10 : c>='A' ? c-'A'+10 : c-'0';}
static unsigned read_hex(const char *path,unsigned char *out,unsigned max)
{
    FILE *f=fopen(path,"r"); char line[4096]; unsigned n=0,i;
    assert(f && fgets(line,sizeof line,f)); fclose(f);
    for(i=0;line[i] && line[i+1] && line[i]!='\n' && n<max;i+=2) out[n++]=(unsigned char)(hex_value(line[i])*16+hex_value(line[i+1]));
    return n;
}
static void put32(unsigned char *p,unsigned v) {unsigned i; for(i=0;i<4;i++) p[i]=(unsigned char)(v>>(8*i));}

static const unsigned char kEpoch[16]={16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
#define XUID 0x9000123456789ULL

/* A reply as the server builds it: `count` records, each owner 10+i, phrase 5, number 4+i, name "Dusk Carton". */
static unsigned build_reply(unsigned char *out,unsigned long long seq,unsigned count,unsigned taken)
{
    unsigned n=QC_BASE+QC_RECORD*count+QC_MAC,i;
    memset(out,0,n);
    memcpy(out,qc_header,19); out[9]=(unsigned char)n; out[10]=(unsigned char)(n>>8); out[19]=2;
    light_put64(out+20,XUID); light_put64(out+28,seq); memcpy(out+36,kEpoch,16);
    put32(out+52,taken); out[56]=2; out[57]=(unsigned char)count;
    for(i=0;i<count;i++) {
        unsigned char *r=out+QC_BASE+QC_RECORD*i;
        r[0]=(unsigned char)(10+i); r[1]=5; r[2]=11; put32(r+4,4+i); r[8]=250; memcpy(r+12,"Dusk Carton",11);
    }
    resign(out,n);
    return n;
}

static void test_wire(void)
{
    unsigned char req[QC_REQUEST], rep[QC_LIMIT+64]; qc_record rec[QC_MAX]; unsigned count=0, taken=0, n;
    qc_request(req,g_ticket_key,XUID,7,kEpoch,3,2);
    assert(req[9]==QC_REQUEST && req[19]==1 && req[52]==3 && req[56]==2 && req[57]==0);
    n=build_reply(rep,7,1,3);
    assert(qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
    assert(count==1 && taken==3 && rec[0].owner==10 && rec[0].phrase==5 && rec[0].number==4 && rec[0].age_ms==250 &&
           !strcmp(rec[0].name,"Dusk Carton"));
    /* refused: an old or future sequence, a stale one, a wrong epoch, xuid or key, a bad length */
    assert(!qc_response(rep,n,g_ticket_key,XUID,7,7,kEpoch,rec,&count,&taken));   /* already received */
    assert(!qc_response(rep,n,g_ticket_key,XUID,6,0,kEpoch,rec,&count,&taken));   /* newer than sent */
    n=build_reply(rep,7,1,3);
    assert(!qc_response(rep,n,g_ticket_key,XUID,48,0,kEpoch,rec,&count,&taken));  /* more than 40 polls old */
    {
        unsigned char other[16]; memcpy(other,kEpoch,16); other[0]^=1;
        assert(!qc_response(rep,n,g_ticket_key,XUID,7,0,other,rec,&count,&taken));
    }
    assert(!qc_response(rep,n,g_ticket_key,XUID+1,7,0,kEpoch,rec,&count,&taken));
    rep[QC_BASE]^=1;                                                                /* tampered: the MAC no longer matches */
    assert(!qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
    assert(!qc_response(rep,n-1,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
    /* refused even when signed: no owner, a phrase past 63, number 0, a name too long or with a zero in it, the same owner twice */
    {
        static const struct { unsigned at; unsigned char v; } bad[]={{0,0},{1,64},{4,0},{2,49},{12,0}};
        unsigned i;
        for(i=0;i<sizeof bad/sizeof *bad;i++) {
            n=build_reply(rep,7,1,3);
            rep[QC_BASE+bad[i].at]=bad[i].v; if(bad[i].at==4) rep[QC_BASE+5]=rep[QC_BASE+6]=rep[QC_BASE+7]=0;
            resign(rep,n);
            assert(!qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
        }
        n=build_reply(rep,7,2,3);
        rep[QC_BASE+QC_RECORD]=10; resign(rep,n);
        assert(!qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
    }
    n=build_reply(rep,7,QC_MAX,3);
    assert(qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken) && count==QC_MAX);
    printf("ok   quick-chat wire: requests, replies and every refusal\n");
}

/* ---- the bridge, through a fake platform ---- */
static int fake_latest(uint32_t *s,uint32_t *p,uint64_t *ms) {*s=1;*p=2;*ms=0;return 1;}
static int fake_deliver(uint32_t from,const char *name,uint32_t seq,uint32_t phrase) {(void)from;(void)name;(void)seq;(void)phrase;return 1;}
static QuickChatBridge fake_bridge={sizeof(QuickChatBridge),QUICKCHAT_BRIDGE_VERSION,16,fake_latest,fake_deliver,NULL};
static const void *g_offer;
static int g_waited;
static int fake_wait_for_mods(uint32_t ms) {(void)ms; g_waited++; return 1;}
static const void *fake_get_interface(const char *name,uint32_t min) {assert(!strcmp(name,"QuickChat.Bridge") && min==1); return g_offer;}

static void test_bridge(void)
{
    static FH2ModApi api;
    api.wait_for_mods=fake_wait_for_mods; api.get_interface=fake_get_interface;
    g_api=&api;
    g_offer=NULL;
    assert(qc_find_bridge()==NULL && g_waited==1);                  /* no QuickChat: nothing, after every mod started */
    g_offer=&fake_bridge;
    assert(qc_find_bridge()==&fake_bridge);
    fake_bridge.size=8;
    assert(qc_find_bridge()==NULL);                                 /* too small to be the struct this shim knows */
    fake_bridge.size=sizeof(QuickChatBridge);
    g_api=NULL;
    assert(qc_find_bridge()==NULL);
    printf("ok   quick-chat bridge: found after every mod started, refused when absent or too small\n");
}

int main(int argc,char **argv)
{
    unsigned i;
    for(i=0;i<32;i++) g_ticket_key[i]=(unsigned char)i;
    g_xuid=XUID;
    if(argc>1 && !strcmp(argv[1],"--vector")) {          /* the request server/quickchat.py must accept */
        unsigned char req[QC_REQUEST];
        qc_request(req,g_ticket_key,XUID,7,kEpoch,3,2);
        for(i=0;i<QC_REQUEST;i++) printf("%02x",req[i]);
        printf("\n");
        return 0;
    }
    if(argc>2 && !strcmp(argv[1],"--reply")) {           /* the reply server/quickchat.py built for it */
        unsigned char rep[QC_LIMIT]; qc_record rec[QC_MAX]; unsigned count=0, taken=0, n=read_hex(argv[2],rep,sizeof rep);
        assert(qc_response(rep,n,g_ticket_key,XUID,7,0,kEpoch,rec,&count,&taken));
        assert(taken==3 && count==1 && rec[0].owner==10 && rec[0].phrase==5 && rec[0].number==4 && !strcmp(rec[0].name,"Dusk Carton"));
        printf("ok   the server's reply is what the shim accepts\n");
        return 0;
    }
    test_wire();
    test_bridge();
    printf("ALL PASSED\n");
    return 0;
}
