// The Guide's Achievements (Games > Achievements): the 360's achievement grid for one game (gp\802_Achievements.xur of 2.0.13604: the
// game's title, "%1!u! of %2!u! Achievements", a grid of tiles, and the focused achievement's name, date, points and description; only
// Back). FH2MP has one game, so the 360's list of played games before it (gp\820_GameShowcaseMe.xur) is not shown: the Gamerscore that
// list had at the right of each game stands beside the count here.
//
// What is unlocked comes from the Achievements mod (Trinculo54/fh2-achievements): it works the achievements out from the save and the
// game's own events and keeps them in fh2-mods\Achievements\User_<id>.ini ([unlocked] id=YYYY-MM-DD HH:MM:SS UTC). The Guide only reads
// that file. Without the mod there is nothing to show and the Achievements row stays greyed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "achievements.h"
#include "guide.h"
#include "ui.h"

using namespace ui;

namespace {

const std::vector<AchDef> DEFS = {
#include "ach_defs.inc"
};

const int COLS = 11, ROWS = 3;         // tiles across, rows on screen
const float TILE = 64, GAP = 8;        // canvas units
const uint32_t I_LOCK = 0xe897;        // Material Icons "lock"

// The Gamerscore mark: a ring with a G, d across, centred on (cx, cy).
void g_mark(ImDrawList* dl, const GuideAssets& a, float cx, float cy, float d, ImU32 col) {
    ImFont* f = a.text_bold ? a.text_bold : a.text;
    dl->AddCircle(ImVec2(cx, cy), d * 0.5f - d * 0.05f, col, 32, d * 0.1f);
    float size = d * 0.62f, w = f->CalcTextSizeA(size, FLT_MAX, 0, "G").x;
    dl->AddText(f, size, ImVec2(cx - w * 0.5f, caps_top(a.bold_m, size, cy)), col, "G");
}

// A number with the mark before it, ending at x (right aligned), centred on cy; returns where it starts.
float g_score(ImDrawList* dl, const GuideAssets& a, const View& v, float x, float cy, float size, ImU32 col, int points) {
    char num[16];
    snprintf(num, sizeof num, "%d", points);
    float w = a.text->CalcTextSizeA(v.sz(size), FLT_MAX, 0, num).x;
    text_mid(dl, a.text, a.text_m, v.sz(size), x, cy, col, num, true);
    float d = v.sz(size);
    g_mark(dl, a, x - w - v.sz(7) - d * 0.5f, cy, d, col);
    return x - w - v.sz(7) - d;
}

void tile(ImDrawList* dl, const GuideAssets& a, ImVec2 p0, ImVec2 p1, bool unlocked, float op) {
    dl->AddRectFilled(p0, p1, alpha(unlocked ? ACCENT : RAISED, op));
    float side = p1.x - p0.x;
    icon_mid(dl, a, side * 0.56f, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f, alpha(unlocked ? WHITE : DISABLED, op), unlocked ? I_TROPHY : I_LOCK);
}

}  // namespace

const std::vector<AchDef>& ach_defs() { return DEFS; }

int AchUnlocks::count() const {
    int n = 0;
    for (const AchDef& d : DEFS) n += has(d.id);
    return n;
}

int AchUnlocks::gamerscore() const {
    int g = 0;
    for (const AchDef& d : DEFS)
        if (has(d.id)) g += d.g;
    return g;
}

void ach_parse(const std::string& text, AchUnlocks* out) {
    out->when.clear();
    bool in = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            in = line == "[unlocked]";
            continue;
        }
        size_t eq = line.find('=');
        if (in && eq != std::string::npos && eq > 0) out->when[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

bool ach_refresh(const std::wstring& azimuth_dir, uint64_t* stamp, AchUnlocks* out) {
    std::wstring dir = azimuth_dir + L"\\fh2-mods\\Achievements\\";
    // the profile played last: the unlock file written last (the mod's .bak and .damaged copies are not unlock files)
    std::wstring newest;
    uint64_t newest_time = 0, cur = 1;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"User_*.ini").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name.size() < 5 || _wcsicmp(name.c_str() + name.size() - 4, L".ini") != 0) continue;
            uint64_t t = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            if (newest.empty() || t > newest_time) newest = name, newest_time = t, cur = t * 31 + fd.nFileSizeLow + 2;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (cur == *stamp) return false;
    AchUnlocks u;
    if (!newest.empty()) {
        // the mod replaces the file while the game runs: opened without keeping anyone out, and a failed read is tried again
        HANDLE h = CreateFileW((dir + newest).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::string text;
        char buf[8192];
        DWORD got = 0;
        while (text.size() < (1u << 20) && ReadFile(h, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
        CloseHandle(h);
        if (text.find("[unlocked]") == std::string::npos) return false;  // being written, or damaged: the mod repairs it
        ach_parse(text, &u);
        u.known = true;
    }
    *stamp = cur;
    *out = std::move(u);
    return true;
}

std::string ach_date(const std::string& when) {
    SYSTEMTIME utc{}, local{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(when.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return "";
    utc.wYear = WORD(y), utc.wMonth = WORD(mo), utc.wDay = WORD(d), utc.wHour = WORD(h), utc.wMinute = WORD(mi), utc.wSecond = WORD(s);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return "";
    wchar_t w[64];
    int n = GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, w, 64, nullptr);
    if (n <= 1) return "";
    char out[128];
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n - 1, out, sizeof out, nullptr, nullptr);
    return std::string(out, size_t(std::max(0, len)));
}

// ---- the Guide's page

void Guide::set_achievements(const AchUnlocks& u) {
    ach_ = u;
    fix_home_focus();
    // the list went away under the page (the mod's file was removed): back to where it was opened from
    if (open && page() == Page::Achievements && !ach_.known) stack_.pop_back(), page_at_ = -10;
}

void Guide::handle_achievements(PadEvent e, double now) {
    int n = int(DEFS.size()), f = ach_focus_;
    switch (e) {
    case PadEvent::Left:
        if (f % COLS) f--;
        break;
    case PadEvent::Right:
        if (f % COLS != COLS - 1 && f + 1 < n) f++;
        break;
    case PadEvent::Up:
        if (f >= COLS) f -= COLS;
        break;
    case PadEvent::Down:
        if (f + COLS < n) f += COLS;
        else if (f / COLS < (n - 1) / COLS) f = n - 1;  // the last row is shorter
        break;
    case PadEvent::B: pop(now); return;
    default: break;
    }
    ach_focus_ = f;
    int row = f / COLS;
    ach_top_ = std::clamp(ach_top_, row - (ROWS - 1), row);
}

void Guide::draw_achievements(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Achievements", title_op);
    float x = L + slide;
    int n = int(DEFS.size());

    // the game, its count and the Gamerscore earned in it
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + W, 386), alpha(PANEL, pop));
    ImVec2 tp = v.pt(x + 27, 123);
    text_mid(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, alpha(WHITE, pop), "Forza Horizon 2");
    ImVec2 rp = v.pt(x + W - 30, 123);
    float left = g_score(dl, a, v, rp.x, rp.y, 22, alpha(WHITE, pop), ach_.gamerscore());
    char count[48];
    snprintf(count, sizeof count, "%d of %d Achievements", ach_.count(), n);
    text_mid(dl, a.text, a.text_m, v.sz(20), left - v.sz(22), rp.y, alpha(MUTED, pop), count, true);
    dl->AddLine(v.pt(x, 150), v.pt(x + W, 150), alpha(LINE, pop), v.sz(1));

    // the grid: three rows on screen, the rest scroll under them
    const float gx = x + (W - (COLS * TILE + (COLS - 1) * GAP)) / 2, gy = 150 + 14;
    int rows = (n + COLS - 1) / COLS;
    dl->PushClipRect(v.pt(x, gy - 5), v.pt(x + W, gy + ROWS * TILE + (ROWS - 1) * GAP + 5), true);
    for (int i = ach_top_ * COLS; i < std::min(n, (ach_top_ + ROWS) * COLS); i++) {
        float tx = gx + (i % COLS) * (TILE + GAP), ty = gy + (i / COLS - ach_top_) * (TILE + GAP);
        ImVec2 p0 = v.pt(tx, ty), p1 = v.pt(tx + TILE, ty + TILE);
        tile(dl, a, p0, p1, ach_.has(DEFS[i].id), pop);
        if (i == ach_focus_) dl->AddRect(ImVec2(p0.x - v.sz(2.5f), p0.y - v.sz(2.5f)), ImVec2(p1.x + v.sz(2.5f), p1.y + v.sz(2.5f)), alpha(WHITE, pop), 0, 0, v.sz(3));
    }
    dl->PopClipRect();
    ImVec2 up = v.pt(x + W - 20, gy + 84), down = v.pt(x + W - 20, gy + 124);
    icon_mid(dl, a, v.sz(28), up.x, up.y, alpha(ach_top_ > 0 ? MUTED : DISABLED, pop), I_EXPAND_LESS);
    icon_mid(dl, a, v.sz(28), down.x, down.y, alpha(ach_top_ + ROWS < rows ? MUTED : DISABLED, pop), I_EXPAND_MORE);

    // the focused achievement: its tile, name, date (when unlocked), points and description
    dl->AddRectFilled(v.pt(x, 386), v.pt(x + W, 607), alpha(PANE, pop));
    if (ach_focus_ >= 0 && ach_focus_ < n) {
        const AchDef& d = DEFS[ach_focus_];
        bool got = ach_.has(d.id);
        const float y0 = 386 + 24, tx = x + 27, side = 96, ix = tx + side + 22, right = x + W - 30;
        tile(dl, a, v.pt(tx, y0), v.pt(tx + side, y0 + side), got, pop);
        float name_w = right - ix;
        if (got) {
            std::string date = ach_date(ach_.when.at(d.id));
            if (!date.empty()) {
                ImVec2 dp = v.pt(right, y0 + 16);
                text_mid(dl, a.text, a.text_m, v.sz(20), dp.x, dp.y, alpha(WHITE, pop), date.c_str(), true);
                name_w -= a.text->CalcTextSizeA(20, FLT_MAX, 0, date.c_str()).x + 20;
            }
        }
        ImVec2 np = v.pt(ix, y0 + 16);
        text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(name_w), alpha(WHITE, pop), d.name);
        // points: the mark, then the number
        ImVec2 gp = v.pt(ix + 11, y0 + 52);
        g_mark(dl, a, gp.x, gp.y, v.sz(22), alpha(got ? WHITE : MUTED, pop));
        char pts[16];
        snprintf(pts, sizeof pts, "%d", d.g);
        text_mid(dl, a.text, a.text_m, v.sz(22), gp.x + v.sz(11 + 8), gp.y, alpha(got ? WHITE : MUTED, pop), pts);
        wrapped(dl, a, v, 21, 1.35f, ix, y0 + 76, right - ix, alpha(MUTED, pop), d.desc);
    }
    const Hint hints[] = {{"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 1, op);
}

// ---- Compare Games (a player's options): this player and the other, achievement by achievement. The 360's gp\826-7_GameAchievStatsYou.xur:
// both pictures with their Gamerscore, the list with one mark for each of the two, and gp\828_AchievDetails.xur behind A (the picture,
// the title, the points, "Achievement Description" and the text). One game, so the 360's list of the games both played
// (gp\825_GameShowcaseCompare.xur) is skipped. The other player's unlocks are what their Guide reported to the server.

namespace {
const int CMP_ROWS = 6;
const uint32_t I_CHECK = 0xe5ca;  // Material Icons "check"
}  // namespace

void Guide::open_compare(const Friend& them, std::vector<GuideRequest>& out, double now) {
    cmp_ = them, cmp_list_ = ListFocus();
    GuideRequest r;
    r.action = GuideAction::Achievements, r.xuid = them.xuid, r.name = them.tag;
    out.push_back(r);
    push(Page::Compare, now);
}

void Guide::handle_compare(PadEvent e, double now) {
    int n = int(DEFS.size());
    switch (e) {
    case PadEvent::Up: cmp_list_.move(-1, n, CMP_ROWS, now); break;
    case PadEvent::Down: cmp_list_.move(1, n, CMP_ROWS, now); break;
    case PadEvent::A:
        ach_detail_ = cmp_list_.row;
        push(Page::AchDetail, now);
        break;
    case PadEvent::B: pop(now); break;
    default: break;
    }
}

void Guide::draw_compare(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Compare Games", title_op);
    float x = L + slide;
    int n = int(DEFS.size());
    auto it = social_.ach_of.find(cmp_.xuid);
    const AchUnlocks* theirs = it == social_.ach_of.end() ? nullptr : &it->second;  // null until the server answered

    // the two players: this one at the left, the other at the right, as their marks in the rows below
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + W, 164), alpha(PANEL, pop));
    player_pic(dl, a, a.gamerpic, social_.me.xuid, v.pt(x + 14, 106), v.pt(x + 62, 154), pop);
    ImVec2 mn = v.pt(x + 74, 118), mg = v.pt(x + 74 + 10, 142);
    text_fit(dl, a.text, a.text_m, v.sz(20), mn.x, mn.y, v.sz(250), alpha(WHITE, pop), gamertag);
    g_mark(dl, a, mg.x, mg.y, v.sz(20), alpha(MUTED, pop));
    char mine[16];
    snprintf(mine, sizeof mine, "%d", ach_.gamerscore());
    text_mid(dl, a.text, a.text_m, v.sz(20), mg.x + v.sz(10 + 7), mg.y, alpha(MUTED, pop), mine);
    player_pic(dl, a, a.pic(cmp_.xuid), cmp_.xuid, v.pt(x + W - 62, 106), v.pt(x + W - 14, 154), pop);
    ImVec2 tn = v.pt(x + W - 74, 118), tg = v.pt(x + W - 74, 142);
    float tw = a.text->CalcTextSizeA(20, FLT_MAX, 0, cmp_.tag.c_str()).x;
    if (tw <= 250) text_mid(dl, a.text, a.text_m, v.sz(20), tn.x, tn.y, alpha(WHITE, pop), cmp_.tag.c_str(), true);
    else text_fit(dl, a.text, a.text_m, v.sz(20), tn.x - v.sz(250), tn.y, v.sz(250), alpha(WHITE, pop), cmp_.tag);
    int their_score = theirs ? theirs->gamerscore() : cmp_.gamerscore;
    if (their_score >= 0) g_score(dl, a, v, tg.x, tg.y, 20, alpha(MUTED, pop), their_score);
    float gw = a.text->CalcTextSizeA(20, FLT_MAX, 0, "Forza Horizon 2").x;
    ImVec2 gp = v.pt(x + (W - gw) / 2, 130);
    text_mid(dl, a.text, a.text_m, v.sz(20), gp.x, gp.y, alpha(MUTED, pop), "Forza Horizon 2");

    // the achievements: tile, name over description, then this player's mark and the other's
    const float top = 164, RH = 69, col_me = W - 112, col_them = W - 44;
    ImVec2 p0 = v.pt(x, top), p1 = v.pt(x + W, top + CMP_ROWS * RH);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->AddLine(v.pt(x, top), v.pt(x + W, top), alpha(LINE, pop), v.sz(1));
    dl->PushClipRect(p0, p1, true);
    float by = top + cmp_list_.bar.value(now) * RH;
    dl->AddRectFilled(v.pt(x, by), v.pt(x + W, by + RH), alpha(ACCENT, pop));
    for (int i = cmp_list_.top; i < std::min(n, cmp_list_.top + CMP_ROWS); i++) {
        const AchDef& d = DEFS[i];
        float y = top + (i - cmp_list_.top) * RH, cy = y + RH / 2;
        bool foc = i == cmp_list_.row, got_me = ach_.has(d.id), got_them = theirs && theirs->has(d.id);
        if (!foc) dl->AddLine(v.pt(x, y + RH - 0.5f), v.pt(x + W, y + RH - 0.5f), alpha(LINE, pop), v.sz(1));
        ImVec2 t0 = v.pt(x + 12, y + 10.5f), t1 = v.pt(x + 60, y + 58.5f);
        dl->AddRectFilled(t0, t1, alpha(foc ? DARK : RAISED, pop));
        icon_mid(dl, a, v.sz(28), (t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f, alpha(got_me || got_them ? WHITE : DISABLED, pop), I_TROPHY);
        ImVec2 n0 = v.pt(x + 74, cy - 13), n1 = v.pt(x + 74, cy + 14.15f);
        float room = col_me - 74 - 40;
        text_fit(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(room), alpha(WHITE, pop), d.name);
        text_fit(dl, a.text, a.text_m, v.sz(18), n1.x, n1.y, v.sz(room), alpha(foc ? WHITE : MUTED2, pop), d.desc);
        auto mark = [&](float cx, bool got) {
            ImVec2 c = v.pt(x + cx, cy);
            icon_mid(dl, a, v.sz(got ? 30 : 24), c.x, c.y, alpha(got ? (foc ? WHITE : BRIGHT) : (foc ? DARK : DISABLED), pop), got ? I_CHECK : I_LOCK);
        };
        mark(col_me, got_me);
        if (theirs) mark(col_them, got_them);
    }
    dl->PopClipRect();
    ImVec2 f0 = v.pt(x, top + CMP_ROWS * RH), f1 = v.pt(x + W, 607);
    dl->AddRectFilled(f0, f1, alpha(PANEL, pop));
    dl->AddLine(ImVec2(f0.x, f0.y + v.sz(0.5f)), ImVec2(f1.x, f0.y + v.sz(0.5f)), alpha(LINE, pop), v.sz(1));
    ImVec2 fr = v.pt(x + W - 30, (top + CMP_ROWS * RH + 607) / 2);
    icon_mid(dl, a, v.sz(24), fr.x - v.sz(12), fr.y, alpha(cmp_list_.top + CMP_ROWS < n ? MUTED : DISABLED, pop), I_EXPAND_MORE);
    icon_mid(dl, a, v.sz(24), fr.x - v.sz(36), fr.y, alpha(cmp_list_.top > 0 ? MUTED : DISABLED, pop), I_EXPAND_LESS);
    char count[32];
    snprintf(count, sizeof count, "%d of %d", cmp_list_.row + 1, n);
    text_mid(dl, a.text, a.text_m, v.sz(18), fr.x - v.sz(56), fr.y, alpha(MUTED, pop), count, true);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}

void Guide::handle_ach_detail(PadEvent e, double now) {
    if (e == PadEvent::B) pop(now);
}

void Guide::draw_ach_detail(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Compare Games", title_op);
    float x = L + slide;
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + W, 607), alpha(PANEL, pop));
    if (ach_detail_ >= 0 && ach_detail_ < int(DEFS.size())) {
        const AchDef& d = DEFS[ach_detail_];
        auto it = social_.ach_of.find(cmp_.xuid);
        bool got = ach_.has(d.id) || (it != social_.ach_of.end() && it->second.has(d.id));
        const float y0 = 96 + 27, tx = x + 27, side = 96, ix = tx + side + 22, right = x + W - 30;
        tile(dl, a, v.pt(tx, y0), v.pt(tx + side, y0 + side), got, pop);
        ImVec2 np = v.pt(ix, y0 + 30);
        text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(right - ix), alpha(WHITE, pop), d.name);
        ImVec2 gp = v.pt(ix + 11, y0 + 68);
        g_mark(dl, a, gp.x, gp.y, v.sz(22), alpha(WHITE, pop));
        char pts[16];
        snprintf(pts, sizeof pts, "%d", d.g);
        text_mid(dl, a.text, a.text_m, v.sz(22), gp.x + v.sz(11 + 8), gp.y, alpha(WHITE, pop), pts);
        ImVec2 lp = v.pt(tx, y0 + side + 38);
        text_mid(dl, a.text, a.text_m, v.sz(18), lp.x, lp.y, alpha(MUTED, pop), "Achievement Description");
        wrapped(dl, a, v, 22, 1.4f, tx, y0 + side + 60, W - 54, alpha(WHITE, pop), d.desc);
    }
    const Hint hints[] = {{"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 1, op);
}
