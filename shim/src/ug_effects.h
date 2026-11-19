/* The lights of FH2-Underglow (qiangqiang101/FH2-Underglow 1.1.0, src/neon_mod.cpp, MIT License, Copyright (c) 2026 Bartholomew Ho), ported to C for
 * other players' cars (ug_remote.inc): where each light of the ring sits (edge_point, zone_of) and its color and level at a moment (effect_now).
 * The same numbers as Underglow's, so another player's car plays the effect its owner sees. Only the music differs: the audio effects listen to
 * the owner's radio, which is not shared, so they play Underglow's own made-up beat (its fallback when it has no radio). No game or OS
 * dependencies beyond math.h; the effects take time in seconds. */
#ifndef FH2MP_UG_EFFECTS_H
#define FH2MP_UG_EFFECTS_H
#include <math.h>

#define UG_MAX_NEON 64
#define UG_EFFECTS 61
enum { UG_ZONE_FRONT, UG_ZONE_RIGHT, UG_ZONE_REAR, UG_ZONE_LEFT, UG_ZONES };
enum {
    UG_STATIC, UG_RAINBOW, UG_WAVE, UG_BREATHE, UG_WIPE, UG_STACK, UG_BLINK, UG_POLICE, UG_AUDIO, UG_CROSSING, UG_HYPNOTOAD, UG_LIGHTNING, UG_MARQUEE,
    UG_COMET, UG_FILL, UG_SWAP, UG_VISOR, UG_AUDIO_PULSE, UG_AUDIO_PEAK, UG_AUDIO_BLEND, UG_BASS_FLASH, UG_AUDIO_SPARKLE, UG_AUDIO_RAINBOW,
    UG_AUDIO_CENTER, UG_AUDIO_BUBBLES, UG_AUDIO_PARTY, UG_AUDIO_SINE, UG_AUDIO_STAR, UG_AUDIO_SYNC, UG_VU_METER, UG_AUDIO_SWIRL, UG_BOUNCING_BALL,
    UG_BREATHING_CIRCLE, UG_FRACTAL_MOTION, UG_MOTION_POINT, UG_RAIN, UG_ROTATING_BEAM, UG_SMOOTH_BLINK, UG_SPARKLE_FADE, UG_ZIGZAG, UG_MARQUEE1,
    UG_ALTERNATE_BLINK, UG_GRADIENT_WAVE, UG_MOVING_PANES, UG_NOISE_MAP, UG_SEQUENCE, UG_WAVY, UG_TWO_COLOR_MARQUEE, UG_BLOOM, UG_BUBBLES,
    UG_COLOR_WHEEL, UG_DOUBLE_RAINBOW, UG_MOTION_POINTS, UG_MOSAIC, UG_RADIAL_RAINBOW, UG_RANDOM_SPIN, UG_ROTATING_RAINBOW, UG_SPIRAL,
    UG_STARRY_NIGHT, UG_SUNRISE, UG_SWIRL_CIRCLES
};

/* the names Underglow's `status` command prints, by effect number */
static const char *const ug_effect_names[UG_EFFECTS] = {
    "Static", "Rainbow", "Rainbow Wave", "Breathe", "Wipe", "Stack", "Blink", "Police", "Audio Visualizer",
    "Crossing Beams", "Hypnotoad", "Lightning", "Random Marquee", "Comet", "Fill", "Swap", "Visor",
    "Audio Pulse", "Peak Meter", "Audio Blend", "Bass Flash", "Audio Sparkle", "Audio Rainbow", "Center Burst",
    "Audio Bubbles", "Audio Party", "Audio Sine", "Audio Star", "Audio Sync", "VU Meter", "Audio Swirl",
    "Bouncing Ball", "Breathing Circle", "Fractal Motion", "Motion Point", "Rain", "Rotating Beam", "Smooth Blink",
    "Sparkle Fade", "Zig Zag", "Marquee",
    "Alternate Blink", "Gradient Wave", "Moving Panes", "Noise Map", "Sequence", "Wavy", "Two-Color Marquee",
    "Bloom", "Bubbles", "Color Wheel", "Double Rainbow", "Motion Points", "Mosaic", "Radial Rainbow", "Random Spin",
    "Rotating Rainbow", "Spiral", "Starry Night", "Sunrise", "Swirl Circles",
};

/* What one car's neon plays: its colors (0..1), effect speed (cycles a second), LEDs per edge and the light rectangle on the car (metres: half
 * width, front and rear edge along the car's z, the side strips' trim, the height). */
typedef struct {
    float rgb1[3], rgb2[3], speed;
    int side, front, rear;
    float half_width, zf, zr, side_trim, height;
} ug_style;

/* Light i of the wheel rectangle, on the car (Underglow's edge_point with the rectangle already inset): the front edge left to right, the right side
 * front to back, the back right to left, the left side back to front. Each edge's lights sit in the middle of equal stretches of it. */
static void ug_edge_point(const ug_style *s, int i, float out[3])
{
    float hw = s->half_width, zf = s->zf, zr = s->zr, a[2], b[2], t;
    float corner[4][2];
    int per_edge[4], e = 0;
    if (hw < 0.1f) hw = 0.1f;
    if (zf < zr + 0.2f) zf = zr = (zf + zr) * 0.5f;
    corner[0][0] = -hw; corner[0][1] = zf; corner[1][0] = hw; corner[1][1] = zf;
    corner[2][0] = hw; corner[2][1] = zr; corner[3][0] = -hw; corner[3][1] = zr;
    per_edge[0] = s->front; per_edge[1] = s->side; per_edge[2] = s->rear; per_edge[3] = s->side;
    while (e < 3 && i >= per_edge[e]) i -= per_edge[e++];
    t = per_edge[e] > 0 ? ((float)i + 0.5f) / (float)per_edge[e] : 0.5f;
    a[0] = corner[e][0]; a[1] = corner[e][1];
    b[0] = corner[(e + 1) % 4][0]; b[1] = corner[(e + 1) % 4][1];
    if (e == 1 || e == 3) {   /* the sides: only between the wheel arches */
        float trim = s->side_trim, len = zf - zr, dir = b[1] > a[1] ? 1.0f : -1.0f;
        if (trim < 0.0f) trim = 0.0f;
        if (trim > len * 0.45f) trim = len * 0.45f;
        a[1] += dir * trim;
        b[1] -= dir * trim;
    }
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = s->height;
    out[2] = a[1] + (b[1] - a[1]) * t;
}

/* A fully saturated color at hue h (0..1: red, yellow, green, cyan, blue, magenta, red). */
static void ug_hue_rgb(float h, float out[3])
{
    float x; int sector;
    h = (h - floorf(h)) * 6.0f;
    x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    sector = (int)h % 6;
    switch (sector) {
    case 0: out[0] = 1; out[1] = x; out[2] = 0; break;
    case 1: out[0] = x; out[1] = 1; out[2] = 0; break;
    case 2: out[0] = 0; out[1] = 1; out[2] = x; break;
    case 3: out[0] = 0; out[1] = x; out[2] = 1; break;
    case 4: out[0] = x; out[1] = 0; out[2] = 1; break;
    default: out[0] = 1; out[1] = 0; out[2] = x; break;
    }
}

/* Where light i is, for the effects: its zone, its place along it (front and rear left to right, the sides front to back), its place among every light
 * of the zones playing the same effect (gi of gn, ring), and where it is on the car as a picture (x, y, ang, rd). */
typedef struct {
    int zone, li, ln;
    float t, lateral;
    int gi, gn;
    float ring, x, y, ang, rd;
    int id;
} ug_pixel;

static void ug_zone_of(const ug_style *s, int i, ug_pixel *p)
{
    int per_edge[UG_ZONES], e = 0;
    per_edge[0] = s->front; per_edge[1] = s->side; per_edge[2] = s->rear; per_edge[3] = s->side;
    while (e < UG_ZONES - 1 && i >= per_edge[e]) i -= per_edge[e++];
    p->zone = e;
    p->ln = per_edge[e] > 0 ? per_edge[e] : 1;
    p->li = e == UG_ZONE_REAR || e == UG_ZONE_LEFT ? p->ln - 1 - i : i;
    p->t = ((float)p->li + 0.5f) / (float)p->ln;
    p->lateral = e == UG_ZONE_RIGHT ? 1.0f : e == UG_ZONE_LEFT ? -1.0f : 2.0f * p->t - 1.0f;
    p->x = p->lateral;
    p->y = e == UG_ZONE_FRONT ? 1.0f : e == UG_ZONE_REAR ? -1.0f : 1.0f - 2.0f * p->t;
    p->ang = (float)(atan2((double)p->x, (double)p->y) / 6.283185307179586);
    if (p->ang < 0.0f) p->ang += 1.0f;
    p->rd = sqrtf(0.16f * p->x * p->x + p->y * p->y) / 1.0770330f;
    if (p->rd > 1.0f) p->rd = 1.0f;
    p->gi = 0; p->gn = 1; p->ring = 0.5f; p->id = 0;
}

/* The ring's pixels for the parts that are lit (bit per zone): the zones that play the same effect count as one run round the car. */
static void ug_pixels(const ug_style *s, const unsigned char effect[UG_ZONES], unsigned parts, ug_pixel *px, int n)
{
    int count[UG_EFFECTS], i;
    for (i = 0; i < UG_EFFECTS; i++) count[i] = 0;
    for (i = 0; i < n; i++) {
        ug_zone_of(s, i, &px[i]);
        px[i].id = i;
        px[i].gi = (parts >> px[i].zone) & 1 ? count[effect[px[i].zone]]++ : 0;
    }
    for (i = 0; i < n; i++) {
        int gn = count[effect[px[i].zone]];
        px[i].gn = gn > 0 ? gn : 1;
        px[i].ring = ((float)px[i].gi + 0.5f) / (float)px[i].gn;
    }
}

static double ug_frac(double x) { return x - floor(x); }
static float ug_clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float ug_tri(double x) { return (float)(1.0 - fabs(2.0 * ug_frac(x) - 1.0)); }
static float ug_hash01(unsigned x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return (float)(x & 0xFFFFFF) / (float)0x1000000;
}

/* The music's level per zone (0..1) and a peak that holds 0.4 s, then falls. Underglow's made-up beat: the owner's radio is not shared. */
typedef struct { float lvl[UG_ZONES], peak[UG_ZONES]; unsigned long long peak_at[UG_ZONES], last; } ug_audio;
static void ug_audio_frame(ug_audio *a, float speed, double secs, unsigned long long now_ms)
{
    float dt = a->last ? (float)(now_ms - a->last) / 1000.0f : 0.016f; int z;
    a->last = now_ms;
    for (z = 0; z < UG_ZONES; z++) {
        double ab = secs * (1.0 + 6.0 * speed), zz = (double)z * 1.7, beat = pow(1.0 - ug_frac(ab * 0.5), 3.0);
        float lvl = ug_clamp01((float)(0.3 + 0.2 * sin(ab * 2.3 + zz) + 0.15 * sin(ab * 5.1 + 2.0 * zz) + 0.1 * sin(ab * 9.7 + 3.0 * zz) + 0.35 * beat));
        float *pk = &a->peak[z];
        a->lvl[z] = lvl;
        if (lvl >= *pk) { *pk = lvl; a->peak_at[z] = now_ms; }
        else if (now_ms - a->peak_at[z] > 400) { *pk -= 0.8f * dt; if (*pk < lvl) *pk = lvl; }
    }
}

static int ug_silent(float lvl) { return lvl <= 0.03f; }
static float ug_adist(float a, float b) { return fabsf((float)ug_frac((double)a - (double)b + 0.5) - 0.5f); }
static float ug_smooth01(float x) { x = ug_clamp01(x); return x * x * (3.0f - 2.0f * x); }
static void ug_mix(const float a[3], const float b[3], float t, float out[3]) { int k; for (k = 0; k < 3; k++) out[k] = a[k] + (b[k] - a[k]) * t; }
static void ug_copy3(float to[3], const float from[3]) { to[0] = from[0]; to[1] = from[1]; to[2] = from[2]; }
static float ug_dot(const ug_pixel *p, float pos, float width) { return ug_clamp01(1.0f - fabsf(((float)p->li + 0.5f) - pos * (float)p->ln) / width); }
static long long ug_mod(long long a, long long m) { return ((a % m) + m) % m; }

/* Light p's color and level (0..1 of the brightness) at `secs`, playing `effect`: Underglow's effect_now. */
static void ug_effect_now(const ug_style *c, const ug_audio *au, int effect, const ug_pixel *p, double secs, float rgb[3], float *level)
{
    const double phase = secs * c->speed, tau = 6.283185307179586;
    const float *base = c->rgb1, *other = c->rgb2;
    ug_copy3(rgb, base);
    *level = 1.0f;
    switch (effect) {
    case UG_RAINBOW: ug_hue_rgb((float)phase, rgb); break;
    case UG_WAVE: ug_hue_rgb((float)(phase + p->ring), rgb); break;
    case UG_BREATHE: *level = (float)(0.1 + 0.9 * (0.5 - 0.5 * cos(phase * tau))); break;
    case UG_WIPE: {
        int step = (int)(ug_frac(phase) * 2.0 * p->ln);
        *level = step < p->ln ? (p->li <= step ? 1.0f : 0.0f) : (p->li > step - p->ln ? 1.0f : 0.0f);
        break;
    }
    case UG_STACK: {
        int n = p->ln, moves = n * (n + 1) / 2, hold = n > 2 ? n : 2, stacked = 0;
        int step = (int)(ug_frac(phase * 0.5) * (double)(moves + hold));
        while (stacked < n && step >= n - stacked) step -= n - stacked++;
        *level = p->li >= n - stacked || (stacked < n && p->li == step) ? 1.0f : 0.0f;
        break;
    }
    case UG_BLINK: *level = ug_frac(phase * 5.0) < 0.5 ? 1.0f : 0.0f; break;
    case UG_POLICE: {
        double q = ug_frac(phase * 3.0), h = ug_frac(q * 2.0);
        int left = p->lateral <= 0.0f, turn = left == (q < 0.5);
        if (left) { rgb[0] = 1.0f; rgb[1] = 0.0f; rgb[2] = 0.0f; } else { rgb[0] = 0.0f; rgb[1] = 0.15f; rgb[2] = 1.0f; }
        *level = turn && (h < 0.2 || (h > 0.3 && h < 0.5)) ? 1.0f : 0.0f;
        break;
    }
    case UG_AUDIO: {
        float lvl = au->lvl[p->zone], d = fabsf(2.0f * p->t - 1.0f);
        ug_hue_rgb((1.0f - d) / 3.0f, rgb);
        *level = !ug_silent(lvl) ? ug_clamp01((lvl - d) * (float)p->ln * 0.5f + 1.0f) : 0.0f;
        break;
    }
    case UG_AUDIO_PULSE: { float lvl = au->lvl[p->zone]; *level = ug_silent(lvl) ? 0.0f : lvl; break; }
    case UG_AUDIO_PEAK: {
        float lvl = au->lvl[p->zone], pk = au->peak[p->zone]; int top;
        if (ug_silent(pk)) { *level = 0.0f; break; }
        top = (int)(pk * (float)p->ln);
        if (top > p->ln - 1) top = p->ln - 1;
        if (p->li == top) { ug_copy3(rgb, other); *level = 1.0f; }
        else *level = ug_silent(lvl) ? 0.0f : ug_clamp01(lvl * (float)p->ln - (float)p->li);
        break;
    }
    case UG_AUDIO_BLEND: { float lvl = au->lvl[p->zone]; ug_mix(base, other, lvl, rgb); *level = ug_silent(lvl) ? 0.0f : 0.2f + 0.8f * lvl; break; }
    case UG_BASS_FLASH: { float bass = au->lvl[UG_ZONE_REAR]; if (bass > 0.85f) ug_copy3(rgb, other); *level = ug_silent(bass) ? 0.0f : bass * bass; break; }
    case UG_AUDIO_SPARKLE: {
        float lvl = au->lvl[p->zone]; double slots = secs * 12.0; unsigned slot = (unsigned)(long long)slots;
        float f = (float)ug_frac(slots), r = ug_hash01(slot * 131u + (unsigned)p->gi * 7u + (unsigned)p->zone);
        *level = !ug_silent(lvl) && r < lvl * 0.7f ? (1.0f - f) * (1.0f - f) : 0.0f;
        break;
    }
    case UG_AUDIO_CENTER: {
        float lvl = au->lvl[p->zone], d = fabsf(2.0f * p->t - 1.0f);
        ug_mix(base, other, d, rgb);
        *level = !ug_silent(lvl) ? ug_clamp01((lvl - d) * (float)p->ln * 0.5f + 1.0f) : 0.0f;
        break;
    }
    case UG_AUDIO_RAINBOW: {
        float lvl = au->lvl[p->zone];
        ug_hue_rgb((float)(phase + p->ring) + 0.3f * lvl, rgb);
        *level = ug_silent(lvl) ? 0.0f : 0.15f + 0.85f * lvl;
        break;
    }
    case UG_CROSSING: {
        float pos = ug_tri(phase * 1.5), a = ug_dot(p, pos, 1.5f), b = ug_dot(p, 1.0f - pos, 1.5f), sum = a + b > 0.0f ? a + b : 1.0f; int k;
        for (k = 0; k < 3; k++) rgb[k] = (base[k] * a + other[k] * b) / sum;
        *level = a > b ? a : b;
        break;
    }
    case UG_HYPNOTOAD: {
        float d = fabsf(2.0f * p->t - 1.0f);
        ug_hue_rgb((float)(d * 1.5 - phase * 2.0), rgb);
        *level = (float)(0.25 + 0.75 * (0.5 + 0.5 * sin(phase * tau * 3.0 - d * 6.0)));
        break;
    }
    case UG_LIGHTNING: {
        double slots = secs * (0.5 + 2.0 * c->speed), into = ug_frac(slots) / (0.5 + 2.0 * c->speed), start;
        unsigned slot = (unsigned)(long long)slots * 4u + (unsigned)p->zone; int flashes, from, len, f;
        *level = 0.0f;
        if (ug_hash01(slot * 7u + 1u) > 0.55f) break;
        start = ug_hash01(slot * 7u + 2u) * 0.4;
        flashes = 1 + (int)(ug_hash01(slot * 7u + 3u) * 3.0f);
        from = (int)(ug_hash01(slot * 7u + 4u) * (float)p->ln);
        len = 1 + (int)(ug_hash01(slot * 7u + 5u) * (float)p->ln);
        if (p->li < from || p->li >= from + len) break;
        for (f = 0; f < flashes; f++) { double t0 = start + f * 0.1; if (into >= t0 && into < t0 + 0.05) *level = f == 0 ? 1.0f : 0.6f; }
        break;
    }
    case UG_MARQUEE: {
        long long step = (long long)(phase * 20.0), k = (long long)p->gi - step, m = ug_mod(k, 3), group = (k - m) / 3;
        ug_hue_rgb(ug_hash01((unsigned)group), rgb);
        *level = m == 0 ? 1.0f : 0.0f;
        break;
    }
    case UG_COMET: {
        double head = ug_frac(phase * 1.5), behind = ug_frac(head - p->ring), tail = fmax(0.3, 4.0 / (double)p->gn);
        *level = behind < tail ? (float)((1.0 - behind / tail) * (1.0 - behind / tail)) : 0.0f;
        break;
    }
    case UG_FILL: { float lvl = ug_tri(phase); *level = ug_clamp01(lvl * (float)p->ln - (float)p->li); break; }
    case UG_SWAP: { int first = p->t < 0.5f, swapped = (long long)(phase * 2.0) % 2 != 0; if (first == swapped) ug_copy3(rgb, other); break; }
    case UG_VISOR: {
        float pos = (float)(0.5 - 0.5 * cos(phase * 1.5 * tau)), w = fmaxf(1.5f, (float)p->ln * 0.2f), v = ug_dot(p, pos, w);
        *level = v * v;
        break;
    }
    case UG_AUDIO_BUBBLES: {
        float lvl = au->lvl[p->zone]; double slots; long long now, k;
        *level = 0.0f;
        if (ug_silent(lvl)) break;
        slots = secs * 5.0; now = (long long)floor(slots);
        for (k = now; k > now - 4; --k) {
            unsigned id = (unsigned)k; float age = (float)((slots - (double)k) / 4.0), r = 0.02f + age * 0.15f * (0.5f + lvl);
            float v = ug_clamp01(1.0f - fabsf(ug_adist(p->ang, ug_hash01(id * 13u + 1u)) - r) / 0.035f) * (1.0f - age);
            if (v > *level) { *level = v; ug_hue_rgb(ug_hash01(id * 13u + 2u), rgb); }
        }
        *level = ug_clamp01(*level * (0.4f + lvl));
        break;
    }
    case UG_AUDIO_PARTY: {
        float lvl = au->lvl[p->zone];
        ug_hue_rgb(ug_hash01((unsigned)(long long)(secs * 3.0) * 4u + (unsigned)p->zone), rgb);
        *level = ug_silent(lvl) ? 0.0f : lvl * lvl;
        break;
    }
    case UG_AUDIO_SINE: {
        float lvl = au->lvl[p->zone], w = (float)(0.5 + 0.5 * sin((p->t * 2.0 - phase * 2.0) * tau));
        *level = ug_silent(lvl) ? 0.0f : ug_clamp01(w * lvl * 1.6f);
        break;
    }
    case UG_AUDIO_STAR: {
        float lvl = au->lvl[p->zone]; double slots; long long now, k;
        *level = 0.0f;
        if (ug_silent(lvl)) break;
        slots = secs * 4.0; now = (long long)floor(slots);
        for (k = now; k > now - 2; --k) {
            unsigned h = (unsigned)k * 97u + (unsigned)p->id * 31u; float v;
            if (ug_hash01(h + 5u) >= lvl * 0.5f) continue;
            v = 1.0f - (float)((slots - (double)k) / 2.0);
            if (v > *level) { *level = v; ug_hue_rgb(ug_hash01(h + 6u), rgb); }
        }
        break;
    }
    case UG_AUDIO_SYNC: {
        float b = au->lvl[UG_ZONE_REAR], lm = au->lvl[UG_ZONE_LEFT], hm = au->lvl[UG_ZONE_RIGHT], tr = au->lvl[UG_ZONE_FRONT];
        float sum = b + lm + hm + tr, loud = fmaxf(fmaxf(b, lm), fmaxf(hm, tr));
        ug_hue_rgb(sum > 0.0f ? (lm * 0.15f + hm * 0.4f + tr * 0.65f) / sum : 0.0f, rgb);
        *level = ug_silent(loud) ? 0.0f : loud;
        break;
    }
    case UG_VU_METER: {
        float lvl = au->lvl[p->zone];
        ug_mix(base, other, p->t, rgb);
        *level = ug_silent(lvl) ? 0.0f : ug_clamp01(lvl * (float)p->ln - (float)p->li);
        break;
    }
    case UG_AUDIO_SWIRL: {
        float loud = (au->lvl[0] + au->lvl[1] + au->lvl[2] + au->lvl[3]) * 0.25f;
        float a = (float)ug_frac(phase * 0.7), b = (float)ug_frac(phase * 0.7 + 0.5), w = 0.04f + 0.2f * loud;
        float va = ug_clamp01(1.0f - ug_adist(p->ang, a) / w), vb = ug_clamp01(1.0f - ug_adist(p->ang, b) / w);
        ug_copy3(rgb, va >= vb ? base : other);
        *level = ug_silent(loud) ? 0.0f : fmaxf(va, vb);
        break;
    }
    case UG_BOUNCING_BALL: { double u = ug_frac(phase * 1.5); *level = ug_dot(p, (float)(4.0 * u * (1.0 - u)), 1.3f); break; }
    case UG_BREATHING_CIRCLE: { float r = (float)(0.5 - 0.5 * cos(phase * tau)); *level = ug_clamp01(1.0f - fabsf(p->rd - r) / 0.25f); break; }
    case UG_FRACTAL_MOTION: {
        double a = p->ang * tau, q = phase * 3.0;
        double v = sin(a * 2.0 + q * 1.1) * 0.5 + sin(a * 5.0 - q * 1.7) * 0.3 + sin(a * 9.0 + q * 2.3) * 0.2;
        *level = ug_clamp01((float)(0.5 + 0.6 * v));
        break;
    }
    case UG_MOTION_POINT: { float pos = (float)(0.5 - 0.5 * cos(phase * 0.5 * tau)); *level = ug_clamp01(1.0f - ug_adist(p->ang, pos) / 0.05f); break; }
    case UG_RAIN: {
        unsigned k;
        *level = 0.0f;
        for (k = 0; k < 3; k++) {
            unsigned d = (unsigned)p->zone * 3u + k;
            double run = phase * (1.5 + ug_hash01(d * 7u + 1u)) + ug_hash01(d * 7u + 2u);
            float behind = (float)ug_frac(run) * 1.3f - p->t;
            if (behind >= 0.0f && behind < 0.25f) *level = fmaxf(*level, 1.0f - behind / 0.25f);
        }
        break;
    }
    case UG_ROTATING_BEAM: {
        float a = (float)ug_frac(phase * 0.5);
        *level = ug_clamp01(1.0f - fminf(ug_adist(p->ang, a), ug_adist(p->ang, a + 0.5f)) / 0.07f);
        break;
    }
    case UG_SMOOTH_BLINK: {
        float u = (float)ug_frac(phase * 2.0);
        *level = u < 0.4f ? 1.0f : u < 0.5f ? ug_smooth01((0.5f - u) * 10.0f) : u < 0.9f ? 0.0f : ug_smooth01((u - 0.9f) * 10.0f);
        break;
    }
    case UG_SPARKLE_FADE: {
        double slots = phase * 8.0 + ug_hash01((unsigned)p->id * 3u + 1u); float f = (float)ug_frac(slots);
        *level = ug_hash01((unsigned)(long long)slots * 131u + (unsigned)p->id * 17u) < 0.3f ? (1.0f - f) * (1.0f - f) : 0.0f;
        break;
    }
    case UG_ZIGZAG: {
        double u = ug_frac(phase * 0.5); float py = (float)(1.0 - 2.0 * u), qx = 2.0f * ug_tri(u * 3.0) - 1.0f;
        float d = sqrtf((p->x - qx) * (p->x - qx) + (p->y - py) * (p->y - py));
        *level = ug_clamp01(1.0f - d / 0.35f);
        break;
    }
    case UG_MARQUEE1: { long long k = (long long)p->gi - (long long)(phase * 20.0); *level = ug_mod(k, 3) == 0 ? 1.0f : 0.0f; break; }
    case UG_ALTERNATE_BLINK: { double u = ug_frac(phase * 2.5); if (u >= 0.5) ug_copy3(rgb, other); *level = ug_frac(u * 2.0) < 0.6 ? 1.0f : 0.0f; break; }
    case UG_GRADIENT_WAVE: ug_mix(base, other, (float)(0.5 - 0.5 * cos((p->ring - phase) * tau)), rgb); break;
    case UG_MOVING_PANES: {
        double dir = p->zone == UG_ZONE_FRONT || p->zone == UG_ZONE_RIGHT ? 1.0 : -1.0;
        long long pane = (long long)floor(p->t * 4.0 + dir * phase * 2.0);
        if (ug_mod(pane, 2)) ug_copy3(rgb, other);
        break;
    }
    case UG_NOISE_MAP: {
        double a = p->ang * tau, q = phase * 3.0, n = sin(a * 2.0 + q) * 0.6 + sin(a * 3.0 - q * 0.7 + 1.3) * 0.4;
        ug_mix(base, other, ug_smooth01((float)(0.5 + 0.5 * n)), rgb);
        break;
    }
    case UG_SEQUENCE: {
        float u = (float)ug_frac(phase);
        float m = u < 0.4f ? 0.0f : u < 0.5f ? ug_smooth01((u - 0.4f) * 10.0f) : u < 0.9f ? 1.0f : ug_smooth01((1.0f - u) * 10.0f);
        ug_mix(base, other, m, rgb);
        break;
    }
    case UG_WAVY: { double w = sin(p->t * 2.0 * tau) * sin(phase * tau); ug_mix(base, other, (float)(0.5 + 0.5 * w), rgb); break; }
    case UG_TWO_COLOR_MARQUEE: {
        long long k = (long long)p->gi - (long long)(phase * 20.0), g = (long long)floor((double)k / 3.0);
        if (g % 2 != 0) ug_copy3(rgb, other);
        *level = ug_mod(k, 3) != 2 ? 1.0f : 0.0f;
        break;
    }
    case UG_BLOOM:
    case UG_BUBBLES: {
        int ring = effect == UG_BUBBLES; double slots = phase * 3.0; long long now = (long long)floor(slots), k;
        *level = 0.0f;
        for (k = now; k > now - 3; --k) {
            unsigned id = (unsigned)k * (ring ? 7u : 11u);
            float age = (float)((slots - (double)k) / 3.0), d = ug_adist(p->ang, ug_hash01(id + 1u)), r = 0.02f + age * 0.15f;
            float v = ring ? ug_clamp01(1.0f - fabsf(d - r) / 0.03f) * (1.0f - age) : d < r ? (1.0f - age) * (1.0f - 0.5f * d / r) : 0.0f;
            if (v > *level) { *level = v; ug_hue_rgb(ug_hash01(id + 2u) + (ring ? 0.0f : d * 2.0f), rgb); }
        }
        break;
    }
    case UG_COLOR_WHEEL: ug_hue_rgb(p->ang + (float)phase, rgb); break;
    case UG_ROTATING_RAINBOW: { double a = phase * 0.5 * tau; ug_hue_rgb((float)((p->x * cos(a) + p->y * sin(a)) * 0.35), rgb); break; }
    case UG_DOUBLE_RAINBOW: ug_hue_rgb((p->x >= 0.0f ? 2.0f * p->ang : -2.0f * p->ang) + (float)phase, rgb); break;
    case UG_RADIAL_RAINBOW: ug_hue_rgb(p->rd - (float)phase, rgb); break;
    case UG_SPIRAL: ug_hue_rgb(p->ang + p->rd * 1.5f - (float)phase, rgb); break;
    case UG_RANDOM_SPIN: {
        unsigned k = (unsigned)(long long)(phase * 0.8) * 4u + (unsigned)p->zone;
        ug_hue_rgb(p->t + (float)(phase * (ug_hash01(k * 3u + 1u) * 2.0f - 1.0f) * 3.0), rgb);
        break;
    }
    case UG_SWIRL_CIRCLES: {
        float a = (float)ug_frac(phase * 0.6), b = (float)ug_frac(phase * 0.6 + 0.5);
        float va = ug_clamp01(1.0f - ug_adist(p->ang, a) / 0.2f), vb = ug_clamp01(1.0f - ug_adist(p->ang, b) / 0.2f);
        ug_hue_rgb((float)(phase * 0.15) + (va >= vb ? 0.0f : 0.5f), rgb);
        *level = fmaxf(va, vb);
        break;
    }
    case UG_MOTION_POINTS: {
        unsigned k;
        *level = 0.0f;
        for (k = 0; k < 5; k++) {
            float sp = (0.3f + 0.7f * ug_hash01(k * 5u + 1u)) * (ug_hash01(k * 5u + 4u) < 0.5f ? -1.0f : 1.0f);
            float v = ug_clamp01(1.0f - ug_adist(p->ang, (float)ug_frac(phase * sp + ug_hash01(k * 5u + 2u))) / 0.04f);
            if (v > *level) { *level = v; ug_hue_rgb(ug_hash01(k * 5u + 3u), rgb); }
        }
        break;
    }
    case UG_MOSAIC: {
        unsigned tile = (unsigned)(p->id / 2); double run = phase * 1.5 + ug_hash01(tile * 7u + 1u) * 4.0;
        ug_hue_rgb(ug_hash01((unsigned)(long long)run * 101u + tile * 7u), rgb);
        *level = (float)sin(ug_frac(run) * 3.141592653589793);
        break;
    }
    case UG_STARRY_NIGHT: {
        double run = phase * 1.2 + ug_hash01((unsigned)p->id * 9u + 1u) * 7.0; unsigned h = (unsigned)(long long)run * 61u + (unsigned)p->id * 13u;
        ug_hue_rgb(ug_hash01(h + 2u), rgb);
        *level = ug_hash01(h) < 0.45f ? (float)sin(ug_frac(run) * 3.141592653589793) : 0.0f;
        break;
    }
    case UG_SUNRISE: {
        float u = (float)ug_frac(phase * 0.25), glow = ug_clamp01(u * 1.6f - p->rd * 0.6f), white; int k;
        ug_hue_rgb(0.0f + 0.13f * glow, rgb);
        white = ug_clamp01((glow - 0.7f) / 0.3f) * 0.7f;
        for (k = 0; k < 3; k++) rgb[k] += (1.0f - rgb[k]) * white;
        *level = u > 0.92f ? glow * (1.0f - u) / 0.08f : glow;
        break;
    }
    default: break;
    }
}
#endif
