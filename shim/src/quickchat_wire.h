/* FH2MP quick chat v1 (server/quickchat.py). Include after sha256.inc and lighting_wire.h; no game or OS dependencies. */
#ifndef FH2MP_QUICKCHAT_WIRE_H
#define FH2MP_QUICKCHAT_WIRE_H
#define QC_BASE 58u
#define QC_MAC 32u
#define QC_REQUEST (QC_BASE + QC_MAC)
#define QC_NAME 48u
#define QC_RECORD (12u + QC_NAME)
#define QC_MAX 12u
#define QC_LIMIT (QC_REQUEST + QC_RECORD * QC_MAX)
#define QC_PHRASES 64u
typedef struct { unsigned char owner, phrase; unsigned number, age_ms; char name[QC_NAME + 1]; } qc_record;
static const unsigned char qc_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','Q','C','1'};
/* The newest message this player sent (number 0: none yet), signed with the session's ticket key. */
static void qc_request(unsigned char out[QC_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                       const unsigned char epoch[16], unsigned number, unsigned char phrase)
{
    unsigned i;
    for(i=0;i<19;i++) out[i]=qc_header[i];
    out[9]=QC_REQUEST; out[10]=0; out[19]=1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    for(i=0;i<4;i++) out[52+i]=(unsigned char)(number>>(8*i));
    out[56]=phrase; out[57]=0;
    hmac_sha256(key,32,out,QC_BASE,out+QC_BASE);
}
/* Checks a reply (the same rules as honk_response: this player's xuid and epoch, a sequence number that is new and recent, the signature)
 * and unpacks its records into `out`, names as zero-ended UTF-8; *taken is the message number of ours the server holds. Returns 1 and the
 * count, or 0 for anything else. */
static int qc_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid,
                       unsigned long long sent, unsigned long long received, const unsigned char epoch[16],
                       qc_record out[QC_MAX], unsigned *count, unsigned *taken)
{
    unsigned i,j,c,k; unsigned char mac[32]; unsigned long long seq;
    if(n<QC_REQUEST || n>QC_LIMIT || !light_equal(data,qc_header,9) || !light_equal(data+11,qc_header+11,8) ||
       (unsigned)(data[9] | data[10]<<8)!=n || data[19]!=2 || light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16)) return 0;
    seq=light_u64(data+28); c=data[57];
    if(seq<=received || seq>sent || sent-seq>40 || c>QC_MAX || n!=QC_REQUEST+QC_RECORD*c) return 0;
    hmac_sha256(key,32,data,n-QC_MAC,mac);
    if(!light_equal(mac,data+n-QC_MAC,QC_MAC)) return 0;
    for(i=0;i<c;i++) {
        const unsigned char *r=data+QC_BASE+QC_RECORD*i; qc_record *o=out+i;
        o->owner=r[0]; o->phrase=r[1];
        o->number=(unsigned)r[4] | (unsigned)r[5]<<8 | (unsigned)r[6]<<16 | (unsigned)r[7]<<24;
        o->age_ms=(unsigned)(r[8] | r[9]<<8);
        if(!o->owner || o->owner==255 || o->phrase>=QC_PHRASES || !o->number || r[2]>QC_NAME || r[3] || r[10] || r[11]) return 0;
        for(k=0;k<r[2];k++) {
            if(!r[12+k]) return 0;            /* a zero inside the name */
            o->name[k]=(char)r[12+k];
        }
        o->name[r[2]]=0;
        for(j=0;j<i;j++) if(out[j].owner==o->owner) return 0;
    }
    *taken=(unsigned)data[52] | (unsigned)data[53]<<8 | (unsigned)data[54]<<16 | (unsigned)data[55]<<24;
    *count=c;
    return 1;
}
#endif
