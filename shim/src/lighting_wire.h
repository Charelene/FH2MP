/* FH2MP lighting v1. Include after sha256.inc; no game or OS dependencies. */
#ifndef FH2MP_LIGHTING_WIRE_H
#define FH2MP_LIGHTING_WIRE_H
#define LIGHT_BASE 55u
#define LIGHT_MAC 32u
#define LIGHT_REQUEST (LIGHT_BASE + LIGHT_MAC)
#define LIGHT_MAX 12u
#define LIGHT_LIMIT (LIGHT_REQUEST + 4u * LIGHT_MAX)
#define LIGHT_LEASE_MS 2000u
static const unsigned char light_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','L','T','1'};
static unsigned long long light_u64(const unsigned char *p)
{
    unsigned long long v = 0; unsigned i;
    for (i=0;i<8;i++) v |= (unsigned long long)p[i] << (8*i);
    return v;
}
static void light_put64(unsigned char *p, unsigned long long v)
{
    unsigned i; for(i=0;i<8;i++,v>>=8) p[i]=(unsigned char)v;
}
static int light_equal(const unsigned char *a, const unsigned char *b, unsigned n)
{
    unsigned i, diff=0; for(i=0;i<n;i++) diff |= a[i]^b[i]; return !diff;
}
static void light_request(unsigned char out[LIGHT_REQUEST], const unsigned char key[32],
                          unsigned long long xuid, unsigned long long seq, const unsigned char epoch[16],
                          unsigned char car, unsigned char flags)
{
    unsigned i;
    for(i=0;i<19;i++) out[i]=light_header[i];
    out[9]=LIGHT_REQUEST; out[10]=0; out[19]=1;
    light_put64(out+20,xuid); light_put64(out+28,seq);
    for(i=0;i<16;i++) out[36+i]=epoch[i];
    out[52]=car; out[53]=flags; out[54]=0;
    hmac_sha256(key,32,out,LIGHT_BASE,out+LIGHT_BASE);
}
static int light_response(const unsigned char *data, unsigned n, const unsigned char key[32],
                           unsigned long long xuid, unsigned long long sent, unsigned long long received,
                           const unsigned char epoch[16])
{
    unsigned i,j,count; unsigned char mac[32]; unsigned long long seq;
    if(n<LIGHT_REQUEST || n>LIGHT_LIMIT || !light_equal(data,light_header,9) ||
       !light_equal(data+11,light_header+11,8) || (unsigned)(data[9] | data[10]<<8)!=n ||
       data[19]!=2 || light_u64(data+20)!=xuid || !light_equal(data+36,epoch,16) ||
       data[52]>127 || data[53]>7) return 0;
    seq=light_u64(data+28); count=data[54];
    if(seq<=received || seq>sent || sent-seq>20 || count>LIGHT_MAX || n!=LIGHT_REQUEST+4*count) return 0;
    hmac_sha256(key,32,data,n-LIGHT_MAC,mac);
    if(!light_equal(mac,data+n-LIGHT_MAC,LIGHT_MAC)) return 0;
    for(i=0;i<count;i++) {
        const unsigned char *r=data+LIGHT_BASE+4*i;
        if(!r[0] || r[0]==255 || r[1]>127 || r[2]>7 || r[3]) return 0;
        for(j=0;j<i;j++) if(data[LIGHT_BASE+4*j]==r[0]) return 0;
    }
    return 1;
}
static unsigned char light_flags(unsigned char manual, unsigned char forced, unsigned char automatic,
                                 unsigned char hazard, unsigned char left, unsigned char right)
{
    unsigned mode=hazard?3:left?1:right?2:0;
    return (unsigned char)((manual?forced!=0:automatic!=0) | mode<<1);
}
#endif
