/* Isolated test of src/eliminator_wire.h (the FH2MPEL1 side channel): the request the C code builds and the reply it accepts, against the frozen
 * vectors the server's own test also reads (native/internal/game/eliminator_test.go, TestEliminatorWireVectors). No game, no network. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/sha256.inc"
#include "src/lighting_wire.h"
#include "src/eliminator_wire.h"

static unsigned char g_key[32];
static const unsigned char g_epoch[16] = {0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf};
#define XUID 0x0102030405060708ull
#define SEQ 5ull

static int hex_value(int c) {return c>='a' ? c-'a'+10 : c>='A' ? c-'A'+10 : c-'0';}
static unsigned read_hex(const char *path,unsigned char *out,unsigned max)
{
    FILE *f=fopen(path,"r"); char line[2048]; unsigned n=0,i;
    assert(f && fgets(line,sizeof line,f)); fclose(f);
    for(i=0;line[i] && line[i+1] && line[i]!='\n' && line[i]!='\r' && n<max;i+=2) out[n++]=(unsigned char)(hex_value(line[i])*16+hex_value(line[i+1]));
    return n;
}
static void resign(unsigned char *data,unsigned n) {hmac_sha256(g_key,32,data,n-EL_MAC,data+n-EL_MAC);}

/* el_response with this test's key, xuid and epoch; sent = received + 1 = SEQ unless given. */
static int accepts(const unsigned char *data,unsigned n,unsigned long long sent,unsigned long long received,
                   unsigned char *v,el_drop *drops,unsigned *count)
{
    return el_response(data,n,g_key,XUID,sent,received,g_epoch,v,v+1,v+2,v+3,v+4,v+5,drops,count);
}

int main(int argc,char **argv)
{
    unsigned char request[EL_REQUEST],reply[EL_LIMIT],bad[EL_LIMIT],v[6]; el_drop drops[EL_DROP_MAX]; unsigned i,n,count=0;
    for(i=0;i<32;i++) g_key[i]=(unsigned char)i;
    el_request(request,g_key,XUID,SEQ,g_epoch,3,EL_CMD_DROP_CLAIM,7,0);
    if(argc==2 && !strcmp(argv[1],"--vector")) {
        for(i=0;i<EL_REQUEST;i++) printf("%02x",request[i]);
        printf("\n");
        return 0;
    }
    assert(argc==3 && !strcmp(argv[1],"--reply"));
    n=read_hex(argv[2],reply,sizeof reply);

    /* the server's reply: active, 2 players, area 2, shrinking + joined, tier 3 (A), challenged by owner 2, two drops */
    assert(accepts(reply,n,SEQ,SEQ-1,v,drops,&count));
    assert(v[0]==EL_PHASE_ACTIVE && v[1]==2 && v[2]==2 && v[3]==(EL_FLAG_AREA_SHRINKING|EL_FLAG_JOINED) && v[4]==3 && v[5]==2);
    assert(count==2 && drops[0].id==1 && drops[0].cls==4 && drops[0].x==100.5f && drops[0].z==-200.25f);
    assert(drops[1].id==2 && drops[1].cls==0 && drops[1].x==-1.0f && drops[1].z==2.0f);

    /* refused: the request's length field in a longer reply (the fault this test is for), an answer already taken, one to a request not sent yet,
     * one cut short, a wrong signature, a request sent back */
    memcpy(bad,reply,n); bad[9]=EL_REQUEST; resign(bad,n);
    assert(!accepts(bad,n,SEQ,SEQ-1,v,drops,&count));
    assert(!accepts(reply,n,SEQ,SEQ,v,drops,&count));
    assert(!accepts(reply,n,SEQ-1,SEQ-2,v,drops,&count));
    assert(!accepts(reply,n-1,SEQ,SEQ-1,v,drops,&count));
    memcpy(bad,reply,n); bad[n-1]^=1;
    assert(!accepts(bad,n,SEQ,SEQ-1,v,drops,&count));
    assert(!accepts(request,EL_REQUEST,SEQ,SEQ-1,v,drops,&count));
    printf("eliminator wire: ok\n");
    return 0;
}
