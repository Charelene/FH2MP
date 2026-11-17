/* FH2MP Eliminator side channel v1 (DRAFT — groundwork).
 *
 * The Eliminator is a battle-royale mode run by the server inside an Online Free Roam world
 * (see docs/eliminator/design.md). Its FH2MP-specific state — join/leave, challenge/accept,
 * car-drop claims, eliminations, the shrink clock — rides a side channel on the game port,
 * exactly like lighting (FH2MPLT1) and honk (FH2MPHN1): the same 11-byte side header, a magic,
 * the player's xuid + sequence + ticket epoch + car, and an HMAC-SHA256 over the payload.
 *
 * The transport and the ticket (server address, HMAC key, xuid, epoch) come from the lighting
 * feature (lighting_sync.inc): this header only defines the Eliminator framing on top of them.
 *
 * Framing (same discipline as lighting/honk): a fixed 58-byte base, then (reply only) 4 bytes about
 * the asking player and the drop records, then the 32-byte MAC over everything before it. Bytes 9-10
 * are the whole datagram's length, in the reply too.
 *
 * Include after sha256.inc and lighting_wire.h (for hmac_sha256, light_put64, light_u64,
 * light_equal). No game or OS dependencies.
 *
 * The message set below is a DRAFT for the first vertical slice and will be finalised together
 * with native/internal/game/eliminator.go. Everything named EL_*_DRAFT is open to change.
 */
#ifndef FH2MP_ELIMINATOR_WIRE_H
#define FH2MP_ELIMINATOR_WIRE_H

#define EL_BASE 58u          /* fixed payload bytes before the records (and the trailing MAC) */
#define EL_MAC 32u
#define EL_REQUEST (EL_BASE + EL_MAC)

/* Fixed header: side header + magic. */
static const unsigned char el_header[19] = {255,255,254,255,0,0,0,0,0,0,0,'F','H','2','M','P','E','L','1'};

/* Reply only, straight after the base: 4 bytes about the asking player (car class tier, the owner id
 * of who challenged them or 0, 2 reserved). Mirrored by the server (elimYou). */
#define EL_YOU 4u

/* Wire record for one car drop in the reply. Mirrored by the server. */
#define EL_DROP_MAX 24u
#define EL_RECORD 12u
#define EL_LIMIT (EL_BASE + EL_YOU + EL_RECORD * EL_DROP_MAX + EL_MAC)

/* Phases (reply byte 53). Matches EliminatorPhase in native/internal/game/eliminator.go. */
#define EL_PHASE_NONE 0u
#define EL_PHASE_WAITING 1u
#define EL_PHASE_DROPPING 2u
#define EL_PHASE_ACTIVE 3u
#define EL_PHASE_HEADTOHEAD 4u
#define EL_PHASE_FINISHED 5u

/* Commands (request byte 53). Matches EliminatorCmd in native/internal/game/eliminator.go. */
#define EL_CMD_JOIN 1u
#define EL_CMD_LEAVE 2u
#define EL_CMD_READY 3u
#define EL_CMD_CHALLENGE 4u   /* arg0 = target owner id (1..254), 0 = the nearest player still in */
#define EL_CMD_ACCEPT 5u      /* arg0 = challenger owner id, 0 = whoever challenged this player */
#define EL_CMD_DECLINE 6u     /* arg0 = challenger owner id, 0 = whoever challenged this player */
#define EL_CMD_DROP_CLAIM 7u  /* arg0 = drop id */

/* Reply flags (reply byte 56). */
#define EL_FLAG_AREA_SHRINKING 1u
#define EL_FLAG_ELIMINATED 2u  /* the asking player is out */
#define EL_FLAG_JOINED 4u      /* the asking player is in the mode */

typedef struct { unsigned char id, cls; float x, z; } el_drop;   /* id 1.., cls = car class tier, x/z = world metres */

static int el_finite(float f) { return f == f && f - f == 0.0f; }

/* Builds the request: header, type 1, xuid, seq, epoch, car, cmd, arg0/arg1, then the MAC. */
static void el_request(unsigned char out[EL_REQUEST], const unsigned char key[32],
                       unsigned long long xuid, unsigned long long seq, const unsigned char epoch[16],
                       unsigned char car, unsigned char cmd, unsigned arg0, unsigned arg1)
{
    unsigned i;
    for (i = 0; i < 19; i++) out[i] = el_header[i];
    out[9] = EL_REQUEST; out[10] = 0; out[19] = 1;
    light_put64(out + 20, xuid); light_put64(out + 28, seq);
    for (i = 0; i < 16; i++) out[36 + i] = epoch[i];
    out[52] = car; out[53] = cmd;
    out[54] = (unsigned char)(arg0 & 0xff); out[55] = (unsigned char)((arg0 >> 8) & 0xff);
    out[56] = (unsigned char)(arg1 & 0xff); out[57] = 0;
    hmac_sha256(key, 32, out, EL_BASE, out + EL_BASE);
}

/* Validates a reply (same rules as light_response/honk_response: this player's xuid and epoch, a
 * sequence that is new and recent, the signature) and unpacks phase/players/area/flags, the asking
 * player's tier and challenger, and the drop records into `out`; returns 1 and *count, or 0 for
 * anything else. */
static int el_response(const unsigned char *data, unsigned n, const unsigned char key[32],
                       unsigned long long xuid, unsigned long long sent, unsigned long long received,
                       const unsigned char epoch[16],
                       unsigned char *phase, unsigned char *players, unsigned char *area, unsigned char *flags,
                       unsigned char *tier, unsigned char *challenger,
                       el_drop out[EL_DROP_MAX], unsigned *count)
{
    unsigned i, j, c; unsigned char mac[32]; unsigned long long seq;
    if (n < EL_BASE + EL_YOU + EL_MAC || n > EL_LIMIT || !light_equal(data, el_header, 9) || !light_equal(data + 11, el_header + 11, 8) ||
        (unsigned)(data[9] | data[10] << 8) != n || data[19] != 2 || light_u64(data + 20) != xuid ||
        !light_equal(data + 36, epoch, 16) || data[52] > 127) return 0;
    seq = light_u64(data + 28); c = data[57];
    if (seq <= received || seq > sent || sent - seq > 20 || c > EL_DROP_MAX || n != EL_BASE + EL_YOU + EL_RECORD * c + EL_MAC) return 0;
    hmac_sha256(key, 32, data, n - EL_MAC, mac);
    if (!light_equal(mac, data + n - EL_MAC, EL_MAC)) return 0;
    *phase = data[53]; *players = data[54]; *area = data[55]; *flags = data[56];
    *tier = data[EL_BASE]; *challenger = data[EL_BASE + 1];
    for (i = 0; i < c; i++) {
        const unsigned char *r = data + EL_BASE + EL_YOU + EL_RECORD * i; el_drop *o = out + i; unsigned k;
        o->id = r[0]; o->cls = r[1];
        for (k = 0; k < 2; k++) {
            unsigned char *dst = (unsigned char *)&o->x + 4 * k; unsigned b;
            for (b = 0; b < 4; b++) dst[b] = r[2 + 4 * k + b];
            if (!el_finite(k ? o->z : o->x)) return 0;
        }
        if (!o->id || o->id == 255) return 0;
        for (j = 0; j < i; j++) if (out[j].id == o->id) return 0;
    }
    *count = c;
    return 1;
}

#endif
