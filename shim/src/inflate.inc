/*
 * Reading one file out of a zip in the game's media folder (stored or deflated), without a C runtime: zip_read_file() for the stateflow
 * change in fh2mp_mod.c. inflate_raw() is a plain RFC 1951 decoder (canonical Huffman codes decoded bit by bit, as in zlib's puff); every
 * read and write is bounds-checked, and any error returns 0. Checked against Python's zlib by shim/test_inflate.sh.
 */
typedef struct {
    const unsigned char *in, *in_end;
    unsigned long bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t out_len, out_cap;
    int err;
} inf_state;

typedef struct {
    short count[16];  /* codes of each length */
    short symbol[320];
} inf_huff;

static unsigned inf_bits(inf_state *s, int need)
{
    unsigned long v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in >= s->in_end) {
            s->err = 1;
            return 0;
        }
        v |= (unsigned long)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (unsigned)(v & ((1UL << need) - 1));
}

static int inf_build(inf_huff *h, const short *length, int n)
{
    short offs[16];
    int i, left = 1;

    for (i = 0; i < 16; i++) {
        h->count[i] = 0;
    }
    for (i = 0; i < n; i++) {
        h->count[length[i]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    for (i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) {
            return -1;     /* over-subscribed */
        }
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) {
        offs[i + 1] = (short)(offs[i] + h->count[i]);
    }
    for (i = 0; i < n; i++) {
        if (length[i]) {
            h->symbol[offs[length[i]]++] = (short)i;
        }
    }
    return left;
}

static int inf_decode(inf_state *s, const inf_huff *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        int count;
        code |= (int)inf_bits(s, 1);
        if (s->err) {
            return -1;
        }
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inf_codes(inf_state *s, const inf_huff *lencode, const inf_huff *distcode)
{
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                    4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int symbol;

    for (;;) {
        symbol = inf_decode(s, lencode);
        if (symbol < 0 || s->err) {
            return -1;
        }
        if (symbol < 256) {
            if (s->out_len >= s->out_cap) {
                return -1;
            }
            s->out[s->out_len++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            size_t len, dist;
            symbol -= 257;
            if (symbol >= 29) {
                return -1;
            }
            len = (size_t)lbase[symbol] + inf_bits(s, lext[symbol]);
            symbol = inf_decode(s, distcode);
            if (symbol < 0 || symbol >= 30 || s->err) {
                return -1;
            }
            dist = (size_t)dbase[symbol] + inf_bits(s, dext[symbol]);
            if (s->err || dist > s->out_len || len > s->out_cap - s->out_len) {
                return -1;
            }
            while (len--) {
                s->out[s->out_len] = s->out[s->out_len - dist];
                s->out_len++;
            }
        }
    }
}

static int inf_fixed(inf_state *s)
{
    static inf_huff lencode, distcode;
    static int built;
    if (!built) {
        short lengths[320];
        int i;
        for (i = 0; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        inf_build(&lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        inf_build(&distcode, lengths, 30);
        built = 1;
    }
    return inf_codes(s, &lencode, &distcode);
}

static int inf_dynamic(inf_state *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    inf_huff lencode, distcode;
    int nlen, ndist, ncode, index, err;

    nlen = (int)inf_bits(s, 5) + 257;
    ndist = (int)inf_bits(s, 5) + 1;
    ncode = (int)inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) {
        return -1;
    }
    for (index = 0; index < ncode; index++) {
        lengths[order[index]] = (short)inf_bits(s, 3);
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (s->err || inf_build(&lencode, lengths, 19) != 0) {
        return -1;
    }
    index = 0;
    while (index < nlen + ndist) {
        int symbol = inf_decode(s, &lencode), len = 0, rep;
        if (symbol < 0 || s->err) {
            return -1;
        }
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        if (symbol == 16) {
            if (index == 0) {
                return -1;
            }
            len = lengths[index - 1];
            rep = 3 + (int)inf_bits(s, 2);
        } else if (symbol == 17) {
            rep = 3 + (int)inf_bits(s, 3);
        } else {
            rep = 11 + (int)inf_bits(s, 7);
        }
        if (s->err || index + rep > nlen + ndist) {
            return -1;
        }
        while (rep--) {
            lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) {
        return -1;
    }
    err = inf_build(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) {
        return -1;
    }
    err = inf_build(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) {
        return -1;
    }
    return inf_codes(s, &lencode, &distcode);
}

/* Raw deflate `in` into exactly `out_cap` bytes at `out`. -> 1 when it decoded cleanly to that size. */
static int inflate_raw(const unsigned char *in, size_t in_len, unsigned char *out, size_t out_cap)
{
    inf_state s;
    int last;

    s.in = in;
    s.in_end = in + in_len;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.out = out;
    s.out_len = 0;
    s.out_cap = out_cap;
    s.err = 0;
    do {
        int type, rc;
        last = (int)inf_bits(&s, 1);
        type = (int)inf_bits(&s, 2);
        if (s.err) {
            return 0;
        }
        if (type == 0) {           /* stored: byte-aligned LEN, NLEN, data */
            unsigned len;
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.in_end - s.in < 4) {
                return 0;
            }
            len = s.in[0] | (unsigned)s.in[1] << 8;
            if ((len ^ 0xffffu) != (s.in[2] | (unsigned)s.in[3] << 8)) {
                return 0;
            }
            s.in += 4;
            if ((size_t)(s.in_end - s.in) < len || len > s.out_cap - s.out_len) {
                return 0;
            }
            mem_copy(s.out + s.out_len, s.in, len);
            s.in += len;
            s.out_len += len;
            rc = 0;
        } else if (type == 1) {
            rc = inf_fixed(&s);
        } else if (type == 2) {
            rc = inf_dynamic(&s);
        } else {
            return 0;
        }
        if (rc != 0) {
            return 0;
        }
    } while (!last);
    return s.out_len == out_cap;
}

#ifndef INFLATE_ONLY
static unsigned zip_u16(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static unsigned long zip_u32(const unsigned char *p)
{
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* The file `name` (exact, case-insensitive) out of the zip at `path`, in a HeapAlloc'd buffer (*size bytes), or NULL. */
static unsigned char *zip_read_file(const WCHAR *path, const char *name, size_t *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER fsz;
    unsigned char *zip = NULL, *out = NULL;
    DWORD got = 0;
    size_t n, i, nlen = (size_t)lstrlenA(name);

    if (f == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(f, &fsz) || fsz.QuadPart < 22 || fsz.QuadPart > (64 << 20) ||
        !(zip = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)fsz.QuadPart)) ||
        !ReadFile(f, zip, (DWORD)fsz.QuadPart, &got, NULL) || got != (DWORD)fsz.QuadPart) {
        CloseHandle(f);
        if (zip) {
            HeapFree(GetProcessHeap(), 0, zip);
        }
        return NULL;
    }
    CloseHandle(f);
    n = (size_t)fsz.QuadPart;
    for (i = n - 22;; i--) {                                    /* end of central directory */
        if (zip_u32(zip + i) == 0x06054b50) {
            size_t cd = zip_u32(zip + i + 16), entries = zip_u16(zip + i + 10), e;
            for (e = 0; e < entries && cd + 46 <= n && zip_u32(zip + cd) == 0x02014b50; e++) {
                unsigned method = zip_u16(zip + cd + 10);
                size_t csize = zip_u32(zip + cd + 20), usize = zip_u32(zip + cd + 24), fl = zip_u16(zip + cd + 28);
                size_t lho = zip_u32(zip + cd + 42), k;
                int same = fl == nlen && cd + 46 + fl <= n;
                for (k = 0; same && k < fl; k++) {
                    same = (zip[cd + 46 + k] | 0x20) == (name[k] | 0x20);
                }
                if (same && lho + 30 <= n && zip_u32(zip + lho) == 0x04034b50) {
                    size_t data = lho + 30 + zip_u16(zip + lho + 26) + zip_u16(zip + lho + 28);
                    if (data <= n && csize <= n - data && usize < (64u << 20) &&
                        (out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, usize + 1))) {
                        int ok = method == 0 ? (csize == usize && (mem_copy(out, zip + data, usize), 1))
                                             : method == 8 && inflate_raw(zip + data, csize, out, usize);
                        if (ok) {
                            *size = usize;
                        } else {
                            HeapFree(GetProcessHeap(), 0, out);
                            out = NULL;
                        }
                    }
                    break;
                }
                cd += 46 + fl + zip_u16(zip + cd + 30) + zip_u16(zip + cd + 32);
            }
            break;
        }
        if (i == 0 || n - i > 22 + 0xffff) {
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, zip);
    return out;
}
#endif
