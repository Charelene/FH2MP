/* FH2 1.0.0.64: BitStream::ReadString (+17676b0) uses a negative
 * ReadBytes result as a u32 stack index (#144). Validate at each of its three
 * receive-handler boundaries, where 80600006 already means message failure.
 * Never call a listener, construct a substitute string, or advance the native
 * cursor on failure. Dispatch then skips the rest of the packet (+1761d60).
 */
typedef unsigned long long (*net_string_receive_fn)(void *, void *, void *, ULONG_PTR);
static net_string_receive_fn g_string_open_orig, g_string_meet_orig, g_string_bucket_orig;
static volatile LONG g_string_rejections[3];

/* why/field/limit: set where the walk failed (the receive wrapper names them in the log); the others describe the cursor */
typedef struct { UINT_PTR data; unsigned size, pos; const char *why; unsigned field_size, field_limit; } net_string_cursor;

static int net_string_span(net_string_cursor *c, unsigned count, unsigned char *out)
{
    unsigned first, size;
    if (c->pos > c->size || count > c->size - c->pos) { c->why = "the message ends inside a string field"; return 0; }
    first = c->pos >> 3;
    size = ((c->pos & 7) + count + 7) >> 3;
    if (first > (UINT_PTR)-1 - c->data ||
        (size && !read_game_memory((const void *)(c->data + first), out, size))) { c->why = "the datagram buffer cannot be read"; return 0; }
    return 1;
}

static int net_string_bits(net_string_cursor *c, unsigned count, unsigned *out)
{
    unsigned char bytes[5];
    unsigned value = 0, i;
    if (count > 32 || !net_string_span(c, count, bytes)) return 0;
    for (i = 0; i < count; ++i)
        value |= (unsigned)((bytes[((c->pos & 7) + i) >> 3] >> (((c->pos & 7) + i) & 7)) & 1) << i;
    c->pos += count;
    if (out) *out = value;
    return 1;
}

static int net_string_skip(net_string_cursor *c, unsigned count)
{
    unsigned char bytes[65];
    /* Largest field below is a 64-byte string, including an unaligned byte. */
    if (count > 512 || !net_string_span(c, count, bytes)) return 0;
    c->pos += count;
    return 1;
}

static int net_string_field(net_string_cursor *c, unsigned limit)
{
    unsigned kind, size;
    if (!net_string_bits(c, 2, &kind) || !net_string_bits(c, kind * 8 + 6, &size)) return 0;
    if (size > limit) {
        c->why = "a string longer than its field allows";
        c->field_size = size;
        c->field_limit = limit;
        return 0;
    }
    return net_string_skip(c, size * 8);
}

static int net_string_check(const void *stream, unsigned handler, net_string_cursor *out_state)
{
    /* +4 size in bits, +8 byte cursor, +c bit cursor, +18 shared buffer;
     * buffer object +10 is its byte-storage pointer (native +1767940).
     */
    unsigned char header[32];
    UINT_PTR holder, data;
    unsigned size, byte_pos, bit_pos, flags;
    net_string_cursor c = {0};
    int ok = 0;
    c.why = "the message is not one of the three guarded handlers";
    if (!stream || !read_game_memory(stream, header, sizeof header)) { c.why = "the reader object cannot be read"; goto out; }
    memcpy(&size, header + 4, 4);
    memcpy(&byte_pos, header + 8, 4);
    memcpy(&bit_pos, header + 12, 4);
    memcpy(&holder, header + 24, sizeof holder);
    c.size = size; c.pos = bit_pos;
    if (!holder || holder > (UINT_PTR)-1 - 16 || bit_pos > size || byte_pos != (bit_pos >> 3) ||
        !read_game_memory((const void *)(holder + 16), &data, sizeof data) || !data) {
        c.why = !holder || holder > (UINT_PTR)-1 - 16 || !data ? "the reader has no buffer" : "the reader's bit position is inconsistent with its byte position or size";
        goto out;
    }
    c.data = data;
    if (handler == 0) /* OpenConnection Receive +176d570 */
        ok = net_string_field(&c, 0x26) && net_string_field(&c, 0x10) && net_string_skip(&c, 162);
    else if (handler == 1) /* MsgCarMeetGameModeSessionReady Receive +17f7440 */
        ok = net_string_field(&c, 0x40) && net_string_field(&c, 0x40);
    else if (handler == 2) { /* RespondBucketListPartner Receive +181f630 */
        ok = net_string_bits(&c, 2, &flags) && net_string_skip(&c, 7) &&
             (flags != 2 || net_string_skip(&c, 8)) && net_string_skip(&c, 6) &&
             net_string_field(&c, 0x24);
    }
out:
    if (out_state) *out_state = c;
    return ok;
}

static __attribute__((unused)) int net_string_preflight(const void *stream, unsigned handler)
{
    return net_string_check(stream, handler, NULL);
}

static unsigned long long net_string_receive(unsigned handler, net_string_receive_fn original,
                                              void *peer, void *stream, void *listener, ULONG_PTR extra)
{
    net_string_cursor why = {0};
    if (!original || !net_string_check(stream, handler, &why)) {
        /* the sender gets no answer and the rest of its datagram is skipped (native 80600006); a client that keeps sending these is named by the count */
        LONG n = InterlockedIncrement(&g_string_rejections[handler < 3 ? handler : 0]);
        if (n <= 4 || n % 256 == 0) {
            char line[512];
            wsprintfA(line, "netstrings: rejected %s message before native decoding (#%ld): %s; reader has %u bits, cursor at bit %u",
                      handler == 0 ? "an OpenConnection" : handler == 1 ? "a CarMeet session-ready" : "a BucketList partner response", (long)n,
                      original ? why.why : "the original handler is not known", why.size, why.pos);
            if (why.field_limit) {
                wsprintfA(line + lstrlenA(line), ", a string of %u characters where %u is the most", why.field_size, why.field_limit);
            }
            log_begin();
            log_raw(line);
            log_raw("\n");
        }
        return 0x80600006u;
    }
    return original(peer, stream, listener, extra);
}

static unsigned long long net_string_open(void *p, void *s, void *l, ULONG_PTR x)
{ return net_string_receive(0, g_string_open_orig, p, s, l, x); }
static unsigned long long net_string_meet(void *p, void *s, void *l, ULONG_PTR x)
{ return net_string_receive(1, g_string_meet_orig, p, s, l, x); }
static unsigned long long net_string_bucket(void *p, void *s, void *l, ULONG_PTR x)
{ return net_string_receive(2, g_string_bucket_orig, p, s, l, x); }
