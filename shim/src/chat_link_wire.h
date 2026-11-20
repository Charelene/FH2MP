/* Chat link v2 wire format: the in-game Guide (Encore Launcher's guide/, inside the game) <-> Encore, loopback TCP. Layout and message
 * list: tests/vectors/chat_link.txt. Pure C, no Windows, usable from C and C++: encoders write one whole frame and return its size (0 when
 * it does not fit), cl_frame finds one frame in a receive buffer, the decoders check that a payload decodes exactly. A str goes out as the
 * caller's bytes and length (at most CL_MAX_STR; cl_utf8_cut shortens longer text at a UTF-8 boundary) and comes back into a struct cl_str,
 * a buffer of CL_MAX_STR bytes and a terminating 0. CHATS and MESSAGES are written entry by entry (cl_chats_begin / cl_messages_begin,
 * cl_*_add, cl_list_end) and read entry by entry (cl_get_chats / cl_get_messages check the whole payload first, then cl_next_*). The Rust
 * mirror is Encore Launcher's crates/encore-core/src/link_wire.rs; both are tested against the same vectors. */
#ifndef FH2MP_CHAT_LINK_WIRE_H
#define FH2MP_CHAT_LINK_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CL_VERSION 2
#define CL_MAX_FRAME 65536u  /* type + payload */
#define CL_MAX_STR 8192u     /* bytes of one str */
#define CL_MAX_TEXT 2000u    /* characters of a SEND's text (Encore's rule; the wire carries any str) */

enum { CL_HELLO = 1, CL_WELCOME = 2, CL_BYE = 9, CL_OPEN = 10, CL_CLOSE = 11, CL_SEND = 12, CL_READ = 13,
       CL_CHATS = 20, CL_MESSAGES = 21, CL_SENT = 22, CL_NOTICE = 23 };
enum { CL_BYE_GAME = 1, CL_BYE_ENCORE = 2, CL_BYE_PROTOCOL = 3, CL_BYE_REFUSED = 4 };  /* BYE reasons */
enum { CL_WELCOME_E2E = 1 };                                                          /* WELCOME flags */
enum { CL_CHAT_E2E = 1, CL_CHAT_CLUB = 2 };                                           /* a CHATS entry's flags */
enum { CL_MSG_UNREAD = 0, CL_MSG_SEEN = 1, CL_MSG_SENDING = 2, CL_MSG_SENT = 3, CL_MSG_READ = 4, CL_MSG_FAILED = 5 };  /* a message's state */
enum { CL_NOTICE_DM = 1, CL_NOTICE_CLUB = 2, CL_NOTICE_NEW_PC = 3 };                  /* NOTICE kinds */

struct cl_str { uint16_t len; char s[CL_MAX_STR + 1]; };
struct cl_hello { uint16_t version; uint8_t secret[32]; uint32_t pid; uint64_t xuid; };
struct cl_welcome { uint16_t version; uint8_t flags; uint64_t xuid; };
struct cl_send { uint32_t request; struct cl_str chat, text; };
struct cl_read { struct cl_str chat; int64_t max_id; };
struct cl_chat { struct cl_str id, title, last; int32_t unread; int64_t last_incoming; uint8_t flags; };
struct cl_message { int64_t id; uint64_t sender; struct cl_str gamertag, body; int64_t created; uint8_t state; };
struct cl_sent { uint32_t request; uint8_t ok; struct cl_str why; };
struct cl_notice { uint8_t kind; struct cl_str chat, title, text; };
/* A CHATS or MESSAGES frame being written into the caller's buffer. */
struct cl_list { uint8_t *out; size_t cap, size, count_at; uint16_t count; int bad; };
/* The entries of a CHATS or MESSAGES payload that cl_get_* checked: the next one at `at`, `left` to go. */
struct cl_entries { const uint8_t *p; size_t n, at; uint16_t left; };

static void cl_le(uint8_t *p, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t cl_rd(const uint8_t *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = n - 1; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* The longest start of s (len bytes) that is at most max bytes and does not end inside a UTF-8 sequence. */
static size_t cl_utf8_cut(const char *s, size_t len, size_t max)
{
    size_t n;
    if (len <= max) {
        return len;
    }
    n = max;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

/* The 6-byte header for a payload of n bytes; returns the whole frame size, or 0 when cap is too small. */
static size_t cl_head(uint8_t *out, size_t cap, uint16_t type, size_t n)
{
    if (n + 2 > CL_MAX_FRAME || cap < n + 6) {
        return 0;
    }
    cl_le(out, (uint64_t)(n + 2), 4);
    cl_le(out + 4, type, 2);
    return n + 6;
}

/* A str at p; returns the bytes written (the caller checked len <= CL_MAX_STR and the room). */
static size_t cl_put_str(uint8_t *p, const char *s, size_t len)
{
    cl_le(p, (uint64_t)len, 2);
    if (len) {
        memcpy(p + 2, s, len);
    }
    return 2 + len;
}

static size_t cl_put_hello(uint8_t *out, size_t cap, const struct cl_hello *m)
{
    size_t size = cl_head(out, cap, CL_HELLO, 46);
    if (size) {
        cl_le(out + 6, m->version, 2);
        memcpy(out + 8, m->secret, 32);
        cl_le(out + 40, m->pid, 4);
        cl_le(out + 44, m->xuid, 8);
    }
    return size;
}

static size_t cl_put_welcome(uint8_t *out, size_t cap, const struct cl_welcome *m)
{
    size_t size = cl_head(out, cap, CL_WELCOME, 11);
    if (size) {
        cl_le(out + 6, m->version, 2);
        out[8] = m->flags;
        cl_le(out + 9, m->xuid, 8);
    }
    return size;
}

static size_t cl_put_bye(uint8_t *out, size_t cap, uint8_t reason)
{
    size_t size = cl_head(out, cap, CL_BYE, 1);
    if (size) {
        out[6] = reason;
    }
    return size;
}

static size_t cl_put_open(uint8_t *out, size_t cap, const char *chat, size_t chat_len)
{
    size_t size;
    if (chat_len > CL_MAX_STR) {
        return 0;
    }
    size = cl_head(out, cap, CL_OPEN, 2 + chat_len);
    if (size) {
        cl_put_str(out + 6, chat, chat_len);
    }
    return size;
}

static size_t cl_put_close(uint8_t *out, size_t cap)
{
    return cl_head(out, cap, CL_CLOSE, 0);
}

static size_t cl_put_send(uint8_t *out, size_t cap, uint32_t request, const char *chat, size_t chat_len, const char *text, size_t text_len)
{
    size_t size;
    if (chat_len > CL_MAX_STR || text_len > CL_MAX_STR) {
        return 0;
    }
    size = cl_head(out, cap, CL_SEND, 4 + 2 + chat_len + 2 + text_len);
    if (size) {
        cl_le(out + 6, request, 4);
        cl_put_str(out + 10 + cl_put_str(out + 10, chat, chat_len), text, text_len);
    }
    return size;
}

static size_t cl_put_read(uint8_t *out, size_t cap, const char *chat, size_t chat_len, int64_t max_id)
{
    size_t size;
    if (chat_len > CL_MAX_STR) {
        return 0;
    }
    size = cl_head(out, cap, CL_READ, 2 + chat_len + 8);
    if (size) {
        cl_le(out + 6 + cl_put_str(out + 6, chat, chat_len), (uint64_t)max_id, 8);
    }
    return size;
}

static size_t cl_put_sent(uint8_t *out, size_t cap, uint32_t request, uint8_t ok, const char *why, size_t why_len)
{
    size_t size;
    if (why_len > CL_MAX_STR) {
        return 0;
    }
    size = cl_head(out, cap, CL_SENT, 4 + 1 + 2 + why_len);
    if (size) {
        cl_le(out + 6, request, 4);
        out[10] = ok;
        cl_put_str(out + 11, why, why_len);
    }
    return size;
}

static size_t cl_put_notice(uint8_t *out, size_t cap, uint8_t kind, const char *chat, size_t chat_len, const char *title, size_t title_len,
                            const char *text, size_t text_len)
{
    size_t size, at;
    if (chat_len > CL_MAX_STR || title_len > CL_MAX_STR || text_len > CL_MAX_STR) {
        return 0;
    }
    size = cl_head(out, cap, CL_NOTICE, 1 + 2 + chat_len + 2 + title_len + 2 + text_len);
    if (size) {
        out[6] = kind;
        at = 7 + cl_put_str(out + 7, chat, chat_len);
        at += cl_put_str(out + at, title, title_len);
        cl_put_str(out + at, text, text_len);
    }
    return size;
}

/* k more bytes of a list being written, or NULL (the list unchanged) when they do not fit the buffer or the frame. */
static uint8_t *cl_list_room(struct cl_list *l, size_t k)
{
    uint8_t *p;
    if (l->bad || l->count == 0xFFFF || k > l->cap - l->size || l->size + k - 4 > CL_MAX_FRAME) {
        return NULL;
    }
    p = l->out + l->size;
    l->size += k;
    return p;
}

static void cl_list_start(struct cl_list *l, uint8_t *out, size_t cap, uint16_t type, const char *chat, size_t chat_len)
{
    l->out = out, l->cap = cap, l->size = 0, l->count_at = 0, l->count = 0;
    l->bad = chat_len > CL_MAX_STR || cap < 6 + (chat ? 2 + chat_len : 0) + 2;
    if (l->bad) {
        return;
    }
    cl_le(out + 4, type, 2);
    l->size = 6;
    if (chat) {
        l->size += cl_put_str(out + 6, chat, chat_len);
    }
    l->count_at = l->size;
    l->size += 2;
}

/* CHATS: then one cl_chats_add per conversation and cl_list_end. */
static void cl_chats_begin(struct cl_list *l, uint8_t *out, size_t cap)
{
    cl_list_start(l, out, cap, CL_CHATS, NULL, 0);
}

/* MESSAGES of one conversation: then one cl_messages_add per message, oldest first, and cl_list_end. */
static void cl_messages_begin(struct cl_list *l, uint8_t *out, size_t cap, const char *chat, size_t chat_len)
{
    cl_list_start(l, out, cap, CL_MESSAGES, chat ? chat : "", chat_len);
}

/* One conversation; 0 (the list unchanged) when it does not fit. */
static int cl_chats_add(struct cl_list *l, const char *id, size_t id_len, const char *title, size_t title_len, const char *last, size_t last_len,
                        int32_t unread, int64_t last_incoming, uint8_t flags)
{
    uint8_t *p;
    if (id_len > CL_MAX_STR || title_len > CL_MAX_STR || last_len > CL_MAX_STR) {
        return 0;
    }
    p = cl_list_room(l, 2 + id_len + 2 + title_len + 2 + last_len + 4 + 8 + 1);
    if (!p) {
        return 0;
    }
    p += cl_put_str(p, id, id_len);
    p += cl_put_str(p, title, title_len);
    p += cl_put_str(p, last, last_len);
    cl_le(p, (uint32_t)unread, 4);
    cl_le(p + 4, (uint64_t)last_incoming, 8);
    p[12] = flags;
    l->count++;
    return 1;
}

/* One message; 0 (the list unchanged) when it does not fit. */
static int cl_messages_add(struct cl_list *l, int64_t id, uint64_t sender, const char *gamertag, size_t gamertag_len, const char *body,
                           size_t body_len, int64_t created, uint8_t state)
{
    uint8_t *p;
    if (gamertag_len > CL_MAX_STR || body_len > CL_MAX_STR) {
        return 0;
    }
    p = cl_list_room(l, 8 + 8 + 2 + gamertag_len + 2 + body_len + 8 + 1);
    if (!p) {
        return 0;
    }
    cl_le(p, (uint64_t)id, 8);
    cl_le(p + 8, sender, 8);
    p += 16 + cl_put_str(p + 16, gamertag, gamertag_len);
    p += cl_put_str(p, body, body_len);
    cl_le(p, (uint64_t)created, 8);
    p[8] = state;
    l->count++;
    return 1;
}

/* The finished CHATS or MESSAGES frame's size, 0 when even its header did not fit. */
static size_t cl_list_end(struct cl_list *l)
{
    if (l->bad) {
        return 0;
    }
    cl_le(l->out, (uint64_t)(l->size - 4), 4);
    cl_le(l->out + l->count_at, l->count, 2);
    return l->size;
}

/* One frame at the start of buf: 1 with its type, payload and whole size, 0 when more bytes are needed, -1 when the length is impossible
 * (the link must close). */
static int cl_frame(const uint8_t *buf, size_t len, uint16_t *type, const uint8_t **payload, size_t *plen, size_t *used)
{
    uint64_t n;
    if (len < 4) {
        return 0;
    }
    n = cl_rd(buf, 4);
    if (n < 2 || n > CL_MAX_FRAME) {
        return -1;
    }
    if (len < 4 + n) {
        return 0;
    }
    *type = (uint16_t)cl_rd(buf + 4, 2);
    *payload = buf + 6;
    *plen = (size_t)n - 2;
    *used = (size_t)n + 4;
    return 1;
}

/* k bytes of a payload at *at (moved past them), or NULL when the payload has fewer left. */
static const uint8_t *cl_take(const uint8_t *p, size_t n, size_t *at, size_t k)
{
    const uint8_t *q;
    if (*at > n || k > n - *at) {
        return NULL;
    }
    q = p + *at;
    *at += k;
    return q;
}

static int cl_take_num(const uint8_t *p, size_t n, size_t *at, int k, uint64_t *v)
{
    const uint8_t *q = cl_take(p, n, at, (size_t)k);
    if (!q) {
        return 0;
    }
    *v = cl_rd(q, k);
    return 1;
}

/* A str at *at into s (NULL: only checked): 0 when it is longer than CL_MAX_STR or than what is left. */
static int cl_take_str(const uint8_t *p, size_t n, size_t *at, struct cl_str *s)
{
    const uint8_t *q = cl_take(p, n, at, 2);
    size_t len;
    if (!q) {
        return 0;
    }
    len = (size_t)cl_rd(q, 2);
    if (len > CL_MAX_STR || !(q = cl_take(p, n, at, len))) {
        return 0;
    }
    if (s) {
        s->len = (uint16_t)len;
        if (len) {
            memcpy(s->s, q, len);
        }
        s->s[len] = 0;
    }
    return 1;
}

/* One CHATS entry at *at into c (NULL: only checked). */
static int cl_take_chat(const uint8_t *p, size_t n, size_t *at, struct cl_chat *c)
{
    uint64_t unread, last, flags;
    if (!cl_take_str(p, n, at, c ? &c->id : NULL) || !cl_take_str(p, n, at, c ? &c->title : NULL) || !cl_take_str(p, n, at, c ? &c->last : NULL) ||
        !cl_take_num(p, n, at, 4, &unread) || !cl_take_num(p, n, at, 8, &last) || !cl_take_num(p, n, at, 1, &flags)) {
        return 0;
    }
    if (c) {
        c->unread = (int32_t)(uint32_t)unread;
        c->last_incoming = (int64_t)last;
        c->flags = (uint8_t)flags;
    }
    return 1;
}

/* One MESSAGES entry at *at into m (NULL: only checked). */
static int cl_take_message(const uint8_t *p, size_t n, size_t *at, struct cl_message *m)
{
    uint64_t id, sender, created, state;
    if (!cl_take_num(p, n, at, 8, &id) || !cl_take_num(p, n, at, 8, &sender) || !cl_take_str(p, n, at, m ? &m->gamertag : NULL) ||
        !cl_take_str(p, n, at, m ? &m->body : NULL) || !cl_take_num(p, n, at, 8, &created) || !cl_take_num(p, n, at, 1, &state)) {
        return 0;
    }
    if (m) {
        m->id = (int64_t)id;
        m->sender = sender;
        m->created = (int64_t)created;
        m->state = (uint8_t)state;
    }
    return 1;
}

static int cl_get_hello(const uint8_t *p, size_t n, struct cl_hello *m)
{
    if (n != 46) {
        return 0;
    }
    m->version = (uint16_t)cl_rd(p, 2);
    memcpy(m->secret, p + 2, 32);
    m->pid = (uint32_t)cl_rd(p + 34, 4);
    m->xuid = cl_rd(p + 38, 8);
    return 1;
}

static int cl_get_welcome(const uint8_t *p, size_t n, struct cl_welcome *m)
{
    if (n != 11) {
        return 0;
    }
    m->version = (uint16_t)cl_rd(p, 2);
    m->flags = p[2];
    m->xuid = cl_rd(p + 3, 8);
    return 1;
}

static int cl_get_bye(const uint8_t *p, size_t n, uint8_t *reason)
{
    if (n != 1) {
        return 0;
    }
    *reason = p[0];
    return 1;
}

static int cl_get_open(const uint8_t *p, size_t n, struct cl_str *chat)
{
    size_t at = 0;
    return cl_take_str(p, n, &at, chat) && at == n;
}

static int cl_get_close(const uint8_t *p, size_t n)
{
    (void)p;
    return n == 0;
}

static int cl_get_send(const uint8_t *p, size_t n, struct cl_send *m)
{
    size_t at = 0;
    uint64_t request;
    if (!cl_take_num(p, n, &at, 4, &request) || !cl_take_str(p, n, &at, &m->chat) || !cl_take_str(p, n, &at, &m->text) || at != n) {
        return 0;
    }
    m->request = (uint32_t)request;
    return 1;
}

static int cl_get_read(const uint8_t *p, size_t n, struct cl_read *m)
{
    size_t at = 0;
    uint64_t max_id;
    if (!cl_take_str(p, n, &at, &m->chat) || !cl_take_num(p, n, &at, 8, &max_id) || at != n) {
        return 0;
    }
    m->max_id = (int64_t)max_id;
    return 1;
}

static int cl_get_sent(const uint8_t *p, size_t n, struct cl_sent *m)
{
    size_t at = 0;
    uint64_t request, ok;
    if (!cl_take_num(p, n, &at, 4, &request) || !cl_take_num(p, n, &at, 1, &ok) || !cl_take_str(p, n, &at, &m->why) || at != n) {
        return 0;
    }
    m->request = (uint32_t)request;
    m->ok = (uint8_t)ok;
    return 1;
}

static int cl_get_notice(const uint8_t *p, size_t n, struct cl_notice *m)
{
    size_t at = 0;
    uint64_t kind;
    if (!cl_take_num(p, n, &at, 1, &kind) || !cl_take_str(p, n, &at, &m->chat) || !cl_take_str(p, n, &at, &m->title) ||
        !cl_take_str(p, n, &at, &m->text) || at != n) {
        return 0;
    }
    m->kind = (uint8_t)kind;
    return 1;
}

/* CHATS: 1 when the whole payload decodes exactly, with e at the first of e->left conversations (cl_next_chat). */
static int cl_get_chats(const uint8_t *p, size_t n, struct cl_entries *e)
{
    size_t at = 0, first;
    uint64_t count, i;
    if (!cl_take_num(p, n, &at, 2, &count)) {
        return 0;
    }
    first = at;
    for (i = 0; i < count; i++) {
        if (!cl_take_chat(p, n, &at, NULL)) {
            return 0;
        }
    }
    if (at != n) {
        return 0;
    }
    e->p = p, e->n = n, e->at = first, e->left = (uint16_t)count;
    return 1;
}

/* The next conversation of a checked CHATS payload: 1 with it in c, 0 after the last. */
static int cl_next_chat(struct cl_entries *e, struct cl_chat *c)
{
    if (!e->left || !cl_take_chat(e->p, e->n, &e->at, c)) {
        return 0;
    }
    e->left--;
    return 1;
}

/* MESSAGES: 1 when the whole payload decodes exactly, with its conversation in chat and e at the first of e->left messages
 * (cl_next_message). */
static int cl_get_messages(const uint8_t *p, size_t n, struct cl_str *chat, struct cl_entries *e)
{
    size_t at = 0, first;
    uint64_t count, i;
    if (!cl_take_str(p, n, &at, chat) || !cl_take_num(p, n, &at, 2, &count)) {
        return 0;
    }
    first = at;
    for (i = 0; i < count; i++) {
        if (!cl_take_message(p, n, &at, NULL)) {
            return 0;
        }
    }
    if (at != n) {
        return 0;
    }
    e->p = p, e->n = n, e->at = first, e->left = (uint16_t)count;
    return 1;
}

/* The next message of a checked MESSAGES payload: 1 with it in m, 0 after the last. */
static int cl_next_message(struct cl_entries *e, struct cl_message *m)
{
    if (!e->left || !cl_take_message(e->p, e->n, &e->at, m)) {
        return 0;
    }
    e->left--;
    return 1;
}

#endif
