/* FH2MP event options v1 (native/internal/game/eventopts.go). Include after sha256.inc and lighting_wire.h; no game or OS dependencies.
 * Request (client -> server) and reply (server -> client) are the same 90 bytes: header 19 (magic FH2MPEO1), type u8 @19 (1 request, 2 reply),
 * xuid u64 @20, seq u64 @28, ticket epoch 16 @36, byte @52 (request: what this game's Modded cars row was set to when its player last started an
 * event, 0 Not allowed, 1 Allowed, for a few seconds after that start, 255 the rest of the time; reply: bit 0 = the session's pending or running
 * event takes modded cars), reserved @53 = 0, reserved u32 @54 = 0, HMAC-SHA256 under the ticket key over the first 58 bytes. State, not events:
 * a lost datagram heals on the next. */
#ifndef FH2MP_EVENTOPTS_WIRE_H
#define FH2MP_EVENTOPTS_WIRE_H
#define EO_BASE 58u
#define EO_MAC 32u
#define EO_REQUEST (EO_BASE + EO_MAC)
#define EO_NONE 255u
#define EO_MODDED 1u
static const unsigned char eo_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','E','O','1'};
static void eo_request(unsigned char out[EO_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                       const unsigned char epoch[16], unsigned char choice)
{
    unsigned i;
    for(i=0;i<19;i++) out[i]=eo_header[i];
    out[9]=EO_REQUEST; out[10]=0; out[19]=1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=choice<=1 ? choice : EO_NONE;
    for(i=53;i<EO_BASE;i++) out[i]=0;
    hmac_sha256(key,32,out,EO_BASE,out+EO_BASE);
}
/* Checks a reply (this player's xuid and epoch, a sequence number that is new and recent, the signature); returns 1 with *flags (EO_MODDED or
 * 0), or 0 for anything else. */
static int eo_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid, unsigned long long sent,
                       unsigned long long received, const unsigned char epoch[16], unsigned char *flags)
{
    unsigned char mac[32]; unsigned long long seq;
    if(n!=EO_REQUEST || !light_equal(data,eo_header,9) || !light_equal(data+11,eo_header+11,8) ||
       (unsigned)(data[9] | data[10]<<8)!=n || data[19]!=2 || light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16) ||
       data[52]>EO_MODDED || data[53] || data[54] || data[55] || data[56] || data[57]) return 0;
    seq=light_u64(data+28);
    if(seq<=received || seq>sent || sent-seq>40) return 0;
    hmac_sha256(key,32,data,n-EO_MAC,mac);
    if(!light_equal(mac,data+n-EO_MAC,EO_MAC)) return 0;
    *flags=data[52];
    return 1;
}
#endif
