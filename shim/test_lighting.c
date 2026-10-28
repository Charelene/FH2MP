/* Isolated memory/codec tests. No game is loaded and no network calls are made. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/sha256.inc"
static unsigned char g_ticket_key[32];
static int g_ticket_ready;
static unsigned long long g_xuid;
static int (WINAPI *g_r_wsasendto)(UINT_PTR,void *,DWORD,DWORD *,DWORD,const void *,int,void *,void *);
static int read_game_memory(const void *p,void *out,SIZE_T n) { if(!p)return 0;memcpy(out,p,n);return 1; }
static int mem_equal(const unsigned char *a,const unsigned char *b,SIZE_T n) {return !memcmp(a,b,n);}
static unsigned char *find_fh2_image(void) {return NULL;}
static int plat_hook(const char *name,void *target,void *detour,void **original) {(void)name;(void)target;(void)detour;(void)original;return 0;}
static void log_begin(void) {}
static void log_raw(const char *s) {(void)s;}
static void log_hex(unsigned long long value,int digits) {(void)value;(void)digits;}
static void log_dec(unsigned long value) {(void)value;}
#include "src/lighting_sync.inc"

static void resign(unsigned char *data,unsigned n) {hmac_sha256(g_ticket_key,32,data,n-32,data+n-32);}
static unsigned char car_memory[0x5200];
static int animation_token;
static void animation_observer(void *car)
{
    unsigned char *c=car;
    assert(c[0x51a1]==1 && c[0x51a2]==1);
    assert(c[0x51bb]==1 && !c[0x51bc] && !c[0x51bd]);
}
static unsigned char receive_observer(void *self,const void *key,const void *state,unsigned stamp)
{
    (void)self;(void)key;(void)state;(void)stamp;return 1;
}
int main(int argc,char **argv)
{
    unsigned char packet[LIGHT_LIMIT],epoch[16],copy[LIGHT_LIMIT]; unsigned i,n;
    for(i=0;i<32;i++)g_ticket_key[i]=(unsigned char)i;
    for(i=0;i<16;i++)epoch[i]=(unsigned char)(16+i);
    g_xuid=0x9000123456789ull;
    light_request(packet,g_ticket_key,g_xuid,7,epoch,5,7);
    if(argc>1 && !strcmp(argv[1],"--vector")) {for(i=0;i<LIGHT_REQUEST;i++)printf("%02x",packet[i]);puts("");return 0;}
    assert(light_flags(0,0,1,0,0,0)==1);
    assert(light_flags(1,0,1,1,0,0)==6);  /* manual off wins over automatic */
    assert(light_flags(1,1,0,0,1,0)==3);
    assert(light_flags(1,1,0,0,0,1)==5);
    assert(!light_response(packet,LIGHT_REQUEST,g_ticket_key,g_xuid,7,0,epoch)); /* reflected request */
    packet[19]=2;packet[54]=1;packet[55]=10;packet[56]=5;packet[57]=7;packet[58]=0;
    n=LIGHT_REQUEST+4;packet[9]=(unsigned char)n;resign(packet,n);
    assert(light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch));
    assert(light_response(packet,n,g_ticket_key,g_xuid,9,6,epoch)); /* delayed reply */
    assert(!light_response(packet,n,g_ticket_key,g_xuid,7,7,epoch)); /* replay */
    assert(!light_response(packet,n,g_ticket_key,g_xuid,6,0,epoch)); /* future */
    assert(!light_response(packet,n,g_ticket_key,g_xuid,28,0,epoch)); /* too old */
    assert(!light_response(packet,n-1,g_ticket_key,g_xuid,7,0,epoch));
    assert(!light_response(packet,n,g_ticket_key,g_xuid+1,0,7,epoch));
    epoch[0]^=1;assert(!light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch));epoch[0]^=1;
    memcpy(copy,packet,n);packet[57]^=1;assert(!light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch));memcpy(packet,copy,n);
    packet[55]=255;resign(packet,n);assert(!light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch));
    packet[55]=10;packet[57]=8;resign(packet,n);assert(!light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch));
    memcpy(packet,copy,n);packet[54]=2;memcpy(packet+59,packet+55,4);n+=4;packet[9]=(unsigned char)n;resign(packet,n);
    assert(!light_response(packet,n,g_ticket_key,g_xuid,7,0,epoch)); /* duplicate owner */
    /* Only matched, live native car/animation identity gets a temporary override. */
    *(void **)(car_memory+0x4d40)=&animation_token;
    car_memory[0x51a1]=0;car_memory[0x51a2]=0;car_memory[0x51bc]=1;
    {
        unsigned char object[0x368]={0},holder[0x30]={0},prediction[0xa0]={0};
        unsigned char *self=object+0x88;
        unsigned char opaque_context[16]={0};
        light_car_key key={0x40000005u,10};
        *(light_car_key *)(object+0x20)=key;
        *(void **)(self+0x2d8)=holder;*(void **)(holder+0x28)=prediction;
        *(void **)(prediction+8)=car_memory;prediction[0x1b]=1;
        *(unsigned *)(prediction+0x34)=1;
        g_light_enabled=1;g_light_receive_original=receive_observer;
        lighting_receive(self,opaque_context,NULL,2);
        assert(g_light_cars[0].car==car_memory && g_light_cars[0].owner==10 && g_light_cars[0].id==5);
        *(unsigned *)(prediction+0x34)=2;key.owner=11;
        lighting_receive(self,opaque_context,NULL,2);
        assert(g_light_cars[0].owner==10); /* native rejects an equal timestamp */
        key.word=0xc0000005u;*(light_car_key *)(object+0x20)=key;lighting_receive(self,opaque_context,NULL,3);
        assert(g_light_cars[0].owner==10); /* a shared traffic key is not a player */
    }
    g_light_enabled=1;g_light.response_at=GetTickCount64();g_light.count=1;
    g_light.records[0]=10;g_light.records[1]=5;g_light.records[2]=7;
    g_light_cars[0]=(light_remote_car){car_memory,&animation_token,5,10,GetTickCount64()};
    g_light_animation_original=animation_observer;
    lighting_animation(car_memory);
    assert(car_memory[0x51a1]==0 && car_memory[0x51a2]==0 && car_memory[0x51bc]==1 && !car_memory[0x51bb]);
    g_light_cars[0].animation=NULL;assert(!lighting_begin(car_memory).active);
    g_light_cars[0].animation=&animation_token;
    g_light_cars[0].owner=11;assert(!lighting_begin(car_memory).active);
    g_light_cars[0].owner=10;g_light_cars[0].id=6;assert(!lighting_begin(car_memory).active);
    g_light_cars[0].id=5;g_light.count=0;assert(!lighting_begin(car_memory).active);
    g_light.count=1;g_light_cars[0].seen=GetTickCount64()-LIGHT_LEASE_MS-1;
    assert(!lighting_begin(car_memory).active);
    g_light_cars[0].seen=GetTickCount64();g_light.response_at=GetTickCount64()-LIGHT_LEASE_MS-1;
    assert(!lighting_begin(car_memory).active);
    {
        struct sockaddr_in address={0};unsigned char nonce[16]={1};
        address.sin_family=AF_INET;g_light.response_at=GetTickCount64();g_light.generation=7;
        lighting_note_ticket(&address,sizeof address,nonce);
        assert(g_light.generation==8 && !g_light.response_at && !g_light.count);
        assert(g_light.address_len==sizeof address && g_light.epoch[0]==1);
    }
    puts("lighting codec, replay protection, native identity and scoped restoration: PASS");
    return 0;
}
