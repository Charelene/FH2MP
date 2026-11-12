/* The local player's underglow, from FH2-Underglow's own `status` command (qiangqiang101/FH2-Underglow 1.1.0, cmd_status in src/neon_mod.cpp, MIT):
 * the platform's command pipe answers it, and its text is turned into the 28-byte state of mods_wire.h. Underglow publishes no interface and its
 * settings live in its own memory, so the command, a documented way in, is what the shim reads. A reply that does not have the lines of 1.1.0's
 * status is refused with the reason (another Underglow version prints something else); the player's underglow is then not shared.
 *
 * What the reply says, line by line (1.1.0's printf):
 *   neon on|off, primary color NAME (r g b), secondary NAME, brightness B, effects: front E, right E, rear E, left E, at S a second; parts on: front right rear left
 *   shape: Side n Front n Rear n Height h FrontInset f RearInset r SideOffset o SideTrim t Range .. Power .. Inner .. Outer ..
 *   car ID: half width W (from the game's data|from its lights), front F, rear R (from the game's data|from its lights)
 *   neon: N light(s), lit|dark
 *   Underglow Kit: not in the garage ...  |  Underglow Kit on the driven car (garage id G, car C): LEVEL, required|not required ...
 * The state is what Underglow draws (neon_frame): the parts switched on that the car's kit has, the kit level's LEDs, the light rectangle inset as
 * edge_point insets it. Include after mods_wire.h; no game or OS dependencies. */
#ifndef FH2MP_UG_STATUS_H
#define FH2MP_UG_STATUS_H
#include "ug_effects.h"

#define UG_COLORS 18
#define UG_CUSTOMS 10
static const struct { const char *name; float rgb[3]; } ug_colors[UG_COLORS] = {
    {"Cyan", {0.0f, 1.0f, 1.0f}},    {"Blue", {0.0f, 0.25f, 1.0f}},   {"Purple", {0.6f, 0.0f, 1.0f}},
    {"Pink", {1.0f, 0.1f, 0.6f}},    {"Red", {1.0f, 0.0f, 0.0f}},     {"Orange", {1.0f, 0.35f, 0.0f}},
    {"Yellow", {1.0f, 0.85f, 0.0f}}, {"Green", {0.0f, 1.0f, 0.15f}},  {"White", {1.0f, 1.0f, 1.0f}},
    {"Lime", {0.55f, 1.0f, 0.0f}},   {"Teal", {0.0f, 0.75f, 0.6f}},   {"Sky Blue", {0.3f, 0.7f, 1.0f}},
    {"Indigo", {0.3f, 0.0f, 1.0f}},  {"Magenta", {1.0f, 0.0f, 1.0f}}, {"Crimson", {1.0f, 0.0f, 0.25f}},
    {"Amber", {1.0f, 0.6f, 0.0f}},   {"Mint", {0.4f, 1.0f, 0.6f}},    {"Lavender", {0.7f, 0.5f, 1.0f}},
};
static const float ug_custom_defaults[UG_CUSTOMS][3] = {
    {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.5f}, {1.0f, 0.8f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.5f, 1.0f, 0.5f},
    {0.5f, 1.0f, 1.0f}, {0.5f, 0.6f, 1.0f}, {0.8f, 0.5f, 1.0f}, {1.0f, 0.5f, 1.0f}, {1.0f, 0.9f, 0.8f},
};
static const char *const ug_kit_names[8] = {"Stock", "Front", "Back", "Sides", "Front and Back", "Front and Sides", "Back and Sides", "Front, Back and Sides"};
static const unsigned char ug_kit_parts[8] = {0, 1, 2, 4, 1 | 2, 1 | 4, 2 | 4, 1 | 2 | 4};   /* Underglow's kFront 1, kBack 2, kSides 4 */

/* The ini's custom colors ([CustomColors] Color1..10 and Name1..10), which the secondary color may be one of. */
typedef struct { float rgb[UG_CUSTOMS][3]; char name[UG_CUSTOMS][32]; } ug_customs;

static unsigned ug_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int ug_starts(const char *s, const char *prefix) { while (*prefix) if (*s++ != *prefix++) return 0; return 1; }
static const char *ug_find(const char *s, const char *what)
{
    for (; *s; s++) if (ug_starts(s, what)) return s;
    return 0;
}
static int ug_same(const char *a, unsigned n, const char *b) { unsigned i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 0; return b[n] == 0; }
static int ug_hexdigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* A number as printf's %g, %f or %d prints it (and as a person writes one in the ini): -> where it ends, or 0. */
static const char *ug_number(const char *s, double *out)
{
    double v = 0, scale = 1; int neg = 0, digits = 0, ex = 0, exneg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); digits++; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); scale *= 10; digits++; } }
    if (!digits) return 0;
    v /= scale;
    if ((*s == 'e' || *s == 'E') && ((s[1] >= '0' && s[1] <= '9') || ((s[1] == '-' || s[1] == '+') && s[2] >= '0' && s[2] <= '9'))) {
        s++;
        if (*s == '-' || *s == '+') exneg = *s++ == '-';
        while (*s >= '0' && *s <= '9') ex = ex * 10 + (*s++ - '0');
        while (ex-- > 0) v = exneg ? v / 10 : v * 10;
    }
    *out = neg ? -v : v;
    return s;
}
/* `key` then a number, searched from s: -> the number's end, or 0 */
static const char *ug_field(const char *s, const char *key, double *out)
{
    const char *at = s ? ug_find(s, key) : 0;
    return at ? ug_number(at + ug_len(key), out) : 0;
}

/* A color as Underglow's parse_color takes it: "0 1 1" (0 to 1), "255 105 180" (any number above 1: 0 to 255), "#FF69B4" or "FF69B4". */
static int ug_parse_color(const char *text, float out[3])
{
    const char *p = text, *hex; double v[3]; unsigned n = 0; int i, bytes = 0;
    while (*p == ' ' || *p == '\t') p++;
    hex = *p == '#' ? p + 1 : p;
    while (ug_hexdigit(hex[n]) >= 0) n++;
    if (n == 6) {
        const char *after = hex + 6; int letters = 0, space = 0; const char *q;
        while (*after == ' ' || *after == '\t') after++;
        for (q = hex; q < hex + 6; q++) letters |= (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F');
        for (q = p; *q; q++) space |= *q == ' ' || *q == ',' || *q == '\t';
        if (!*after && (*p == '#' || letters || !space)) {
            for (i = 0; i < 3; i++) out[i] = (float)(ug_hexdigit(hex[2 * i]) * 16 + ug_hexdigit(hex[2 * i + 1])) / 255.0f;
            return 1;
        }
    }
    for (i = 0; i < 3; i++) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!(p = ug_number(p, &v[i]))) return 0;
        bytes |= v[i] > 1.0;
    }
    for (i = 0; i < 3; i++) out[i] = ug_clamp01((float)(bytes ? v[i] / 255.0 : v[i]));
    return 1;
}

/* The secondary color by the name status gives it: a preset, "Custom N", or a custom color's own name. */
static int ug_color_named(const char *name, unsigned n, const ug_customs *cc, float out[3])
{
    unsigned i;
    for (i = 0; i < UG_COLORS; i++) if (ug_same(name, n, ug_colors[i].name)) { ug_copy3(out, ug_colors[i].rgb); return 1; }
    for (i = 0; i < UG_CUSTOMS; i++) if (cc->name[i][0] && ug_same(name, n, cc->name[i])) { ug_copy3(out, cc->rgb[i]); return 1; }
    if (n >= 8 && n <= 9 && ug_starts(name, "Custom ")) {
        unsigned k = (unsigned)(name[7] - '0');
        if (n == 9) k = k * 10 + (unsigned)(name[8] - '0');
        if (k >= 1 && k <= UG_CUSTOMS) { ug_copy3(out, cc->rgb[k - 1]); return 1; }
    }
    return 0;
}

/* The effect named name[0..n), -1 when none */
static int ug_effect_named(const char *name, unsigned n)
{
    int i;
    for (i = 0; i < UG_EFFECTS; i++) if (ug_same(name, n, ug_effect_names[i])) return i;
    return -1;
}

static unsigned char ug_byte(double v, double scale) { v = v * scale + 0.5; return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void ug_put16(unsigned char *p, int v) { if (v > 32767) v = 32767; if (v < -32768) v = -32768; p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff); }
static int ug_mm(double m) { return (int)(m * 1000.0 + (m < 0 ? -0.5 : 0.5)); }

/* Underglow's scaled_leds and clamp_shape: Lv2 has 1.5 times Lv1's LEDs, Lv3 twice, at most UG_MAX_NEON in all (the longest edges lose first). */
static void ug_scale_shape(int *side, int *front, int *rear, int leds)
{
    static const float scale[4] = {1.0f, 1.0f, 1.5f, 2.0f};
    int *edge[3]; int k;
    if (leds > 1) {
        float f = scale[leds > 3 ? 3 : leds];
        *side = (int)((float)*side * f + 0.5f); *front = (int)((float)*front * f + 0.5f); *rear = (int)((float)*rear * f + 0.5f);
    }
    edge[0] = side; edge[1] = front; edge[2] = rear;
    for (k = 0; k < 3; k++) { if (*edge[k] < 0) *edge[k] = 0; if (*edge[k] > UG_MAX_NEON) *edge[k] = UG_MAX_NEON; }
    while (2 * *side + *front + *rear > UG_MAX_NEON) {
        if (*side * 2 >= *front && *side * 2 >= *rear && *side) --*side;
        else if (*front >= *rear && *front) --*front;
        else --*rear;
    }
}

/* -> 1 with the wire state of the car being driven, 0 with *why when the reply is not 1.1.0's status. */
static int ug_status_to_wire(const char *reply, const ug_customs *cc, unsigned char ug[28], const char **why)
{
    const char *p, *q, *line2, *line3, *line4, *line5, *e[UG_ZONES + 1];
    static const char *const zone_keys[UG_ZONES] = {"front ", "right ", "rear ", "left "};
    double r1[3], bright, speed, side, front, rear, height, finset, rinset, soffset, strim, hw, zf, zr;
    float rgb2[3];
    int neon, lit, effect[UG_ZONES], k, kit_gates = 0, leds = 1, iside, ifront, irear, db_length;
    unsigned switches = 0, parts, kitp = 7;
    for (k = 0; k < 28; k++) ug[k] = 0;
    *why = "no reply";
    if (!reply || !(ug_starts(reply, "neon on,") || ug_starts(reply, "neon off,"))) { *why = "the reply does not start with \"neon on,\" or \"neon off,\""; return 0; }
    neon = ug_starts(reply, "neon on,");
    if (!(line2 = ug_find(reply, "\nshape: ")) || !(line3 = ug_find(line2 + 1, "\ncar ")) || !(line4 = ug_find(line3 + 1, "\nneon: ")) ||
        !(line5 = ug_find(line4 + 1, "\nUnderglow Kit"))) { *why = "the shape, car, neon or kit line is missing"; return 0; }
    /* line 1: the primary color's rgb, the secondary's name, brightness, the four effects, the speed, the parts switched on */
    if (!(p = ug_find(reply, ", primary color ")) || !(p = ug_find(p, " (")) || !(p = ug_number(p + 2, &r1[0])) || *p != ' ' ||
        !(p = ug_number(p + 1, &r1[1])) || *p != ' ' || !(p = ug_number(p + 1, &r1[2])) || !ug_starts(p, "), secondary ")) {
        *why = "the primary color is not \"NAME (r g b)\""; return 0;
    }
    p += 13;
    if (!(q = ug_find(p, ", brightness ")) || q > line2 || !ug_color_named(p, (unsigned)(q - p), cc, rgb2)) { *why = "the secondary color's name is not a known color"; return 0; }
    if (!(p = ug_number(q + 13, &bright)) || !ug_starts(p, ", effects: ")) { *why = "no brightness"; return 0; }
    p += 11;
    for (k = 0; k < UG_ZONES; k++) {
        if (!ug_starts(p, zone_keys[k])) { *why = "the effects are not front, right, rear, left"; return 0; }
        e[k] = p + ug_len(zone_keys[k]);
        if (!(q = ug_find(e[k], k < UG_ZONES - 1 ? ", " : ", at ")) || q > line2) { *why = "an effect name does not end"; return 0; }
        if ((effect[k] = ug_effect_named(e[k], (unsigned)(q - e[k]))) < 0) { *why = "an effect name is not one of 1.1.0's"; return 0; }
        p = q + 2;
    }
    if (!ug_starts(p, "at ") || !(p = ug_number(p + 3, &speed)) || !ug_starts(p, " a second; parts on: ")) { *why = "no effect speed"; return 0; }
    for (p += 21; *p && *p != '\n'; ) {
        for (k = 0; k < UG_ZONES; k++) if (ug_starts(p, zone_keys[k]) || (k == UG_ZONE_LEFT && ug_starts(p, "left"))) break;
        if (k == UG_ZONES) { *why = "a part name is not front, right, rear or left"; return 0; }
        switches |= 1u << k;
        p += k == UG_ZONE_LEFT ? 4 : ug_len(zone_keys[k]);
    }
    /* line 2: the shape (Lv1's LEDs) and the insets */
    if (!ug_field(line2, "Side ", &side) || !ug_field(line2, " Front ", &front) || !ug_field(line2, " Rear ", &rear) ||
        !ug_field(line2, " Height ", &height) || !ug_field(line2, " FrontInset ", &finset) || !ug_field(line2, " RearInset ", &rinset) ||
        !ug_field(line2, " SideOffset ", &soffset) || !ug_field(line2, " SideTrim ", &strim)) { *why = "the shape line lacks a field"; return 0; }
    /* line 3: the car's lights rectangle; "from the game's data" for its length means no insets */
    if (!(p = ug_field(line3, ": half width ", &hw)) || !(p = ug_find(p, ", front ")) || !(p = ug_number(p + 8, &zf)) ||
        !ug_starts(p, ", rear ") || !(p = ug_number(p + 7, &zr))) { *why = "the car line lacks the half width, front or rear"; return 0; }
    db_length = ug_starts(p, " (from the game's data)");
    /* line 4: lit or dark now */
    q = ug_find(line4, "light(s), ");
    if (!q || q > line5) { *why = "the neon line lacks \"light(s), lit|dark\""; return 0; }
    lit = ug_starts(q + 10, "lit");
    /* line 5: the kit decides the parts and the LED level when it is required */
    if (ug_starts(line5, "\nUnderglow Kit on the driven car (")) {
        const char *level = ug_find(line5, "): ");
        if (!level) { *why = "the kit line lacks the kit"; return 0; }
        level += 3;
        if (ug_find(level, ", required")) {   /* ", not required (Require Underglow Kit is off)" has no ", required" */
            kit_gates = 1;
            if (ug_starts(level, "Stock")) kitp = 0;
            else {
                const char *lv = ug_find(level, " Lv");
                int found = -1;
                if (!lv || lv[3] < '1' || lv[3] > '3') { *why = "the kit's level is not Stock or \"<kit> Lv1..3\""; return 0; }
                for (k = 1; k < 8; k++) if (ug_same(level, (unsigned)(lv - level), ug_kit_names[k])) found = k;
                if (found < 0) { *why = "the kit's name is not one of 1.1.0's"; return 0; }
                kitp = ug_kit_parts[found];
                leds = lv[3] - '0';
            }
        }
    }
    parts = switches & ((kitp & 1 ? 1u << UG_ZONE_FRONT : 0) | (kitp & 2 ? 1u << UG_ZONE_REAR : 0) | (kitp & 4 ? (1u << UG_ZONE_RIGHT) | (1u << UG_ZONE_LEFT) : 0));
    if (!neon || !lit || !parts) { *why = 0; return 1; }   /* dark: the all-zero state */
    iside = (int)side; ifront = (int)front; irear = (int)rear;
    ug_scale_shape(&iside, &ifront, &irear, kit_gates ? leds : 1);
    hw += soffset;
    if (hw < 0.1) hw = 0.1;
    if (!db_length) { zf -= finset; zr += rinset; }
    if (zf < zr + 0.2) zf = zr = (zf + zr) * 0.5;
    if (hw > MODS_UG_WIDTH_MAX / 1000.0) hw = MODS_UG_WIDTH_MAX / 1000.0;
    if (zf > MODS_UG_LENGTH_MAX / 1000.0) zf = MODS_UG_LENGTH_MAX / 1000.0;
    if (zr < -MODS_UG_LENGTH_MAX / 1000.0) zr = -MODS_UG_LENGTH_MAX / 1000.0;
    if (zr > zf) zr = zf;
    ug[0] = 1; ug[1] = (unsigned char)parts; ug[2] = (unsigned char)iside; ug[3] = (unsigned char)ifront; ug[4] = (unsigned char)irear;
    for (k = 0; k < UG_ZONES; k++) ug[5 + k] = (unsigned char)effect[k];
    for (k = 0; k < 3; k++) { ug[9 + k] = ug_byte(r1[k], 255.0); ug[12 + k] = ug_byte(rgb2[k], 255.0); }
    ug[15] = ug_byte(bright, 64.0); ug[16] = ug_byte(speed, 50.0);
    ug_put16(ug + 17, ug_mm(hw)); ug_put16(ug + 19, ug_mm(zf)); ug_put16(ug + 21, ug_mm(zr));
    ug[23] = ug_byte(strim, 100.0);
    {
        int cm = (int)(height * 100.0 + (height < 0 ? -0.5 : 0.5));
        if (cm < -128) cm = -128;
        if (cm > 127) cm = 127;
        ug[24] = (unsigned char)(signed char)cm;
    }
    *why = 0;
    return 1;
}

/* The wire state as ug_effects.h plays it (remote cars). */
static void ug_style_of(const unsigned char ug[28], ug_style *s)
{
    int k;
    for (k = 0; k < 3; k++) { s->rgb1[k] = (float)ug[9 + k] / 255.0f; s->rgb2[k] = (float)ug[12 + k] / 255.0f; }
    s->speed = (float)ug[16] / 50.0f;
    s->side = ug[2]; s->front = ug[3]; s->rear = ug[4];
    s->half_width = (float)(ug[17] | ug[18] << 8) / 1000.0f;
    s->zf = (float)mods_s16(ug + 19) / 1000.0f;
    s->zr = (float)mods_s16(ug + 21) / 1000.0f;
    s->side_trim = (float)ug[23] / 100.0f;
    s->height = (float)(signed char)ug[24] / 100.0f;
}
#endif
