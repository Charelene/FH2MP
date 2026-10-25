/*
 * SHA-256 (FIPS 180-4), plain C with no library calls (the shim links KERNEL32 only). Used for the car database check (gamedb_hash in
 * fh2mp_mod.c) and the UDP connection ticket (udp_ticket_send). Checked against Python's hashlib by shim/test_sha256.sh.
 */
typedef struct {
    unsigned int h[8];
    unsigned char buf[64];
    unsigned long long len;
    unsigned int n;
} sha256_ctx;

static const unsigned int sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static unsigned int sha256_ror(unsigned int x, int r)
{
    return (x >> r) | (x << (32 - r));
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    unsigned int w[64], a, b, d, e, f, g, h, cc, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (unsigned int)p[4 * i] << 24 | (unsigned int)p[4 * i + 1] << 16 | (unsigned int)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        unsigned int s0 = sha256_ror(w[i - 15], 7) ^ sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = sha256_ror(w[i - 2], 17) ^ sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (sha256_ror(e, 6) ^ sha256_ror(e, 11) ^ sha256_ror(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
        t2 = (sha256_ror(a, 2) ^ sha256_ror(a, 13) ^ sha256_ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g, g = f, f = e, e = d + t1, d = cc, cc = b, b = a, a = t1 + t2;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    static const unsigned int iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    int i;

    for (i = 0; i < 8; i++) {
        c->h[i] = iv[i];
    }
    c->len = 0;
    c->n = 0;
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, unsigned long long n)
{
    c->len += n;
    while (n) {
        if (c->n == 0 && n >= 64) {
            sha256_block(c, p);
            p += 64, n -= 64;
            continue;
        }
        c->buf[c->n++] = *p++;
        n--;
        if (c->n == 64) {
            sha256_block(c, c->buf);
            c->n = 0;
        }
    }
}

/* Writes the 32-byte digest into `out`. */
static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    unsigned long long bits = c->len * 8;
    unsigned char pad = 0x80;
    unsigned char lenb[8];
    int i;

    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) {
        sha256_update(c, &pad, 1);
    }
    for (i = 0; i < 8; i++) {
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha256_update(c, lenb, 8);
    for (i = 0; i < 32; i++) {
        out[i] = (unsigned char)(c->h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

/* Writes the digest as 64 lower-case hex digits and a NUL into `hex`. */
static void sha256_final_hex(sha256_ctx *c, char hex[65])
{
    unsigned char d[32];
    int i;

    sha256_final(c, d);
    for (i = 0; i < 32; i++) {
        hex[2 * i] = "0123456789abcdef"[d[i] >> 4];
        hex[2 * i + 1] = "0123456789abcdef"[d[i] & 15];
    }
    hex[64] = 0;
}

/* HMAC-SHA256 (RFC 2104) of `msg` under `key`. */
static void hmac_sha256(const unsigned char *key, unsigned long long klen, const unsigned char *msg, unsigned long long mlen,
                        unsigned char out[32])
{
    unsigned char k[64], pad[64], inner[32];
    sha256_ctx c;
    int i;

    for (i = 0; i < 64; i++) {
        k[i] = 0;
    }
    if (klen > 64) {
        sha256_init(&c);
        sha256_update(&c, key, klen);
        sha256_final(&c, k);
    } else {
        for (i = 0; i < (int)klen; i++) {
            k[i] = key[i];
        }
    }
    for (i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);
    for (i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}
