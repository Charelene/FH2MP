/* FH2MP car-select hold v2 (server/carsel.py). Include after sha256.inc and lighting_wire.h; no game or OS dependencies.
 * Request (client -> server) and reply (server -> client) are the same 90 bytes: header 19 (magic FH2MPCS2), type u8 @19 (1 request, 2 reply),
 * xuid u64 @20, seq u64 @28, ticket epoch 16 @36, byte @52 (request: state, bit0 = inside the cars-loaded wait of an event, bit1 = inside a car select
 * countdown wait; reply: 1 = keep waiting, 0 = release), reserved @53 = 0, u32 LE @54 (request: reserved 0; reply: milliseconds left until the group's
 * deadline, 0xFFFFFFFF = no deadline), HMAC-SHA256 under the ticket key over the first 58 bytes. State, not events: a lost datagram heals on the next. */
#ifndef FH2MP_CARSEL_WIRE_H
#define FH2MP_CARSEL_WIRE_H
#define CARSEL_BASE 58u
#define CARSEL_MAC 32u
#define CARSEL_REQUEST (CARSEL_BASE + CARSEL_MAC)
#define CARSEL_NO_DEADLINE 0xFFFFFFFFu
static const unsigned char carsel_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','C','S','2'};
static void carsel_put32(unsigned char *p, unsigned v) {p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);}
static unsigned carsel_get32(const unsigned char *p) {return (unsigned)p[0] | (unsigned)p[1]<<8 | (unsigned)p[2]<<16 | (unsigned)p[3]<<24;}
static void carsel_request(unsigned char out[CARSEL_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                           const unsigned char epoch[16], unsigned char state)
{
    unsigned i;
    for(i=0;i<19;i++) out[i]=carsel_header[i];
    out[9]=CARSEL_REQUEST; out[10]=0; out[19]=1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=state&3; out[53]=0; carsel_put32(out+54,0);
    hmac_sha256(key,32,out,CARSEL_BASE,out+CARSEL_BASE);
}
/* Client report (type 3, client -> server, never answered): why this game sits where it sits (a long wait for cars, a stall). Same header, xuid, seq and epoch
 * as a request; code u8 @52 (1 cars-loaded wait, 2 car cache wait, 3 stall trace snapshot, 4 stall trace end, 5 other, 6 race wait detail), reserved @53 = 0, u32 LE detail @54 (what
 * the code says it is, e.g. milliseconds waited), plain printable ASCII text from @58 up to CARSEL_REPORT_TEXT, then the HMAC-SHA256 over everything before it. The length
 * field @9 is the whole datagram. A server without reports ignores it (it only accepts the 90-byte request). */
#define CARSEL_REPORT_TEXT 200u
#define CARSEL_REPORT_MAX (CARSEL_BASE + CARSEL_REPORT_TEXT + CARSEL_MAC)
static unsigned carsel_report(unsigned char out[CARSEL_REPORT_MAX], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                              const unsigned char epoch[16], unsigned char code, unsigned detail, const char *text)
{
    unsigned i, n = 0, total;
    for(i=0;i<19;i++) out[i]=carsel_header[i];
    out[19]=3;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=code; out[53]=0; carsel_put32(out+54,detail);
    for(i=0;text && text[i] && n<CARSEL_REPORT_TEXT;i++) {
        unsigned char c=(unsigned char)text[i];
        out[CARSEL_BASE+n++]=(c>=0x20 && c<0x7f) ? c : '?';
    }
    total=CARSEL_BASE+n+CARSEL_MAC;
    out[9]=(unsigned char)total; out[10]=(unsigned char)(total>>8);
    hmac_sha256(key,32,out,CARSEL_BASE+n,out+CARSEL_BASE+n);
    return total;
}
/* Checks a reply (this player's xuid and epoch, a sequence number that is new and recent, the signature); returns 1 with *hold (0 or 1) and
 * *remaining (ms to the group's deadline, CARSEL_NO_DEADLINE = none), or 0 for anything else. */
static int carsel_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid, unsigned long long sent,
                           unsigned long long received, const unsigned char epoch[16], unsigned char *hold, unsigned *remaining)
{
    unsigned char mac[32]; unsigned long long seq;
    if(n!=CARSEL_REQUEST || !light_equal(data,carsel_header,9) || !light_equal(data+11,carsel_header+11,8) ||
       (unsigned)(data[9] | data[10]<<8)!=n || data[19]!=2 || light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16) ||
       data[52]>1 || data[53]) return 0;
    seq=light_u64(data+28);
    if(seq<=received || seq>sent || sent-seq>40) return 0;
    hmac_sha256(key,32,data,n-CARSEL_MAC,mac);
    if(!light_equal(mac,data+n-CARSEL_MAC,CARSEL_MAC)) return 0;
    *hold=data[52]; *remaining=carsel_get32(data+54);
    return 1;
}
#endif
