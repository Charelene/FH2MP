/* FH2MP horn v1 (native/internal/game/side.go). Include after sha256.inc and lighting_wire.h; no game or OS dependencies. */
#ifndef FH2MP_HONK_WIRE_H
#define FH2MP_HONK_WIRE_H
#define HONK_BASE 57u
#define HONK_MAC 32u
#define HONK_REQUEST (HONK_BASE + HONK_MAC)
#define HONK_RECORD 20u
#define HONK_MAX 12u
#define HONK_LIMIT (HONK_REQUEST + HONK_RECORD * HONK_MAX)
#define HONK_HORNS 31u
#define HONK_OFF 255u
#define HONK_CAP_MS 10000u
typedef struct { unsigned char owner, car, horn, flags; unsigned short press, age; float pos[3]; } honk_record;
static const unsigned char honk_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','H','N','1'};
static void honk_request(unsigned char out[HONK_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                         const unsigned char epoch[16], unsigned char car, unsigned char horn, unsigned short press)
{
    unsigned i;
    for(i=0;i<19;i++) out[i]=honk_header[i];
    out[9]=HONK_REQUEST; out[10]=0; out[19]=1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=car; out[53]=horn; out[54]=0; out[55]=(unsigned char)press; out[56]=(unsigned char)(press>>8);
    hmac_sha256(key,32,out,HONK_BASE,out+HONK_BASE);
}
static int honk_finite(float f) { return f==f && f-f==0.0f; }
/* Checks a reply (the same rules as light_response: this player's xuid and epoch, a sequence number that is new and recent, the signature)
 * and unpacks its records into `out`; returns 1 and the count, or 0 for anything else. */
static int honk_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid,
                         unsigned long long sent, unsigned long long received, const unsigned char epoch[16],
                         honk_record out[HONK_MAX], unsigned *count)
{
    unsigned i,j,c; unsigned char mac[32]; unsigned long long seq;
    if(n<HONK_REQUEST || n>HONK_LIMIT || !light_equal(data,honk_header,9) || !light_equal(data+11,honk_header+11,8) ||
       (unsigned)(data[9] | data[10]<<8)!=n || data[19]!=2 || light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16) ||
       data[52]>127 || (data[53]!=HONK_OFF && data[53]>=HONK_HORNS)) return 0;
    seq=light_u64(data+28); c=data[54];
    if(seq<=received || seq>sent || sent-seq>40 || c>HONK_MAX || n!=HONK_REQUEST+HONK_RECORD*c) return 0;
    hmac_sha256(key,32,data,n-HONK_MAC,mac);
    if(!light_equal(mac,data+n-HONK_MAC,HONK_MAC)) return 0;
    for(i=0;i<c;i++) {
        const unsigned char *r=data+HONK_BASE+HONK_RECORD*i; honk_record *o=out+i; unsigned k;
        o->owner=r[0]; o->car=r[1]; o->horn=r[2]; o->flags=r[3];
        o->press=(unsigned short)(r[4] | r[5]<<8); o->age=(unsigned short)(r[6] | r[7]<<8);
        for(k=0;k<3;k++) {
            unsigned char *dst=(unsigned char *)&o->pos[k]; unsigned b;
            for(b=0;b<4;b++) dst[b]=r[8+4*k+b];
            if(!honk_finite(o->pos[k])) return 0;
        }
        if(!o->owner || o->owner==255 || o->car>127 || o->horn>=HONK_HORNS || o->flags>1) return 0;
        for(j=0;j<i;j++) if(out[j].owner==o->owner) return 0;
    }
    *count=c;
    return 1;
}
#endif
