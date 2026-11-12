/* FH2MP mod state v1 (native/internal/game/modsync.go, magic FH2MPMD1): what other players' games need to show this player's add-on mods, and the
 * server's word on whether NitroSystem's NOS may burn here. Include after sha256.inc and lighting_wire.h; no game or OS dependencies.
 *
 * Request (type 1, client -> server, MODS_REQUEST bytes): header 19 (length u16 LE @9), type u8 @19, xuid u64 @20, seq u64 @28, ticket epoch 16 @36,
 * car id @52 (the car the game server has for this player), NOS flags @53 (MODS_NOS_*), reserved @54 = 0, underglow state @55 (MODS_UG bytes, below),
 * HMAC-SHA256 under the ticket key over everything before it.
 * Reply (type 2, server -> client): the request's first 52 bytes with type 2, car @52 (echo), server flags @53 (MODS_SERVER_*), record count @54, then
 * count records of MODS_RECORD bytes (owner, car, NOS flags, 0, underglow state) for other players of the same world, then the HMAC. Length field @9.
 *
 * Underglow state (FH2-Underglow 1.1.0 by qiangqiang101, MIT; what its `status` command says about the car being driven):
 *   +0 flags (bit0 lit), +1 parts lit (bit0 front, bit1 right, bit2 rear, bit3 left: the kit's parts that are switched on), +2 LEDs down each side,
 *   +3 across the front, +4 across the back (2*side+front+rear <= MODS_UG_LIGHTS), +5..+8 effect of the front, right, rear and left (< MODS_UG_EFFECTS,
 *   Underglow's numbers), +9..+11 primary color RGB, +12..+14 secondary, +15 brightness (1/64), +16 effect speed (cycles a second, 1/50),
 *   +17 half width of the light rectangle (u16 mm), +19 its front edge and +21 its rear edge along the car (s16 mm, front >= rear), +23 side trim (cm),
 *   +24 height above the car's origin (s8 cm), +25..+27 reserved 0. A state that is not lit is all zero. */
#ifndef FH2MP_MODS_WIRE_H
#define FH2MP_MODS_WIRE_H
#define MODS_UG 28u
#define MODS_HEAD 55u
#define MODS_BASE (MODS_HEAD + MODS_UG)
#define MODS_MAC 32u
#define MODS_REQUEST (MODS_BASE + MODS_MAC)
#define MODS_RECORD (4u + MODS_UG)
#define MODS_MAX 12u
#define MODS_LIMIT (MODS_HEAD + MODS_RECORD * MODS_MAX + MODS_MAC)
#define MODS_SEQ_WINDOW 40u
#define MODS_NOS_BURNING 0x01u      /* the player's NOS burns now (NitroSystem's is_active) */
#define MODS_NOS_ALLOWED 0x02u      /* the shim lets NOS burn now (online Free Roam and the server agrees) */
#define MODS_NOS_SWITCH 0x04u       /* NitroSystem with its off switch (2.0.0 or newer) is loaded */
#define MODS_NOS_NOSWITCH 0x08u     /* a NitroSystem without the switch is loaded: its NOS cannot be turned off */
#define MODS_NOS_ALL 0x0fu
#define MODS_SERVER_NOS_OK 0x01u    /* this player is in a Free Roam world and not in an event: NOS may burn */
#define MODS_UG_LIGHTS 64u
#define MODS_UG_EFFECTS 61u
#define MODS_UG_WIDTH_MAX 3000u
#define MODS_UG_LENGTH_MAX 6000
static const unsigned char mods_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','M','D','1'};

static int mods_s16(const unsigned char *p) { int v = p[0] | p[1] << 8; return v >= 0x8000 ? v - 0x10000 : v; }

/* 1 when an underglow state follows the rules above */
static int mods_ug_valid(const unsigned char *ug)
{
    unsigned i, width = (unsigned)(ug[17] | ug[18] << 8);
    int front = mods_s16(ug + 19), rear = mods_s16(ug + 21);
    if (ug[0] > 1 || ug[1] > 15 || 2u * ug[2] + ug[3] + ug[4] > MODS_UG_LIGHTS || ug[25] || ug[26] || ug[27]) return 0;
    for (i = 5; i < 9; i++) if (ug[i] >= MODS_UG_EFFECTS) return 0;
    if (width > MODS_UG_WIDTH_MAX || front > MODS_UG_LENGTH_MAX || front < -MODS_UG_LENGTH_MAX || rear > MODS_UG_LENGTH_MAX ||
        rear < -MODS_UG_LENGTH_MAX || front < rear) return 0;
    return 1;
}

static void mods_request(unsigned char out[MODS_REQUEST], const unsigned char key[32], unsigned long long xuid, unsigned long long seq,
                         const unsigned char epoch[16], unsigned char car, unsigned char nos, const unsigned char ug[MODS_UG])
{
    unsigned i;
    for (i = 0; i < 19; i++) out[i] = mods_header[i];
    out[9] = (unsigned char)MODS_REQUEST; out[10] = (unsigned char)(MODS_REQUEST >> 8); out[19] = 1;
    light_put64(out + 20, xuid); light_put64(out + 28, seq);
    for (i = 0; i < 16; i++) out[36 + i] = epoch[i];
    out[52] = car; out[53] = (unsigned char)(nos & MODS_NOS_ALL); out[54] = 0;
    for (i = 0; i < MODS_UG; i++) out[MODS_HEAD + i] = ug[i];
    hmac_sha256(key, 32, out, MODS_BASE, out + MODS_BASE);
}

typedef struct { unsigned char owner, car, nos, ug[MODS_UG]; } mods_record;

/* Checks a reply (this player's xuid and epoch, a sequence number that is new and recent, the signature, every record well formed and its owner
 * once) and unpacks it: 1 with the server flags, the records and their count, 0 for anything else. */
static int mods_response(const unsigned char *data, unsigned n, const unsigned char key[32], unsigned long long xuid, unsigned long long sent,
                         unsigned long long received, const unsigned char epoch[16], unsigned char *server, mods_record out[MODS_MAX], unsigned *count)
{
    unsigned i, j, c; unsigned char mac[32]; unsigned long long seq;
    if (n < MODS_HEAD + MODS_MAC || n > MODS_LIMIT || !light_equal(data, mods_header, 9) || !light_equal(data + 11, mods_header + 11, 8) ||
        (unsigned)(data[9] | data[10] << 8) != n || data[19] != 2 || light_u64(data + 20) != xuid || !light_equal(data + 36, epoch, 16) ||
        data[52] > 127 || data[53] > MODS_SERVER_NOS_OK) return 0;
    seq = light_u64(data + 28); c = data[54];
    if (seq <= received || seq > sent || sent - seq > MODS_SEQ_WINDOW || c > MODS_MAX || n != MODS_HEAD + MODS_RECORD * c + MODS_MAC) return 0;
    hmac_sha256(key, 32, data, n - MODS_MAC, mac);
    if (!light_equal(mac, data + n - MODS_MAC, MODS_MAC)) return 0;
    for (i = 0; i < c; i++) {
        const unsigned char *r = data + MODS_HEAD + MODS_RECORD * i;
        if (!r[0] || r[0] == 255 || r[1] > 127 || r[2] > MODS_NOS_ALL || r[3] || !mods_ug_valid(r + 4)) return 0;
        for (j = 0; j < i; j++) if (out[j].owner == r[0]) return 0;
        out[i].owner = r[0]; out[i].car = r[1]; out[i].nos = r[2];
        for (j = 0; j < MODS_UG; j++) out[i].ug[j] = r[4 + j];
    }
    *server = data[53];
    *count = c;
    return 1;
}
#endif
