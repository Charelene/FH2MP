/* Chat link v1 wire format: the shim <-> the chat core (fh2mp-chat), loopback TCP. Layout and message list: tests/vectors/chat_link.txt.
 * Pure C, no Windows: encoders write one whole frame and return its size (0 when it does not fit), cl_frame finds one frame in a receive
 * buffer, the decoders check a payload's exact size. The Rust mirror is client/crates/fh2mp-chat/src/link_wire.rs; both are tested against
 * the same vectors. */
#ifndef FH2MP_CHAT_LINK_WIRE_H
#define FH2MP_CHAT_LINK_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CL_VERSION 1
#define CL_MAX_FRAME 65536u   /* type + payload */
#define CL_MAX_ARG 512u

enum { CL_HELLO = 1, CL_WELCOME, CL_PAD, CL_GUIDE, CL_GUIDE_STATE, CL_WINDOW, CL_GAME_STATE, CL_OPEN_PAGE, CL_BYE };

struct cl_hello { uint16_t version; uint8_t secret[32]; uint32_t pid; uint64_t hwnd, xuid; };
struct cl_pad { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
struct cl_window { int32_t x, y, w, h; uint8_t flags; };
struct cl_game_state { uint8_t online, mode; uint16_t players; };
struct cl_open_page { uint8_t page; uint16_t len; char arg[CL_MAX_ARG + 1]; };

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

static size_t cl_put_hello(uint8_t *out, size_t cap, const struct cl_hello *m)
{
    size_t size = cl_head(out, cap, CL_HELLO, 54);
    if (size) {
        cl_le(out + 6, m->version, 2);
        memcpy(out + 8, m->secret, 32);
        cl_le(out + 40, m->pid, 4);
        cl_le(out + 44, m->hwnd, 8);
        cl_le(out + 52, m->xuid, 8);
    }
    return size;
}

static size_t cl_put_welcome(uint8_t *out, size_t cap, uint16_t version, uint8_t flags)
{
    size_t size = cl_head(out, cap, CL_WELCOME, 3);
    if (size) {
        cl_le(out + 6, version, 2);
        out[8] = flags;
    }
    return size;
}

static size_t cl_put_pad(uint8_t *out, size_t cap, const struct cl_pad *m)
{
    size_t size = cl_head(out, cap, CL_PAD, 12);
    if (size) {
        cl_le(out + 6, m->buttons, 2);
        out[8] = m->lt;
        out[9] = m->rt;
        cl_le(out + 10, (uint16_t)m->lx, 2);
        cl_le(out + 12, (uint16_t)m->ly, 2);
        cl_le(out + 14, (uint16_t)m->rx, 2);
        cl_le(out + 16, (uint16_t)m->ry, 2);
    }
    return size;
}

/* GUIDE, GUIDE_STATE and BYE carry one byte. */
static size_t cl_put_byte(uint8_t *out, size_t cap, uint16_t type, uint8_t v)
{
    size_t size = cl_head(out, cap, type, 1);
    if (size) {
        out[6] = v;
    }
    return size;
}

static size_t cl_put_window(uint8_t *out, size_t cap, const struct cl_window *m)
{
    size_t size = cl_head(out, cap, CL_WINDOW, 17);
    if (size) {
        cl_le(out + 6, (uint32_t)m->x, 4);
        cl_le(out + 10, (uint32_t)m->y, 4);
        cl_le(out + 14, (uint32_t)m->w, 4);
        cl_le(out + 18, (uint32_t)m->h, 4);
        out[22] = m->flags;
    }
    return size;
}

static size_t cl_put_game_state(uint8_t *out, size_t cap, const struct cl_game_state *m)
{
    size_t size = cl_head(out, cap, CL_GAME_STATE, 4);
    if (size) {
        out[6] = m->online;
        out[7] = m->mode;
        cl_le(out + 8, m->players, 2);
    }
    return size;
}

static size_t cl_put_open_page(uint8_t *out, size_t cap, uint8_t page, const char *arg, size_t n)
{
    size_t size;
    if (n > CL_MAX_ARG) {
        return 0;
    }
    size = cl_head(out, cap, CL_OPEN_PAGE, 3 + n);
    if (size) {
        out[6] = page;
        cl_le(out + 7, (uint64_t)n, 2);
        memcpy(out + 9, arg, n);
    }
    return size;
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

static int cl_get_hello(const uint8_t *p, size_t n, struct cl_hello *m)
{
    if (n != 54) {
        return 0;
    }
    m->version = (uint16_t)cl_rd(p, 2);
    memcpy(m->secret, p + 2, 32);
    m->pid = (uint32_t)cl_rd(p + 34, 4);
    m->hwnd = cl_rd(p + 38, 8);
    m->xuid = cl_rd(p + 46, 8);
    return 1;
}

static int cl_get_welcome(const uint8_t *p, size_t n, uint16_t *version, uint8_t *flags)
{
    if (n != 3) {
        return 0;
    }
    *version = (uint16_t)cl_rd(p, 2);
    *flags = p[2];
    return 1;
}

static int cl_get_pad(const uint8_t *p, size_t n, struct cl_pad *m)
{
    if (n != 12) {
        return 0;
    }
    m->buttons = (uint16_t)cl_rd(p, 2);
    m->lt = p[2];
    m->rt = p[3];
    m->lx = (int16_t)(uint16_t)cl_rd(p + 4, 2);
    m->ly = (int16_t)(uint16_t)cl_rd(p + 6, 2);
    m->rx = (int16_t)(uint16_t)cl_rd(p + 8, 2);
    m->ry = (int16_t)(uint16_t)cl_rd(p + 10, 2);
    return 1;
}

static int cl_get_byte(const uint8_t *p, size_t n, uint8_t *v)
{
    if (n != 1) {
        return 0;
    }
    *v = p[0];
    return 1;
}

static int cl_get_window(const uint8_t *p, size_t n, struct cl_window *m)
{
    if (n != 17) {
        return 0;
    }
    m->x = (int32_t)(uint32_t)cl_rd(p, 4);
    m->y = (int32_t)(uint32_t)cl_rd(p + 4, 4);
    m->w = (int32_t)(uint32_t)cl_rd(p + 8, 4);
    m->h = (int32_t)(uint32_t)cl_rd(p + 12, 4);
    m->flags = p[16];
    return 1;
}

static int cl_get_game_state(const uint8_t *p, size_t n, struct cl_game_state *m)
{
    if (n != 4) {
        return 0;
    }
    m->online = p[0];
    m->mode = p[1];
    m->players = (uint16_t)cl_rd(p + 2, 2);
    return 1;
}

static int cl_get_open_page(const uint8_t *p, size_t n, struct cl_open_page *m)
{
    size_t len;
    if (n < 3) {
        return 0;
    }
    len = (size_t)cl_rd(p + 1, 2);
    if (len > CL_MAX_ARG || n != 3 + len) {
        return 0;
    }
    m->page = p[0];
    m->len = (uint16_t)len;
    memcpy(m->arg, p + 3, len);
    m->arg[len] = 0;
    return 1;
}

#endif
