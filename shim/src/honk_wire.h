/* FH2MP horn v1 and v2 (native/internal/game/side.go). Include after sha256.inc and lighting_wire.h; no game or OS dependencies.
 * v1 (request type 1, reply type 2) numbers a horn by its place in HonkHonk 0.3.4/0.4.0's 31 sound bank, v2 (request type 3, reply type 4, the same bytes
 * otherwise) by its place in HonkHonk 1.0.0's 47 sound bank (honk_names), up to 63 for sounds a later build appends. The shim works in v2 numbers ("horn
 * ids"); v1 numbers are converted here, at the wire, and so are they on the server: a released v1 shim and a v2 shim hear each other's horns as the same sound. */
#ifndef FH2MP_HONK_WIRE_H
#define FH2MP_HONK_WIRE_H
#define HONK_BASE 57u
#define HONK_MAC 32u
#define HONK_REQUEST (HONK_BASE + HONK_MAC)
#define HONK_RECORD 20u
#define HONK_MAX 12u
#define HONK_LIMIT (HONK_REQUEST + HONK_RECORD * HONK_MAX)
#define HONK_V1_HORNS 31u
#define HONK_IDS 64u
#define HONK_KNOWN 47u
#define HONK_OFF 255u
#define HONK_CAP_MS 60000u            /* HonkHonk 1.0.0's sirens run up to 14 s a loop and are held; the age field (u16 ms) ends at 65.5 s */
typedef struct { unsigned char owner, car, horn, flags; unsigned short press, age; float pos[3]; } honk_record;
static const unsigned char honk_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','H','N','1'};
/* The horn ids: the sample names of HonkHonk 1.0.0's HZ2_SFX_HonkHonk.bank in bank order (its FSB5 name table; README and Sound_List.txt agree). */
static const char *const honk_names[HONK_KNOWN] = {
    "Car","Truck","Classic","Train","Warthog","Bicycle","Reversing","Police","Police2","Police3","Martinshorn","Firetruck","Boat","Doom","Doorbell","Honk",
    "Horse","Quack","Rooster","Starter","Teams","WindowsXP","Wololo","Augh","Bad to the bone","Bruh","DejaVu","Discord","DJ","Goofy","Lego","MGS",
    "Social credit","Spiderman","Wow","Cat","Meow","Siren","SleighBells","Wheelspin","Win10","Xbox","Nyooom","La Cucaracha","Cyberpunk","Rizz","Stinky"};
/* 1.0.0 put Police2, Police3, Martinshorn and Firetruck (8-11) after Police (7): 0.4.0's Boat (8) to Wow (30) are four places up. -1: no such sound. */
static int honk_v1_to_id(unsigned v1) { return v1>=HONK_V1_HORNS ? -1 : v1<8 ? (int)v1 : (int)v1+4; }
static int honk_id_to_v1(unsigned id) { return id<8 ? (int)id : id>=12 && id<HONK_V1_HORNS+4 ? (int)id-4 : -1; }
/* A request: v2 carries the horn id, v1 its v1 number (HONK_OFF for a sound the 31 sound bank does not have). */
static void honk_request(unsigned char out[HONK_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                         const unsigned char epoch[16], unsigned char car, unsigned char horn, unsigned short press, int v2)
{
    unsigned i;
    if(!v2 && horn!=HONK_OFF) {int v1=honk_id_to_v1(horn); horn=v1<0?(unsigned char)HONK_OFF:(unsigned char)v1;}
    for(i=0;i<19;i++) out[i]=honk_header[i];
    out[9]=HONK_REQUEST; out[10]=0; out[19]=v2?3:1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=car; out[53]=horn; out[54]=0; out[55]=(unsigned char)press; out[56]=(unsigned char)(press>>8);
    hmac_sha256(key,32,out,HONK_BASE,out+HONK_BASE);
}
static int honk_finite(float f) { return f==f && f-f==0.0f; }
/* Checks a reply (the same rules as light_response: this player's xuid and epoch, a sequence number that is new and recent, the signature)
 * and unpacks its records into `out`, horns as ids; returns 1 for a v1 reply, 2 for a v2 reply and the count, or 0 for anything else. */
static int honk_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid,
                         unsigned long long sent, unsigned long long received, const unsigned char epoch[16],
                         honk_record out[HONK_MAX], unsigned *count)
{
    unsigned i,j,c,horns; unsigned char mac[32]; unsigned long long seq; int v2;
    if(n<HONK_REQUEST || n>HONK_LIMIT || !light_equal(data,honk_header,9) || !light_equal(data+11,honk_header+11,8) ||
       (unsigned)(data[9] | data[10]<<8)!=n || (data[19]!=2 && data[19]!=4)) return 0;
    v2=data[19]==4; horns=v2?HONK_IDS:HONK_V1_HORNS;
    if(light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16) || data[52]>127 || (data[53]!=HONK_OFF && data[53]>=horns)) return 0;
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
        if(!o->owner || o->owner==255 || o->car>127 || o->horn>=horns || o->flags>1) return 0;
        if(!v2) o->horn=(unsigned char)honk_v1_to_id(o->horn);
        for(j=0;j<i;j++) if(out[j].owner==o->owner) return 0;
    }
    *count=c;
    return v2?2:1;
}
#endif
