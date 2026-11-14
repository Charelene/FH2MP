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
#include <cmath>
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
    draw_header(dl, a, origin, s, zoom, op, "Achievements", title_op);
    float x = L + slide;
    int n = int(DEFS.size());

    // the game, its count and the Gamerscore earned in it
    panel_fill(dl, v.pt(x, 96), v.pt(x + W, 386), pop);
    ImVec2 tp = v.pt(x + 27, 123);
    text_mid(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, alpha(TEXT, pop), "Forza Horizon 2");
    ImVec2 rp = v.pt(x + W - 30, 123);
    float left = g_score(dl, a, v, rp.x, rp.y, 22, alpha(TEXT, pop), ach_.gamerscore());
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
        if (i == ach_focus_) dl->AddRect(ImVec2(p0.x - v.sz(2.5f), p0.y - v.sz(2.5f)), ImVec2(p1.x + v.sz(2.5f), p1.y + v.sz(2.5f)), alpha(TEXT, pop), 0, 0, v.sz(3));
    }
    dl->PopClipRect();
    ImVec2 up = v.pt(x + W - 20, gy + 84), down = v.pt(x + W - 20, gy + 124);
    icon_mid(dl, a, v.sz(28), up.x, up.y, alpha(ach_top_ > 0 ? MUTED : DISABLED, pop), I_EXPAND_LESS);
    icon_mid(dl, a, v.sz(28), down.x, down.y, alpha(ach_top_ + ROWS < rows ? MUTED : DISABLED, pop), I_EXPAND_MORE);

    // the focused achievement: its tile, name, date (when unlocked), points and description
    dl->AddRectFilled(v.pt(x, 386), v.pt(x + W, PAGE_B), alpha(PANE, pop));
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
                text_mid(dl, a.text, a.text_m, v.sz(20), dp.x, dp.y, alpha(TEXT, pop), date.c_str(), true);
                name_w -= a.text->CalcTextSizeA(20, FLT_MAX, 0, date.c_str()).x + 20;
            }
        }
        ImVec2 np = v.pt(ix, y0 + 16);
        text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(name_w), alpha(TEXT, pop), d.name);
        // points: the mark, then the number
        ImVec2 gp = v.pt(ix + 11, y0 + 52);
        g_mark(dl, a, gp.x, gp.y, v.sz(22), alpha(got ? TEXT : MUTED, pop));
        char pts[16];
        snprintf(pts, sizeof pts, "%d", d.g);
        text_mid(dl, a.text, a.text_m, v.sz(22), gp.x + v.sz(11 + 8), gp.y, alpha(got ? TEXT : MUTED, pop), pts);
        wrapped(dl, a, v, 21, 1.35f, ix, y0 + 76, right - ix, alpha(MUTED, pop), d.desc);
    }
    const Hint hints[] = {{"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 1, op);
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
    draw_header(dl, a, origin, s, zoom, op, "Compare Games", title_op);
    float x = L + slide;
    int n = int(DEFS.size());
    auto it = social_.ach_of.find(cmp_.xuid);
    const AchUnlocks* theirs = it == social_.ach_of.end() ? nullptr : &it->second;  // null until the server answered

    // the two players: this one at the left, the other at the right, as their marks in the rows below
    panel_fill(dl, v.pt(x, 96), v.pt(x + W, 164), pop);
    player_pic(dl, a, a.gamerpic, social_.me.xuid, v.pt(x + 14, 106), v.pt(x + 62, 154), pop);
    ImVec2 mn = v.pt(x + 74, 118), mg = v.pt(x + 74 + 10, 142);
    text_fit(dl, a.text, a.text_m, v.sz(20), mn.x, mn.y, v.sz(250), alpha(TEXT, pop), gamertag);
    g_mark(dl, a, mg.x, mg.y, v.sz(20), alpha(MUTED, pop));
    char mine[16];
    snprintf(mine, sizeof mine, "%d", ach_.gamerscore());
    text_mid(dl, a.text, a.text_m, v.sz(20), mg.x + v.sz(10 + 7), mg.y, alpha(MUTED, pop), mine);
    player_pic(dl, a, a.pic(cmp_.xuid), cmp_.xuid, v.pt(x + W - 62, 106), v.pt(x + W - 14, 154), pop);
    ImVec2 tn = v.pt(x + W - 74, 118), tg = v.pt(x + W - 74, 142);
    float tw = a.text->CalcTextSizeA(20, FLT_MAX, 0, cmp_.tag.c_str()).x;
    if (tw <= 250) text_mid(dl, a.text, a.text_m, v.sz(20), tn.x, tn.y, alpha(TEXT, pop), cmp_.tag.c_str(), true);
    else text_fit(dl, a.text, a.text_m, v.sz(20), tn.x - v.sz(250), tn.y, v.sz(250), alpha(TEXT, pop), cmp_.tag);
    int their_score = theirs ? theirs->gamerscore() : cmp_.gamerscore;
    if (their_score >= 0) g_score(dl, a, v, tg.x, tg.y, 20, alpha(MUTED, pop), their_score);
    float gw = a.text->CalcTextSizeA(20, FLT_MAX, 0, "Forza Horizon 2").x;
    ImVec2 gp = v.pt(x + (W - gw) / 2, 130);
    text_mid(dl, a.text, a.text_m, v.sz(20), gp.x, gp.y, alpha(MUTED, pop), "Forza Horizon 2");

    // the achievements: tile, name over description, then this player's mark and the other's
    const float top = 164, RH = 69, col_me = W - 112, col_them = W - 44;
    ImVec2 p0 = v.pt(x, top), p1 = v.pt(x + W, top + CMP_ROWS * RH);
    panel_fill(dl, p0, p1, pop);
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
        icon_mid(dl, a, v.sz(28), (t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f, alpha(got_me || got_them ? (foc ? INK : TEXT) : DISABLED, pop), I_TROPHY);
        ImVec2 n0 = v.pt(x + 74, cy - 13), n1 = v.pt(x + 74, cy + 14.15f);
        float room = col_me - 74 - 40;
        text_fit(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(room), alpha(foc ? INK : TEXT, pop), d.name);
        text_fit(dl, a.text, a.text_m, v.sz(18), n1.x, n1.y, v.sz(room), alpha(foc ? INK : MUTED2, pop), d.desc);
        auto mark = [&](float cx, bool got) {
            ImVec2 c = v.pt(x + cx, cy);
            icon_mid(dl, a, v.sz(got ? 30 : 24), c.x, c.y, alpha(got ? (foc ? INK : BRIGHT) : (foc ? DARK : DISABLED), pop), got ? I_CHECK : I_LOCK);
        };
        mark(col_me, got_me);
        if (theirs) mark(col_them, got_them);
    }
    dl->PopClipRect();
    ImVec2 f0 = v.pt(x, top + CMP_ROWS * RH), f1 = v.pt(x + W, PAGE_B);
    panel_fill(dl, f0, f1, pop);
    dl->AddLine(ImVec2(f0.x, f0.y + v.sz(0.5f)), ImVec2(f1.x, f0.y + v.sz(0.5f)), alpha(LINE, pop), v.sz(1));
    ImVec2 fr = v.pt(x + W - 30, (top + CMP_ROWS * RH + PAGE_B) / 2);
    icon_mid(dl, a, v.sz(24), fr.x - v.sz(12), fr.y, alpha(cmp_list_.top + CMP_ROWS < n ? MUTED : DISABLED, pop), I_EXPAND_MORE);
    icon_mid(dl, a, v.sz(24), fr.x - v.sz(36), fr.y, alpha(cmp_list_.top > 0 ? MUTED : DISABLED, pop), I_EXPAND_LESS);
    char count[32];
    snprintf(count, sizeof count, "%d of %d", cmp_list_.row + 1, n);
    text_mid(dl, a.text, a.text_m, v.sz(18), fr.x - v.sz(56), fr.y, alpha(MUTED, pop), count, true);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
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
    draw_header(dl, a, origin, s, zoom, op, "Compare Games", title_op);
    float x = L + slide;
    panel_fill(dl, v.pt(x, 96), v.pt(x + W, PAGE_B), pop);
    if (ach_detail_ >= 0 && ach_detail_ < int(DEFS.size())) {
        const AchDef& d = DEFS[ach_detail_];
        auto it = social_.ach_of.find(cmp_.xuid);
        bool got = ach_.has(d.id) || (it != social_.ach_of.end() && it->second.has(d.id));
        const float y0 = 96 + 27, tx = x + 27, side = 96, ix = tx + side + 22, right = x + W - 30;
        tile(dl, a, v.pt(tx, y0), v.pt(tx + side, y0 + side), got, pop);
        ImVec2 np = v.pt(ix, y0 + 30);
        text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(right - ix), alpha(TEXT, pop), d.name);
        ImVec2 gp = v.pt(ix + 11, y0 + 68);
        g_mark(dl, a, gp.x, gp.y, v.sz(22), alpha(TEXT, pop));
        char pts[16];
        snprintf(pts, sizeof pts, "%d", d.g);
        text_mid(dl, a.text, a.text_m, v.sz(22), gp.x + v.sz(11 + 8), gp.y, alpha(TEXT, pop), pts);
        ImVec2 lp = v.pt(tx, y0 + side + 38);
        text_mid(dl, a.text, a.text_m, v.sz(18), lp.x, lp.y, alpha(MUTED, pop), "Achievement Description");
        wrapped(dl, a, v, 22, 1.4f, tx, y0 + side + 60, W - 54, alpha(TEXT, pop), d.desc);
    }
    const Hint hints[] = {{"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 1, op);
}

// ---- the achievement toast
//
// The Guide's toast (an accent square, a dark panel unfolding from it, the title in the accent over a white line) moving on Game Bar's
// times: Microsoft.XboxGamingOverlay 7.326's GameBar.exe shows an achievement with Widgets/Toasts/EnhancedToastPanel.xaml, a rare one
// with EnhancedGleamToastPanel.xaml and the Mythic Achievement with EnhancedAuroraToastPanel.xaml; their storyboards were read out of
// the compiled XAML in Game Bar's resources.pri, their timers out of the panels' code (2026-11-11). The user asked for the Guide's square
// style on those times (2026-11-11), so Game Bar's circle and pill are the square and the panel here, and what was sized to them is
// scaled to the square (64 units for Game Bar's 100 px).
//   Show, from 0: four squares grow out of the toast's centre 0.1 s apart (light, medium, dark, medium again; 0 -> 1.12 -> 1); the icon's
//     square grows 0.667-1.433 s; the icon pops in 0.7-1.333 s (0 -> 1.2 -> 1); the panel unfolds 1.433-2.433 s while the square slides
//     to its left; the text comes in 1.333-2.333 s and fades in 1.733-2.333 s.
//   The text turns over at 4 s, when Show's 4 s storyboard has run (inferred: Game Bar goes there from an event, not a timer); the hint
//     replaces it at 7.133 s (8 s for the mythic); Hide starts at 8.467 s (10 s for the mythic).
//   Hide, 1.633 s: the panel folds and the square slides back over 0.667 s, the icon plays its exit and shrinks 0.767-1.367 s, the
//     squares go last (dark 0.866-1.366 s, medium 0.9-1.5 s, light 0.933-1.633 s).
//   Rare: from 2.433 s a gleam crosses the panel, twice. Mythic: from 2.433 s two pulses and two turning glows behind and over the icon, a
//     ripple at 4.433 s, the gleam with six sparkles on the panel's edges from 5.064 s, and the aurora haze around the toast from 2.433 s
//     (its cut-out fitted to the square and the panel rather than placed as on Game Bar's pill).
// The icon sheets are played at 30 frames a second, as Game Bar's flip books are. The plain and rare ones start and end with the
// console's emblem turning; those frames are replaced by the Guide's emblem (a white disc with the trophy of the icon font) making the
// same turn.

namespace {

const float PI = 3.14159265f;

enum class Ease { Linear, ExpIn, ExpOut, ExpInOut, BackIn, Spline };
struct Key {
    float t = 0, v = 0;
    Ease e = Ease::Linear;
    float p0 = 0, p1 = 0, p2 = 0, p3 = 0;  // Spline: its control points; Exp*: p0 the exponent; BackIn: p0 the amplitude
};
Key kf(float t, float v) { return Key{t, v, Ease::Linear, 0, 0, 0, 0}; }
Key kfs(float t, float v, float x1, float y1, float x2, float y2) { return Key{t, v, Ease::Spline, x1, y1, x2, y2}; }
Key kfei(float t, float v, float ex = 7) { return Key{t, v, Ease::ExpIn, ex, 0, 0, 0}; }
Key kfeo(float t, float v, float ex = 7) { return Key{t, v, Ease::ExpOut, ex, 0, 0, 0}; }
Key kfeio(float t, float v, float ex = 2) { return Key{t, v, Ease::ExpInOut, ex, 0, 0, 0}; }
Key kfbi(float t, float v, float amp) { return Key{t, v, Ease::BackIn, amp, 0, 0, 0}; }

// XAML's easing functions (ExponentialEase, BackEase, a key spline), x in 0..1.
float eased(const Key& k, float x) {
    switch (k.e) {
    case Ease::Linear: return x;
    case Ease::ExpIn:
    case Ease::ExpOut:
    case Ease::ExpInOut: {
        float e = k.p0;
        auto in = [&](float u) { return (expf(e * u) - 1) / (expf(e) - 1); };
        if (k.e == Ease::ExpIn) return in(x);
        if (k.e == Ease::ExpOut) return 1 - in(1 - x);
        return x < 0.5f ? in(2 * x) / 2 : 1 - in(2 - 2 * x) / 2;
    }
    case Ease::BackIn: return x * x * x - x * k.p0 * sinf(PI * x);
    case Ease::Spline: return css_bezier(k.p0, k.p1, k.p2, k.p3, x);
    }
    return x;
}

// A key frame track as XAML plays it: the first key's value before it, the later key's easing between two, the last key's value after.
float track(std::initializer_list<Key> keys, float t) {
    const Key* prev = nullptr;
    for (const Key& k : keys) {
        if (t < k.t) {
            if (!prev) return k.v;
            return prev->v + (k.v - prev->v) * eased(k, (t - prev->t) / std::max(1e-6f, k.t - prev->t));
        }
        prev = &k;
    }
    return prev ? prev->v : 0;
}

// 0 -> 1 -> 0 over twice `half` seconds, again and again (an AutoReverse animation that repeats Forever).
float back_and_forth(float t, float half) {
    float ph = fmodf(std::max(0.0f, t), 2 * half) / half;
    return ph < 1 ? ph : 2 - ph;
}

ImU32 mix(ImU32 a, ImU32 b, float k) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * k, x.y + (y.y - x.y) * k, x.z + (y.z - x.z) * k, x.w));
}

// One cell of a sheet, w by h, centred on c.
void cell(ImDrawList* dl, const SpriteSheet& sh, int f, ImVec2 c, float w, float h, ImU32 col) {
    if (!sh.tex || !sh.frames || w < 0.5f || h < 0.5f) return;
    f = std::clamp(f, 0, sh.frames - 1);
    float u = float(f % sh.cols) / sh.cols, v = float(f / sh.cols) / sh.rows;
    dl->AddImage(sh.tex, ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), ImVec2(u, v), ImVec2(u + 1.0f / sh.cols, v + 1.0f / sh.rows), col);
}

// A square picture centred on c, turned by angle (radians).
void turned(ImDrawList* dl, ImTextureID tex, ImVec2 c, float size, float angle, ImU32 col) {
    if (!tex || size < 0.5f) return;
    float h = size / 2, co = cosf(angle), si = sinf(angle);
    auto p = [&](float x, float y) { return ImVec2(c.x + x * co - y * si, c.y + x * si + y * co); };
    dl->AddImageQuad(tex, p(-h, -h), p(h, -h), p(h, h), p(-h, h), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), col);
}

void picture(ImDrawList* dl, ImTextureID tex, ImVec2 c, float w, float h, ImU32 col) {
    if (tex && w > 0.5f && h > 0.5f) dl->AddImage(tex, ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), ImVec2(0, 0), ImVec2(1, 1), col);
}

// What was drawn since vertex `from`, squeezed horizontally about x = cx by kx (a XAML ScaleX) and moved by dx.
void squeeze(ImDrawList* dl, int from, float cx, float kx, float dx = 0) {
    for (int i = from; i < dl->VtxBuffer.Size; i++) dl->VtxBuffer[i].pos.x = cx + (dl->VtxBuffer[i].pos.x - cx) * kx + dx;
}

// The Guide's emblem where the console's is on the icon sheets: a white disc with the trophy cut out of it in the circle's colour. turn 0
// faces; towards 1 it has turned away to the right, towards -1 to the left, as the console's emblem turns on the sheets.
void emblem(ImDrawList* dl, const GuideAssets& a, ImVec2 c, float d, ImU32 ink, float turn, float op, uint32_t glyph = I_TROPHY) {
    if (d < 1 || op <= 0.004f) return;
    dl->AddCircleFilled(c, d / 2, alpha(WHITE, op), 48);
    float face = cosf(std::min(1.0f, std::fabs(turn)) * PI / 2);
    if (face < 0.03f) return;
    int from = dl->VtxBuffer.Size;
    icon_mid(dl, a, d * 0.64f, c.x, c.y, alpha(ink, op * std::min(1.0f, face * 1.6f)), glyph);
    squeeze(dl, from, c.x, face, (turn > 0 ? 1 : -1) * (1 - face) * d * 0.22f);
}

// When Hide starts (the panels' timers).
float hide_at(int tier) { return tier == 2 ? 10.0f : 8.467f; }

// The Xbox One toast's laurels (Settings > Notifications > Xbox One), as Game Bar builds them: one half of the wreath in a 112-unit
// canvas from a stem curve and leaf outlines, mirrored for the other side. They are no picture: the outlines and the table of leaves
// are the values Game Bar is compiled with, in its pixels (its capsule is 100 high). A leaf outline is its tip, a curve down to its base
// and a curve back up; a stem its foot and one curve.
const float LEAF[10][14] = {
    {48.654f, -31.684f, 48.163f, -25.602f, 43.702f, -25.7f, 47.626f, -11.183f, 52.53f, -14.565f, 54.539f, -26.14f, 48.654f, -31.684f},
    {48.705f, -31.919f, 48.187f, -25.645f, 43.439f, -19.909f, 47.626f, -11.183f, 52.221f, -16.37f, 55.038f, -27.595f, 48.705f, -31.919f},
    {48.654f, -31.684f, 48.203f, -24.178f, 43.73f, -22.22f, 47.626f, -11.183f, 52.731f, -16.727f, 55.034f, -26.599f, 48.654f, -31.684f},
    {48.956f, -31.747f, 48.43f, -24.68f, 43.737f, -21.537f, 47.626f, -11.183f, 52.884f, -17.048f, 55.367f, -26.838f, 48.956f, -31.747f},
    {48.999f, -31.845f, 48.54f, -25.794f, 43.412f, -17.751f, 47.626f, -11.183f, 53.276f, -17.382f, 55.885f, -26.497f, 48.999f, -31.845f},
    {49.478f, -33.175f, 46.504f, -30.487f, 45.299f, -19.893f, 47.626f, -11.183f, 53.213f, -13.066f, 54.324f, -23.581f, 49.478f, -33.175f},
    {48.756f, -31.86f, 45.613f, -28.317f, 46.131f, -19.509f, 47.626f, -11.183f, 52.876f, -12.446f, 54.21f, -22.585f, 48.756f, -31.86f},
    {49.003f, -32.571f, 46.084f, -28.364f, 45.876f, -19.866f, 47.579f, -10.994f, 53.421f, -11.328f, 54.708f, -24.245f, 49.003f, -32.571f},
    {49.654f, -34.858f, 46.166f, -31.731f, 46.841f, -18.904f, 47.587f, -11.084f, 53.621f, -11.473f, 55.41f, -26.073f, 49.654f, -34.858f},
    {48.654f, -31.684f, 48.163f, -25.602f, 45.346f, -23.118f, 47.626f, -11.183f, 53.943f, -15.597f, 55.571f, -24.598f, 48.654f, -31.684f},
};
const float STEM[2][8] = {{56.273f, 100.737f, 71.112f, 100.737f, 82.803f, 94.893f, 91.072f, 86.58f},     // rare: short
                          {56.273f, 100.737f, 83.253f, 100.737f, 99.827f, 81.417f, 104.347f, 63.07f}};  // mythic: long
const float LEAF_PIVOT[2] = {47.751f, -10.909f};  // every leaf turns and grows about this point (its base)
// A leaf: its outline, the angle it rests at and the one it swings in from, where it goes, its size, and when it grows and shrinks (ms).
struct Leaf {
    int shape;
    float rot, from_rot, x, y, scale, in0, in1, out0, out1;
};
const Leaf WREATH_RARE[5] = {{9, 34.2f, 69.2f, 40.088f, 100.488f, 1, 367, 933, 0, 250},      {3, 21.3f, -29.7f, 31.055f, 106.473f, 0.961f, 200, 800, 67, 317},
                             {4, 41.3f, 17.3f, 13.888f, 111.476f, 1.033f, 0, 600, 167, 417}, {7, 76.9f, 136.9f, 26.591f, 108.632f, 1.025f, 67, 667, 133, 383},
                             {8, 95.3f, 118.3f, 8.297f, 111.918f, 1.051f, 0, 567, 183, 433}};
const Leaf WREATH_MYTHIC[9] = {{0, 0, 30, 56.068f, 75.967f, 1, 433, 933, 0, 250},              {1, -12, -63, 53.027f, 83.765f, 0.81f, 300, 800, 67, 317},
                               {2, 4.4f, -46.6f, 44.199f, 96.468f, 0.883f, 133, 633, 150, 384}, {3, 21.3f, -29.7f, 31.055f, 106.473f, 0.961f, 67, 567, 184, 434},
                               {4, 41.3f, 17.3f, 13.888f, 111.476f, 1.033f, 0, 500, 217, 467},  {5, 39.2f, 101.7f, 50.673f, 88.277f, 0.81f, 200, 700, 117, 367},
                               {6, 59.4f, 119.4f, 40.275f, 100.196f, 0.965f, 100, 600, 167, 417}, {7, 76.9f, 136.9f, 26.591f, 108.632f, 1.025f, 33, 533, 200, 450},
                               {8, 95.3f, 118.3f, 8.297f, 111.918f, 1.051f, 0, 467, 234, 467}};

// The wreath round the icon at c (pixels; u: pixels per Game Bar pixel), t seconds into the toast and h into its Hide (negative before
// it). The stem grows from its foot, each leaf grows while it swings open to its resting angle, and on Hide they fold back, tips first
// (the first sprout 0.95 s after the capsule starts to open for the rare one, 1.05 s for the mythic, as captures of the console show).
// Each is the colour given under a white sheen (Game Bar's runs from 60% to 20% across the leaf: 40% here).
void laurels(ImDrawList* dl, ImVec2 c, float u, int tier, float t, float h, ImU32 leaf) {
    auto seg = [](float v, float a, float b) { return std::clamp((v - a) / (b - a), 0.0f, 1.0f); };
    auto exp_out = [](float x) { return (1 - expf(-7 * x)) / (1 - expf(-7.0f)); };
    auto cube = [](float x) { return x * x * x; };
    const float in = (t - (tier == 2 ? 2.48f : 2.38f)) * 1000, out = h * 1000;
    const float stem = exp_out(seg(in, 100, 500)) * (1 - cube(seg(out, 0, tier == 2 ? 467.0f : 433.0f)));
    const float* st = STEM[tier - 1];
    const Leaf* wreath = tier == 2 ? WREATH_MYTHIC : WREATH_RARE;
    const ImU32 sheen[2] = {leaf, IM_COL32(255, 255, 255, 102)};
    for (float side : {1.0f, -1.0f}) {
        // the half's canvas: its corner 56 left of the icon's centre and 50 above it
        auto place = [&](float x, float y) { return ImVec2(c.x + side * (x - 56) * u, c.y + (y - 50) * u); };
        if (stem > 0.001f) {
            auto sp = [&](int i) { return place(st[0] + (st[i] - st[0]) * stem, st[1] + (st[i + 1] - st[1]) * stem); };
            dl->AddBezierCubic(sp(0), sp(2), sp(4), sp(6), leaf, 1.569f * u);
            dl->AddBezierCubic(sp(0), sp(2), sp(4), sp(6), IM_COL32(255, 255, 255, 51), 1.569f * u);
        }
        for (int i = 0; i < (tier == 2 ? 9 : 5); i++) {
            const Leaf& L = wreath[i];
            float k = exp_out(seg(in, L.in0, L.in1)) * (1 - cube(seg(out, L.out0, L.out1)));  // 0 folded away, 1 open
            if (k <= 0.001f) continue;
            float ang = (L.from_rot + (L.rot - L.from_rot) * k) * PI / 180, co = cosf(ang), si = sinf(ang), sc = L.scale * k;
            auto pt = [&](int j) {
                float x = (LEAF[L.shape][j] - LEAF_PIVOT[0]) * sc, y = (LEAF[L.shape][j + 1] - LEAF_PIVOT[1]) * sc;
                return place(LEAF_PIVOT[0] + x * co - y * si + L.x, LEAF_PIVOT[1] + x * si + y * co + L.y);
            };
            for (ImU32 col : sheen) {
                dl->PathLineTo(pt(0));
                dl->PathBezierCubicCurveTo(pt(2), pt(4), pt(6));
                dl->PathBezierCubicCurveTo(pt(8), pt(10), pt(12));
                if (side < 0) std::reverse(dl->_Path.begin(), dl->_Path.end());  // the mirrored half runs the other way round
                dl->PathFillConvex(col);
            }
        }
    }
}

}  // namespace

void ach_art_set(AchToastArt* art, const char* file, ImTextureID tex) {
    struct Slot {
        const char* name;
        SpriteSheet AchToastArt::*sheet;
        int tier, cols, rows, frames;  // tier -1: not one of the three per tier
    };
    // frames: the flip books' FrameCount in Game Bar's XAML (the sheets end in empty cells)
    static const Slot sheets[] = {
        {"TrophyIntro.png", nullptr, 0, 6, 8, 43},      {"TrophyIdle.png", nullptr, 0, 1, 1, 1},        {"TrophyOutro.png", nullptr, 0, 5, 7, 32},
        {"DiamondIntroV2.png", nullptr, 1, 6, 8, 42},   {"DiamondIdleV2.png", nullptr, 1, 4, 5, 20},    {"DiamondOutroV2.png", nullptr, 1, 5, 7, 32},
        {"MythicV2Intro.png", nullptr, 2, 10, 12, 120}, {"MythicV2Idle.png", nullptr, 2, 7, 9, 60},     {"MythicV2Outro.png", nullptr, 2, 6, 7, 41},
        {"MythicV2IntroHighlights.png", &AchToastArt::intro_hi, -1, 10, 12, 120}, {"MythicV2IdleHighlights.png", &AchToastArt::idle_hi, -1, 7, 9, 60},
        {"MythicV2OutroHighlights.png", &AchToastArt::outro_hi, -1, 6, 7, 41},    {"Aurora.png", &AchToastArt::aurora, -1, 7, 9, 60},
    };
    for (const Slot& sl : sheets) {
        if (strcmp(sl.name, file)) continue;
        SpriteSheet sh;
        sh.tex = tex, sh.cols = sl.cols, sh.rows = sl.rows, sh.frames = sl.frames;
        if (sl.sheet) art->*sl.sheet = sh;
        else if (strstr(file, "Intro")) art->intro[sl.tier] = sh;
        else if (strstr(file, "Idle")) art->idle[sl.tier] = sh;
        else art->outro[sl.tier] = sh;
        return;
    }
    if (!strcmp(file, "gleam.png")) art->gleam = tex;
    else if (!strcmp(file, "ShimmerSparkle.png")) art->sparkle = tex;
    else if (!strcmp(file, "Ripple.png")) art->ripple = tex;
    else if (!strcmp(file, "MythicGlowA.png")) art->glow_a = tex;
    else if (!strcmp(file, "MythicGlowB.png")) art->glow_b = tex;
    else if (!strcmp(file, "MythicPulse.png")) art->pulse = tex;
}

const char* const* ach_art_files(int where) {
    static const char* const notifications[] = {"TrophyIntro.png",   "TrophyIdle.png",   "TrophyOutro.png",   "DiamondIntroV2.png", "DiamondIdleV2.png",
                                                "DiamondOutroV2.png", "MythicV2Intro.png", "MythicV2Idle.png", "MythicV2Outro.png",  "MythicV2IntroHighlights.png",
                                                "MythicV2IdleHighlights.png", "MythicV2OutroHighlights.png", nullptr};
    static const char* const game_bar[] = {"Aurora.png", "gleam.png", "ShimmerSparkle.png", "Ripple.png", "MythicGlowA.png", "MythicGlowB.png", "MythicPulse.png", nullptr};
    return where == 0 ? notifications : game_bar;
}

void Guide::ach_toast(AchTier tier, int gamerscore, const std::string& name, const std::string& desc, float percent, int index, double now) {
    AchToast n;
    n.tier = tier, n.g = gamerscore, n.name = name, n.desc = desc, n.percent = percent, n.index = index;
    if (!ach_toast_showing(now) && ach_toasts_.empty()) ach_now_ = n, ach_at_ = now, ach_sound_ = int(tier) + 1;  // at once
    else if (ach_toasts_.size() < 8) ach_toasts_.push_back(n);                                                  // after the ones before it
}

bool Guide::take_ach_sound(AchTier* tier) {
    if (!ach_sound_) return false;
    *tier = AchTier(ach_sound_ - 1), ach_sound_ = 0;
    return true;
}

bool Guide::ach_toast_showing(double now) const { return now - ach_at_ >= 0 && now - ach_at_ < ach_toast_seconds(ach_now_.tier, ach_style_); }

void Guide::draw_ach_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now) {
    float t = float(now - ach_at_);
    if ((t < 0 || t > ach_toast_seconds(ach_now_.tier, ach_style_)) && !ach_toasts_.empty()) {
        ach_now_ = ach_toasts_.front(), ach_toasts_.pop_front(), ach_at_ = now, t = 0;
        ach_sound_ = int(ach_now_.tier) + 1;
    }
    if (t < 0 || t > ach_toast_seconds(ach_now_.tier, ach_style_)) return;
    float lift = now - toast_at_ >= 0 && now - toast_at_ < 6.3 ? 74.0f : 0.0f;  // an ordinary toast showing: this one stands above it
    ach_lift_ += (lift - ach_lift_) * 0.2f;
    if (ach_style_ == 1) return draw_ach_toast_360(dl, a, s, origin, t);
    if (ach_style_ == 2) return draw_ach_toast_one(dl, a, s, origin, t);
    const AchToast& n = ach_now_;
    const int tier = int(n.tier);
    const bool rare = tier == 1, mythic = tier == 2;
    const AchToastArt& art = a.ach;
    const float TR = 4.0f, TS = mythic ? 8.0f : 7.133f, TH = hide_at(tier), TE = 2.433f;
    const float h = t - TH;  // into Hide
    const bool hiding = h >= 0;
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };

    // the Guide's toast: a 64-unit square and a panel as wide as its words (here the widest of its three turns), centred, at y 600
    const float Q = 64, k = Q / 100, TSZ = 17;  // k: Game Bar pixels (its circle is 100 px) to canvas units
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    std::string title = mythic ? "Mythic Achievement unlocked" : rare ? "Rare Achievement unlocked" : "Achievement unlocked";
    if (rare && n.percent >= 0) {
        char pc[24];
        snprintf(pc, sizeof pc, " - %.2f%%", n.percent);
        title += pc;
    }
    std::string second = n.g >= 0 ? std::to_string(n.g) + " - " + n.name : n.name;
    const float gmark = n.g >= 0 ? TSZ * 0.82f + 6 : 0;
    const float ed = 18, gap = 6;  // the hint's emblem
    float wa = a.text->CalcTextSizeA(TSZ, FLT_MAX, 0, "Hold").x, wb = a.text->CalcTextSizeA(TSZ, FLT_MAX, 0, "to open").x;
    float words = std::max({bf->CalcTextSizeA(TSZ, FLT_MAX, 0, title.c_str()).x, a.text->CalcTextSizeA(TSZ, FLT_MAX, 0, second.c_str()).x + gmark,
                            std::min(380.0f, a.text->CalcTextSizeA(TSZ, FLT_MAX, 0, n.desc.c_str()).x), wa + gap + ed + gap + wb});
    const float pw = std::clamp(std::ceil(words) + 22 + 12, 220.0f, 440.0f), half = pw * 0.5f, area = pw - 22 - 12;
    const float bx = 640 - (Q + pw) * 0.5f, by = 600 - ach_lift_;

    const ImU32 sq_col = ACCENT, light_col = mix(ACCENT, WHITE, 0.5f), medium_col = mix(ACCENT, WHITE, 0.25f), dark_col = mix(ACCENT, BLACK, 0.3f);
    const ImU32 ring_col[4] = {light_col, medium_col, dark_col, medium_col};

    float unfold = !hiding ? track({kf(1.433f, 0), kfeo(2.433f, 1)}, t) : h < 0.667f ? track({kf(0, 1), kfei(0.667f, 0)}, h) : 0;
    float shift = half * (1 - unfold);
    ImVec2 ic = at(bx + Q / 2 + shift, by + Q / 2), mid = at(640, by + Q / 2);
    float bg_scale = !hiding ? track({kf(0.667f, 0), kfs(1.433f, 1, .36f, .12f, .5f, 1)}, t) : 1;
    float bg_op = !hiding ? track({kf(0, 0), kf(0.167f, 1)}, t) : h < 0.8f ? track({kf(0, 1), kfei(0.8f, 0)}, h) : 0;
    auto square = [&](ImVec2 c, float side, ImU32 col) {
        if (side > 0.5f) dl->AddRectFilled(ImVec2(c.x - side / 2, c.y - side / 2), ImVec2(c.x + side / 2, c.y + side / 2), col);
    };

    // the mythic's aurora haze, behind everything: Game Bar's 260 x 120 cells have a 146 x 30 cut-out with the haze brightest at its edge;
    // here the cut-out is fitted just inside the square and the panel, so the haze rises from their edges (8 frames a second, there and
    // back; it fades in 2.433-3.433 s and on Hide shrinks with the toast and fades)
    if (mythic && art.aurora.tex) {
        float op = !hiding ? track({kf(2.433f, 0), kfeio(3.433f, 1)}, t) : track({kf(0, 1), kfeio(0.75f, 0)}, h);
        if (op > 0.004f) {
            float w = Q + pw * std::max(unfold, 0.0f), fx = w * 0.97f / 146, fy = Q * 0.94f / 30;
            int i = int(t * 8) % 118, f = i < 60 ? i : 118 - i;
            cell(dl, art.aurora, f, at(640, by + Q / 2), 260 * fx * s, 120 * fy * s, alpha(WHITE, op));
        }
    }

    // the squares, at the toast's centre (behind the panel: on Hide they are under it while it folds)
    if (!hiding) {
        static const float delay[4] = {0, 0.1f, 0.2f, 0.3f}, gone[4] = {0.967f, 1.067f, 1.167f, 1.433f};
        for (int i = 0; i < 4; i++) {
            if (t >= gone[i]) continue;
            float d0 = delay[i];
            float sc = i == 0 ? track({kf(0, 0), kfs(0.533f, 1.12f, .54f, 0, .81f, 1), kfs(0.867f, 1, .36f, .12f, .5f, 1)}, t)
                              : track({kf(0, 0), kfs(d0, 0, .71f, 0, 1, 1), kfs(d0 + 0.533f, 1.12f, .54f, 0, .81f, 1), kfs(d0 + 0.867f, 1, .36f, .12f, .5f, 1)}, t);
            square(mid, Q * s * sc, ring_col[i]);
        }
    } else {
        if (h >= 0.933f && h < 1.633f) square(mid, Q * s * track({kf(0.933f, 0.97f), kfbi(1.633f, 0, 0.5f)}, h), ring_col[0]);
        if (h >= 0.9f && h < 1.5f) square(mid, Q * s * track({kf(0.9f, 0.97f), kfbi(1.5f, 0, 0.5f)}, h), ring_col[1]);
        if (h >= 0.267f && h < 1.366f) square(mid, Q * s * track({kf(0.866f, 1), kfei(1.366f, 0)}, h), ring_col[2]);
    }

    // the panel, and what runs over it
    ImVec2 p0 = at(bx + Q + shift, by), p1 = at(bx + Q + pw - shift, by + Q);
    if (unfold > 0.002f) {
        dl->AddRectFilled(p0, p1, TOAST);
        dl->PushClipRect(p0, p1, true);
        float tg = mythic ? TE + 2.631f : TE, len = mythic ? 3.95f : 3.0f, tau = (t - tg) * 1.3f;  // twice, 1.3 times as fast
        if ((rare || mythic) && tau >= 0 && tau < 2 * len) {
            float u = fmodf(tau, len), pc = bx + Q + half, ps = pw / 630;  // Game Bar's pill is 630 px: its places scale to the panel
            if (art.gleam && u >= 1 && u < 3) {
                float gx = track({kf(1, -100), kf(3, 100)}, u), gop = track({kf(1, 0), kf(2, 0.8f), kf(3, 0)}, u);
                dl->AddImage(art.gleam, at(pc + (gx - 215) * ps, by), at(pc + (gx + 215) * ps, by + Q), ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, gop * 0.6f));
            }
            if (mythic && art.sparkle) {
                // six sparkles on the panel's top and bottom edges, each in its own time
                static const float sp[6][7] = {{-122.5f, 0, 1, 1.85f, 2.75f, 2, 1.6f},   {-89, 1, 1.3f, 2.1f, 2.9f, 1.6f, 2.4f}, {-15, 0, 1.4f, 2.35f, 3.3f, 3, 3},
                                               {23, 1, 1.75f, 2.55f, 3.4f, 2.2f, 1.6f}, {203, 0, 1.95f, 2.85f, 3.75f, 2.4f, 1.8f}, {228, 1, 2.2f, 3.05f, 3.95f, 2.7f, 3}};
                dl->PopClipRect();
                for (const float* q : sp) {
                    if (u < q[2] || u >= q[4]) continue;
                    float op = track({kf(q[2], 0), kf(q[3], 1), kf(q[4], 0)}, u) * unfold;
                    float sx = track({kf(q[2], 0.3f), kfei(q[3], q[5]), kf(q[4], 0.3f)}, u), sy = track({kf(q[2], 0.3f), kfei(q[3], q[6]), kf(q[4], 0.3f)}, u);
                    picture(dl, art.sparkle, at(pc + q[0] * ps, by + q[1] * Q), 14 * k * s * sx, 24 * k * s * sy, alpha(WHITE, op));
                }
                dl->PushClipRect(p0, p1, true);
            }
        }

        // the words: the title in the accent over the Gamerscore and name; from 4 s the description; from 7.133 s (8 s) the hint
        const float x = bx + Q + 22 - 0.6f * half * (1 - unfold);
        float text_op = !hiding && t < TS ? track({kf(1.733f, 0), kf(2.333f, 1)}, t) : 0;
        if (text_op > 0.004f) {
            float dx = track({kf(1.333f, 60), kfeo(2.333f, 0)}, t), rt = t - TR;
            float front = rt < 0 ? 0 : track({kf(0, 0), kfei(0.267f, -Q)}, rt), back = rt < 0 ? Q : track({kf(0, Q), kf(0.233f, Q), kfeo(0.833f, 0)}, rt);
            ImU32 col = alpha(WHITE, text_op);
            if (front > -Q + 1) {
                text_fit(dl, bf, a.bold_m, TSZ * s, at(x + dx, 0).x, at(0, by + 21 + front).y, area * s, alpha(ACCENT, text_op), title);
                float y2 = at(0, by + 43 + front).y, x2 = at(x + dx, 0).x;
                if (n.g >= 0) {
                    // the Gamerscore mark: a white disc with the G cut out
                    float gd = TSZ * 0.82f * s;
                    dl->AddCircleFilled(ImVec2(x2 + gd / 2, y2), gd / 2, col, 32);
                    float gsz = gd * 0.66f, gw = bf->CalcTextSizeA(gsz, FLT_MAX, 0, "G").x;
                    dl->AddText(bf, gsz, ImVec2(x2 + gd / 2 - gw / 2, caps_top(a.bold_m, gsz, y2)), alpha(TOAST, text_op), "G");
                    x2 += gd + 6 * s;
                }
                text_fit(dl, a.text, a.text_m, TSZ * s, x2, y2, (area - gmark) * s, col, second);
            }
            if (back < Q - 1 && !n.desc.empty()) {
                // two lines at most, the second cut with an ellipsis
                const char* b = n.desc.c_str();
                const char* e = b + n.desc.size();
                const char* br = a.text->CalcWordWrapPosition(TSZ, b, e, area);
                if (br == b) br = e;
                std::string first(b, br), rest(br < e && *br == ' ' ? br + 1 : br, e);
                float xs = at(x + dx, 0).x;
                text_mid(dl, a.text, a.text_m, TSZ * s, xs, at(0, by + (rest.empty() ? 32 : 21) + back).y, col, first.c_str());
                if (!rest.empty()) text_fit(dl, a.text, a.text_m, TSZ * s, xs, at(0, by + 43 + back).y, area * s, col, rest);
            }
        }
        float hint_op = 0, hint_dx = 0;
        if (!hiding && t >= TS) hint_op = track({kf(0.066f, 0), kf(0.267f, 1)}, t - TS);
        else if (hiding) hint_op = track({kf(0, 1), kfei(0.5f, 0)}, h), hint_dx = track({kf(0, 0), kfei(0.6f, -40)}, h);
        if (hint_op > 0.004f) {
            // "Hold (a gamepad on a white disc) to open", in the middle of the panel
            float w = wa + gap + ed + gap + wb, hx = bx + Q + half - w / 2 + hint_dx;
            float cy = at(0, by + Q / 2).y;
            text_mid(dl, a.text, a.text_m, TSZ * s, at(hx, 0).x, cy, alpha(WHITE, hint_op), "Hold");
            emblem(dl, a, ImVec2(at(hx + wa + gap + ed / 2, 0).x, cy), ed * s, TOAST, 0, hint_op, I_GAMES);  // the button: a gamepad (user 2026-11-11)
            text_mid(dl, a.text, a.text_m, TSZ * s, at(hx + wa + gap + ed + gap, 0).x, cy, alpha(WHITE, hint_op), "to open");
        }
        dl->PopClipRect();
    }

    // the icon's square
    if (bg_op > 0.004f) square(ic, Q * s * bg_scale, alpha(sq_col, bg_op));

    // the mythic's pulses behind the icon (two, 0.35 s apart, from 2.433 s)
    if (mythic && art.pulse)
        for (int j = 0; j < 2; j++) {
            float u = t - TE - 0.35f * j;
            if (u < 0 || u >= 1.2f) continue;
            float sc = track({kf(0, 0.6f), kfs(0.667f, 1.35f, .6f, 0, .1f, 1)}, u), op = track({kf(0, 0), kf(0.1f, 0.9f), kf(0.667f, 0.7f), kf(1.2f, 0)}, u);
            picture(dl, art.pulse, ic, 130 * k * s * sc, 130 * k * s * sc, alpha(WHITE, op));
        }

    // the icon: its intro (the emblem's frames replaced), its loop, and from Hide its exit
    const float cellq = mythic ? Q * 0.86f * 160 / 105 : (rare ? 84.0f : 68.0f) * k;  // the mythic's ring (105 of 160 px) inside the square
    const int x_end = rare ? 11 : 12, x_start = rare ? 21 : 20;                      // the console's emblem: intro before x_end, exit from x_start
    const float held = 38 / 30.0f;                                                   // frame 1 held for 36 more frames (plain, rare)
    const float disc = 60 * k;                                                       // the sheets' disc
    float iscale = !hiding ? track({kf(0.7f, 0), kfs(1.067f, 1.2f, .52f, 0, .59f, 1), kfs(1.333f, 1, .34f, .05f, .69f, 1)}, t)
                           : track({kf(0.767f, 1), kfei(1.367f, 0)}, h);
    float iop = !hiding ? track({kf(0, 0), kfei(0.667f, 1)}, t) : 1;
    const SpriteSheet &in = art.intro[tier], &loop = art.idle[tier], &out = art.outro[tier];
    const bool have = in.tex && loop.tex && out.tex;
    auto sheet = [&](const SpriteSheet& sh, const SpriteSheet& hi, int f, float op) {
        float side = cellq * s * iscale;
        cell(dl, sh, f, ic, side, side, alpha(WHITE, op));
        if (mythic) cell(dl, hi, f, ic, side, side, alpha(WHITE, op));
    };
    const uint32_t glyph = mythic ? 0xe65fu : rare ? 0xead5u : uint32_t(I_TROPHY);  // without the art: "auto_awesome", "diamond", the trophy
    float in_op = !hiding ? iop * (mythic ? (t < 4.0f ? 1.0f : 0.0f) : track({kf(2.6f, 1), kf(2.7f, 0)}, t)) : 0;
    float loop_op = (!hiding ? (mythic ? (t >= 4.0f ? 1.0f : 0.0f) : track({kf(2.5f, 0), kf(2.6f, 1)}, t)) : track({kf(0, 1), kfei(0.5f, 0)}, h)) * iop;
    float loop_from = mythic ? 4.0f : 2.5f;
    if (in_op > 0.004f) {
        if (mythic && have) {
            sheet(in, art.intro_hi, std::min(int(t * 30), in.frames - 1), in_op);
        } else {
            int f = t < 1 / 30.0f ? 0 : t < held ? 1 : 2 + int((t - held) * 30);
            if (f < x_end) {
                // the emblem faces until frame 2, then turns away to the right by frame x_end (1.6 s: the sounds' peak)
                emblem(dl, a, ic, disc * s * iscale, sq_col, std::clamp((t - held) * 30 / float(x_end - 2), 0.0f, 1.0f), in_op);
            } else if (have) {
                sheet(in, art.intro_hi, std::min(f, in.frames - 1), in_op);
            } else {
                float g = std::clamp((t - (held + (x_end - 2) / 30.0f)) / 0.35f, 0.0f, 1.0f);
                icon_mid(dl, a, disc * s * iscale * css_bezier(.2f, .8f, .3f, 1.2f, g), ic.x, ic.y, alpha(WHITE, in_op), glyph);
            }
        }
    }
    if (loop_op > 0.004f && t >= loop_from) {
        if (have) sheet(loop, art.idle_hi, int((t - loop_from) * 30) % std::max(1, loop.frames), loop_op);
        else icon_mid(dl, a, disc * s * iscale, ic.x, ic.y, alpha(WHITE, loop_op), glyph);
    }
    if (hiding && iscale > 0.003f) {
        int f = std::min(int(h * 30), have ? out.frames - 1 : 31);
        if (!mythic && f >= x_start) {
            // the emblem turns in from the left by frame 31 and stays
            emblem(dl, a, ic, disc * s * iscale, sq_col, -(1 - std::clamp((h * 30 - x_start) / float(31 - x_start), 0.0f, 1.0f)), 1);
        } else if (have) {
            sheet(out, art.outro_hi, f, 1);
        } else {
            float g = 1 - std::clamp(h / 0.6f, 0.0f, 1.0f);
            icon_mid(dl, a, disc * s * iscale * g, ic.x, ic.y, alpha(WHITE, g), glyph);
        }
    }

    // over the mythic's icon: the ripple (from 4.433 s) and the two glows (from 2.433 s, turning and breathing; on Hide they shrink and go)
    if (mythic) {
        float u = t - TE - 2;
        if (art.ripple && u >= 0 && u < 1.5f) {
            float sc = track({kf(0, 0), kfs(1.5f, 1.2f, 0, 0, 0, 1)}, u), op = track({kf(0, 1), kfs(1.4f, 0, .4f, 0, .2f, 1)}, u);
            picture(dl, art.ripple, ic, 200 * k * s * sc, 200 * k * s * sc, alpha(WHITE, op));
        }
        float g = t - TE;
        float layer = !hiding ? track({kf(0, 0), kfs(4.5f, 0.8f, .4f, 0, .2f, 1)}, g) : track({kf(0, 0.8f), kfei(0.733f, 0)}, h);
        float gsc = !hiding ? 1 : track({kf(0, 1), kfeio(0.733f, 0.75f)}, h);
        if (g > 0 && layer > 0.004f) {
            turned(dl, art.glow_a, ic, 270 * k * s * gsc * (0.92f + 0.16f * back_and_forth(g, 2.5f)), g / 9 * 2 * PI,
                   alpha(WHITE, layer * (0.55f + 0.3f * back_and_forth(g, 3.3f))));
            turned(dl, art.glow_b, ic, 270 * k * s * gsc * (1.08f - 0.16f * back_and_forth(g, 2.0f)), -g / 7 * 2 * PI,
                   alpha(WHITE, layer * (1 - 0.45f * back_and_forth(g, 2.3f))));
        }
    }
}

// ---- the Xbox 360's notification (Settings > Notifications > Xbox 360)
//
// The 360's own popup, from its layout and timeline (xam's notify.xur, the scr_Notification visual: 241 frames at 60 a second, in its
// 853x480 units): a ring of four quadrants pops out of nothing over a dark disc while a green burst spreads from it, a pill unrolls to
// the right of it and overshoots, the player's quadrant lights, and two lines of text fade in; while it holds, what is in the ring takes
// turns; then it all goes the way it came. The pill, the ring and the burst are drawn here from the measured figures (the pill 60,60,60
// with a darker rim and a light halo, 44.7 high, a half circle at its left and 12-unit corners at its right; the ring 35.7 across and
// 4.2 thick with 5.3-unit gaps). Where the 360 shows its logo, the Guide shows its own emblem.
//
// The 360 had one popup. The rare and the mythic ones are made for it here in its own language: the rare one in ice blue with a diamond
// and a sheen that crosses the pill; the mythic one in gold, its four quadrants lighting in turn as the console's ring does, a second
// burst, and two more turns of the hold, so that it stays for the whole of its sound (nine seconds).
namespace {

struct NK {
    float f;   // the frame the key is on
    char how;  // to the next key: 'n' held, 'l' linear, 'e' the timeline's ease (out)
    float v;
};

// The timeline's ease with its parameters 0, 100, 50: full speed for the first half of the time, then slowing to a stop.
float ease360(float t) { return t <= 0.5f ? t / 0.75f : (0.75f - (1 - t) * (1 - t)) / 0.75f; }

template <size_t N>
float key(const NK (&k)[N], float f) {
    size_t i = 0;
    while (i + 1 < N && k[i + 1].f <= f) i++;
    if (k[i].how == 'n' || i + 1 == N || f <= k[i].f) return k[i].v;
    float t = (f - k[i].f) / (k[i + 1].f - k[i].f);
    return k[i].v + (k[i + 1].v - k[i].v) * (k[i].how == 'e' ? ease360(t) : t);
}

// notify.xur's tracks: the pill (bgCurve1: its width and where its right end is moved to, its opacity, which its left cap shares), the
// dark disc (logoback), the ring (bg), the burst (explosion), the logo's place (xenonLogo1), the icon (Image) and the text.
const NK PILL_SX[] = {{0, 'n', 0.1f}, {23, 'l', 0.1f}, {27, 'l', 0.1f}, {44, 'l', 1.194f}, {51, 'n', 1}, {228, 'l', 1}, {240, 'n', 0.076f}};
const NK PILL_DX[] = {{0, 'n', 29.064f}, {23, 'l', 29.064f}, {27, 'l', 29.074f}, {44, 'l', -5.928f}, {51, 'n', 0.262f}, {228, 'l', 0.287f}, {240, 'n', 29.852f}};
const NK PILL_A[] = {{0, 'n', 0}, {23, 'l', 0}, {27, 'n', 0.95f}, {228, 'l', 0.95f}, {240, 'n', 0}};
const NK BACK_S[] = {{0, 'e', 0.1f}, {28, 'l', 1.45f}, {39, 'n', 1}, {196, 'l', 1}, {210, 'n', 1.4f}, {215, 'l', 1.4f}, {233, 'n', 0.2f}};
const NK BACK_A[] = {{0, 'e', 0}, {28, 'l', 1}, {39, 'n', 1}, {196, 'l', 1}, {210, 'n', 0.682f}, {215, 'l', 0.682f}, {233, 'n', 0}};
const NK QUAD_S[] = {{0, 'e', 0.154f}, {28, 'l', 1.6f}, {39, 'n', 1}, {196, 'l', 1}, {210, 'n', 1.45f}, {215, 'l', 1.45f}, {233, 'n', 0.2f}};
const NK QUAD_A[] = {{0, 'e', 0}, {28, 'n', 1}, {233, 'n', 0}};
const NK BURST_S[] = {{0, 'n', 0.09f}, {5, 'e', 0.09f}, {37, 'l', 1.12f}, {43, 'l', 0.9f}, {48, 'n', 0.718f}};
const NK BURST_A[] = {{0, 'n', 0}, {5, 'e', 0.5f}, {37, 'l', 1}, {43, 'l', 0.3f}, {48, 'n', 0}, {186, 'l', 0}, {205, 'l', 0.5f}, {210, 'n', 0}};
const NK LOGO_S[] = {{0, 'n', 0.1f}, {5, 'e', 0.1f}, {37, 'l', 1.5f}, {43, 'n', 0.95f}, {222, 'l', 0.95f}, {233, 'n', 0.2f}};
const NK LOGO_A[] = {{0, 'n', 0},   {5, 'e', 0.5f}, {37, 'l', 1},  {43, 'n', 1},  {60, 'l', 1},  {65, 'n', 0}, {125, 'l', 0},
                     {130, 'n', 1}, {180, 'l', 1},  {185, 'n', 0}, {195, 'l', 0}, {210, 'n', 1}, {233, 'n', 0}};
const NK ICON_A[] = {{0, 'n', 0}, {65, 'l', 0}, {70, 'n', 1}, {120, 'l', 1}, {125, 'n', 0}, {186, 'l', 1}, {192, 'n', 0}};
const NK TEXT_A[] = {{0, 'n', 0}, {38, 'l', 0}, {44, 'n', 1}, {217, 'l', 1}, {223, 'n', 0}};

// The hold is the timeline's loop (frames 65 to 185), gone round once more (three times more for the mythic one: ten seconds in
// all, a second longer than its sound) before the way out from 186.
int loops360(int tier) { return tier == 2 ? 3 : 1; }
float frames360(int tier) { return 185 + 120.0f * loops360(tier) + 54; }

ImU32 rgb(int r, int g, int b, float op) { return IM_COL32(r, g, b, int(255 * std::clamp(op, 0.0f, 1.0f) + 0.5f)); }

}  // namespace

void Guide::draw_ach_toast_360(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, float t) {
    const AchToast& n = ach_now_;
    const int tier = int(n.tier);
    const bool rare = tier == 1, mythic = tier == 2;
    // where the timeline stands
    const float F = t * 60, out_from = 185 + 120.0f * loops360(tier);
    const float f = F <= 185 ? F : F < out_from ? 65 + fmodf(F - 185, 120) : std::min(186 + (F - out_from), 240.0f);
    const bool leaving = F >= out_from;

    // the tier's colours: the lit quadrant, the burst's inner and outer glow, the pill's halo, the first line, the emblem's ink
    struct Tint { int lit[3], in[3], out[3], halo[3], title[3], ink[3]; };
    static const Tint TINT[3] = {{{165, 232, 55}, {15, 235, 15}, {130, 235, 15}, {235, 235, 235}, {235, 235, 235}, {40, 130, 20}},
                                 {{120, 215, 255}, {20, 150, 255}, {110, 210, 255}, {150, 215, 255}, {170, 225, 255}, {20, 105, 170}},
                                 {{255, 210, 80}, {255, 160, 10}, {255, 215, 90}, {255, 205, 90}, {255, 222, 130}, {150, 95, 0}}};
    const Tint& c = TINT[tier];
    auto col = [&](const int v[3], float op) { return rgb(v[0], v[1], v[2], op); };

    // the 360's units to pixels: its popup is centred on the screen, and stands where the Guide's toasts do (canvas y 600 to 664)
    const float lift = ach_lift_;
    const float CX = 229.7f, CY = 374.8f, U = 1.5f * s;
    auto P = [&](float x, float y) { return ImVec2(origin.x + x * 1.5f * s, origin.y + (632 - lift + (y - CY) * 1.5f) * s); };
    const ImVec2 oc = P(CX, CY);

    // the pill: a half circle round the ring at its left, 12-unit corners at its right end, which the timeline moves
    const float pa = key(PILL_A, f), HALF = 22.35f;
    const float xr = 196.667f + (645.7f - 196.667f) * key(PILL_SX, f) + key(PILL_DX, f);
    auto pill = [&](float d) {
        float r = HALF - d, rc = std::max(1.0f, std::min(12.0f, (xr - CX) * 0.5f) - d);
        dl->PathArcTo(oc, r * U, PI * 0.5f, PI * 1.5f, 24);
        dl->PathArcTo(P(xr - d - rc, CY - r + rc), rc * U, -PI * 0.5f, 0, 8);
        dl->PathArcTo(P(xr - d - rc, CY + r - rc), rc * U, 0, PI * 0.5f, 8);
    };
    if (pa > 0.004f && xr > CX + 4) {
        // the halo: the figure's light edge (about 120 of 255 on the edge itself, 50 just outside it, gone 4.5 units out)
        const float halo[2][2] = {{9, 20}, {5, 30}};  // a stroke's width and its share of 255
        for (auto& h : halo) pill(0), dl->PathStroke(col(c.halo, pa * h[1] / 255), ImDrawFlags_Closed, h[0] * U);
        pill(0), dl->PathFillConvex(rgb(44, 44, 44, pa));
        pill(1.2f), dl->PathFillConvex(rgb(52, 52, 52, pa));
        pill(2.6f), dl->PathFillConvex(rgb(60, 60, 60, pa));
        pill(-0.4f), dl->PathStroke(col(c.halo, pa * 0.47f), ImDrawFlags_Closed, 1.1f * U);
        // the rare and the mythic ones: a sheen crosses the pill early in each turn of the hold
        float g = (f - 72) / 40;
        if (tier > 0 && g > 0 && g < 1 && !leaving) {
            float x0 = CX + HALF + 4, x1 = xr - 12, sx = x0 + (x1 - x0 + 60) * g - 30, peak = 0.16f * sinf(g * PI) * pa;
            ImU32 on = col(c.title, peak), off = col(c.title, 0);
            dl->PushClipRect(P(x0, CY - HALF + 2.6f), P(x1, CY + HALF - 2.6f), true);
            dl->AddRectFilledMultiColor(P(sx - 30, CY - HALF), P(sx, CY + HALF), off, on, on, off);
            dl->AddRectFilledMultiColor(P(sx, CY - HALF), P(sx + 30, CY + HALF), on, off, off, on);
            dl->PopClipRect();
        }
    }

    // the dark disc under the ring: 15,15,15 out to 18.4 units, gone by 21
    const float bs = key(BACK_S, f), ba = key(BACK_A, f);
    if (ba > 0.004f) {
        dl->AddCircleFilled(oc, 18.4f * bs * U, rgb(15, 15, 15, ba), 48);
        dl->AddCircle(oc, 19.3f * bs * U, rgb(15, 15, 15, ba * 0.6f), 48, 1.8f * bs * U);
        dl->AddCircle(oc, 20.5f * bs * U, rgb(15, 15, 15, ba * 0.25f), 48, 1.4f * bs * U);
    }

    // the ring: four quadrants, lighter at the top left than at the bottom right; the lit ones in the tier's colour. The player's is the
    // top left one (from frame 44 to 192); the mythic one lights all four in turn, a quarter of a second each
    const float rs = key(QUAD_S, f), ra = key(QUAD_A, f);
    if (ra > 0.004f) {
        static const int grey[4] = {134, 114, 96, 114};  // top left, top right, bottom right, bottom left
        const float gap = 0.169f;                        // half a 5.3-unit gap at the ring's middle, as an angle
        int lit = f >= 44 && f < 192 ? (mythic ? int((f - 44) / 15) % 4 : 0) : -1;
        for (int q = 0; q < 4; q++) {
            float a0 = PI + q * PI * 0.5f + gap, a1 = PI + (q + 1) * PI * 0.5f - gap;
            dl->PathArcTo(oc, 15.7f * rs * U, a0, a1, 12);
            dl->PathStroke(q == lit ? col(c.lit, ra) : rgb(grey[q], grey[q], grey[q], ra), 0, 4.2f * rs * U);
        }
    }

    // the burst: a dark middle inside a glow that spreads and thins (the tier's colours where the 360 has 15,235,15 and 130,235,15).
    // The mythic one has a second, at the start of its last turn of the hold
    auto burst = [&](float g) {
        float sc = key(BURST_S, g), op = key(BURST_A, g);
        if (op <= 0.004f) return;
        dl->AddCircleFilled(oc, 14.0f * sc * U, rgb(15, 15, 15, op), 48);
        dl->AddCircle(oc, 16.6f * sc * U, col(c.in, op * 150 / 255), 48, 2.6f * sc * U);
        dl->AddCircle(oc, 18.7f * sc * U, col(c.out, op * 100 / 255), 48, 2.4f * sc * U);
        dl->AddCircle(oc, 20.4f * sc * U, col(c.in, op * 45 / 255), 48, 1.8f * sc * U);
    };
    burst(f);
    if (mythic && F >= out_from - 120 && !leaving) burst(F - (out_from - 120) + 5);

    // in the ring, in turns: the Guide's emblem (a light disc with the tier's mark cut out of it: the trophy, a diamond, a star burst),
    // where the 360 has its logo; then the trophy alone, where the 360 has the achievement's picture
    const uint32_t mark = mythic ? 0xe65fu : rare ? 0xead5u : uint32_t(I_TROPHY);
    const float ls = key(LOGO_S, f), la = key(LOGO_A, f), ia = key(ICON_A, f);
    if (la > 0.004f) emblem(dl, a, oc, 28.5f * ls * U, col(c.ink, 1), 0, la, mark);
    if (ia > 0.004f) icon_mid(dl, a, 22.0f * U, oc.x, oc.y, rgb(235, 235, 235, ia), I_TROPHY);

    // the words: the 360's two lines, 14.7 high in 235 from x 255.7 ("Achievement unlocked" and "%sG - %s" are its own); the first line
    // takes the tier's colour, and a rare one says how few players have it
    const float ta = key(TEXT_A, f);
    if (ta > 0.004f) {
        std::string title = mythic ? "Mythic achievement unlocked" : rare ? "Rare achievement unlocked" : "Achievement unlocked";
        if (rare && n.percent >= 0) {
            char pc[24];
            snprintf(pc, sizeof pc, " - %.2f%%", n.percent);
            title += pc;
        }
        std::string second = n.g >= 0 ? std::to_string(n.g) + "G - " + n.name : n.desc.empty() ? n.name : n.name + " - " + n.desc;
        ImVec2 l1 = P(255.667f, 366.1f), l2 = P(255.667f, 383.9f);
        text_fit(dl, a.text, a.text_m, 14.667f * U, l1.x, l1.y, 373 * U, col(c.title, ta), title);
        text_fit(dl, a.text, a.text_m, 14.667f * U, l2.x, l2.y, 373 * U, rgb(235, 235, 235, ta), second);
    }
}

// ---- the Xbox One's toast (Settings > Notifications > Xbox One)
//
// The console's achievement toast, as the Xbox app and Game Bar draw it: four rings ripple out at the centre and the icon pops in, then
// a capsule opens sideways while the icon rides to its left end and the words slide in from the right; part-way through the words turn
// over to the achievement's description, and near the end a hint takes their place; then the capsule closes and the rings shrink away.
// A rare one has laurels and a gleam; the mythic one adds an aurora, a glow, pulses and sparkles to that.
//
// Everything is in Game Bar's pixels (1080 = the screen's height; the capsule is 100 high and at most 630 wide). The geometry, the key
// times and the easings are those of Game Bar's toast panels; the times it sets from code (when the words turn over, when the hint
// shows, when a rare one's gleam starts, when the toast leaves) and the lines' places were measured from 60 frames a second captures of
// the real toasts, and the colours by tier are sampled from console captures (the mythic one's is a pick: there was no capture). The
// icons and the effects are the Xbox app's own files, the ones the Guide's toast has; without them a mark stands in for the icon and the
// effects are left out. The words are in Game Bar's own Segoe Sans Text where Game Bar is installed (assets.cpp), and in Segoe UI
// Semibold, the nearest Windows has to it, where it is not.
namespace {

const float ONE_ROTATE = 3.89f, ONE_HINT = 7.0f, ONE_OPEN = 2.433f, ONE_HIDE = 1.633f, ONE_GLEAM_SPEED = 1.3f;
float one_gleam_cycle(int tier) { return tier == 2 ? 3.95f : 3; }       // the mythic panel's cycle also carries the sparkles
float one_gleam_start(int tier) { return tier == 2 ? 2.631f : 3.64f; }   // the mythic panel's BeginTime; the rare one's is measured
// When Hide starts: a rare one just after its second gleam (8.33 s, measured), the mythic one at 10 s, a plain one as the rare.
float one_hide_at(int tier) { return tier == 2 ? 10.0f : tier ? one_gleam_start(1) + 2 * one_gleam_cycle(1) / ONE_GLEAM_SPEED + 0.08f : 8.33f; }

float seg(float t, float a, float b) { return std::clamp((t - a) / (b - a), 0.0f, 1.0f); }  // how far t is through a..b
float exp_out(float x) { return (1 - expf(-7 * x)) / (1 - expf(-7.0f)); }                    // ExponentialEase, exponent 7
float exp_in(float x) { return 1 - exp_out(1 - x); }
float back_in(float x) { return x * x * x - x * 0.5f * sinf(x * PI); }                       // BackEase in, amplitude 0.5
struct Spline {
    float x1, y1, x2, y2;
};
float bez(float p, Spline s) { return css_bezier(s.x1, s.y1, s.x2, s.y2, p); }
// Scale 0 -> peak -> 1 after delay d: the pop the rings and the icon share, each leg on its own spline.
float one_pop(float t, float d, float up, float settle, float peak, Spline rise, Spline fall) {
    return t < d + up ? peak * bez(seg(t, d, d + up), rise) : peak + (1 - peak) * bez(seg(t, d + up, d + up + settle), fall);
}
// A pulse's opacity: 0 -> 0.9 at 0.1 s -> 0.7 at 0.667 s -> 0 at 1.2 s.
float pulse_alpha(float p) {
    return p < 0.1f ? 0.9f * seg(p, 0, 0.1f) : p < 0.667f ? 0.9f - 0.2f * seg(p, 0.1f, 0.667f) : 0.7f * (1 - seg(p, 0.667f, 1.2f));
}
// A flip book's frame for a tick: 0 to n - 1 once, pausing on frame hold_at for hold_for more ticks.
int held(int tick, int n, int hold_at, int hold_for) {
    if (hold_at >= 0 && tick > hold_at) tick = std::max(hold_at, tick - hold_for);
    return std::clamp(tick, 0, n - 1);
}
// A flip book played forwards then backwards, for ever.
int boomerang(int tick, int n) {
    int p = tick % (2 * n - 2);
    return p < n ? p : 2 * n - 2 - p;
}

// The flip books by tier: the cell's size, the frame the intro pauses on and for how long (30 ticks a second), and when the intro
// hands over to the idle loop (Game Bar's FlipBook settings).
const struct {
    float cell;
    int hold_at, hold_for;
    float idle_at;
} ONE_SPRITES[3] = {{68, 1, 36, 2.6f}, {84, 1, 36, 2.6f}, {160, -1, 0, 4.0f}};

}  // namespace

float ach_toast_seconds(AchTier tier, int style) {
    if (style == 1) return frames360(int(tier)) / 60 + 0.05f;
    if (style == 2) return one_hide_at(int(tier)) + ONE_HIDE + 0.05f;
    return hide_at(int(tier)) + 1.633f + 0.05f;
}

void Guide::draw_ach_toast_one(ImDrawList* dl, const GuideAssets& g, float s, ImVec2 origin, float a) {
    const AchToast& n = ach_now_;
    const int tier = int(n.tier);
    const AchToastArt& art = g.ach;
    static const ImU32 FILL[3] = {IM_COL32(0x1e, 0x9e, 0x3e, 255), IM_COL32(0x8b, 0x1a, 0x5c, 255), IM_COL32(0x0e, 0x8f, 0x7e, 255)};
    // Game Bar's brushes: the capsule and the dark ring; the icon's disc and the medium rings; the light ring
    const ImU32 dark = FILL[tier], mid = mix(dark, WHITE, 0.18f), light = mix(dark, WHITE, 0.4f);
    auto white = [](float op) { return IM_COL32(255, 255, 255, int(255 * std::clamp(op, 0.0f, 1.0f) + 0.5f)); };

    // Game Bar's pixels to the screen's: the toast is centred, its middle 120 above the screen's foot (canvas y 640)
    const float R = 50, U = s * 720 / 1080;
    const ImVec2 C(origin.x + 640 * s, origin.y + (640 - ach_lift_) * s);
    auto P = [&](float x, float y) { return ImVec2(C.x + x * U, C.y + y * U); };
    ImFont* tf = g.text_one ? g.text_one : g.text;
    const FontMetrics& tm = g.text_one ? g.one_m : g.text_m;
    const float TS = 26;  // every line is size 26 in the panel, in a box 34 high with its baseline 27 below the top
    auto width = [&](const std::string& t) { return tf->CalcTextSizeA(TS, FLT_MAX, 0, t.c_str()).x; };
    // a line of text with its baseline at y
    auto line = [&](float x, float y, ImU32 col, const char* b, const char* e = nullptr) {
        ImVec2 p = P(x, y);
        dl->AddText(tf, TS * U, ImVec2(p.x, p.y - TS * U * tm.asc / (tm.asc + tm.desc)), col, b, e);
    };

    // the words: the title over "(G) 20  -  name"; then the description, up to two lines 460 wide; then the hint
    std::string title = tier == 2 ? "Mythic achievement unlocked" : tier ? "Rare achievement unlocked" : "Achievement unlocked";
    if (tier == 1 && n.percent >= 0) {
        char pc[24];
        snprintf(pc, sizeof pc, " - %.2f%%", n.percent);
        title += pc;
    }
    const bool score = n.g >= 0;
    const std::string body = score ? std::to_string(n.g) + "  -  " + n.name : n.name;
    const float mark = score ? 36.5f : 0;  // the Gamerscore mark: a 26-pixel disc with the G cut out, 36.5 ahead of the score
    const char* db = n.desc.c_str();
    const char* de = db + n.desc.size();
    const char* dbr = tf->CalcWordWrapPosition(TS, db, de, 460);
    if (dbr == db) dbr = de;
    const std::string back1(db, dbr), back2(dbr < de && *dbr == ' ' ? dbr + 1 : dbr, de);
    const float hw_a = width("Hold"), hw_b = width("to open"), hgap = 8, hed = 27;  // the hint: "Hold (a gamepad on a disc) to open"
    const float hint_w = hw_a + hgap + hed + hgap + hw_b;
    // the capsule fits its widest words and keeps that width: the icon 100, a gap of 18, up to 460 of words, 50 to its right end
    const float words = std::max({width(title), width(body) + mark, width(back1), width(back2), hint_w + 2});
    const float full = (118 + std::min(words, 460.0f) + 50) / 2;  // half the open capsule

    const float h = a - one_hide_at(tier);  // seconds into Hide (negative while it shows)
    const float openk = exp_out(seg(a, 1.433f, ONE_OPEN)) * (1 - exp_in(seg(h, 0, 0.667f)));
    const float half = full * openk, ix = -(full - R) * openk;  // half the capsule now; the icon's centre
    const ImVec2 ic = P(ix, 0);

    // the mythic's aurora: a flip book of 260 x 120 cells at 8 a second, forwards then backwards, 624 x 336 behind the toast. It fades
    // in over the second after the capsule opens, and leaves fading and shrinking to 30% over 0.75 s
    if (tier == 2 && a > ONE_OPEN && h < 0.75f) {
        float k = 1 - 0.7f * seg(h, 0, 0.75f);
        cell(dl, art.aurora, boomerang(int(a * 8), 60), C, 624 * k * U, 336 * k * U, white(exp_out(seg(a, ONE_OPEN, 3.433f)) * (1 - exp_in(seg(h, 0, 0.75f)))));
    }

    if (half > 1) {
        // the shadow: Game Bar's is a capsule 5 larger all round in 15,15,15 at 38 of 255, soft over its last 4. It is squeezed
        // sideways with the capsule and fades from 0.3 s to 0.667 s into Hide
        float sa = h < 0 ? 1 : 1 - exp_in(seg(h, 0.3f, 0.667f));
        static const float SH[3][2] = {{5, 16}, {3.5f, 11}, {2, 11}};  // how far out, and its share of 255
        for (auto& l : SH)
            dl->AddRectFilled(P(-(full + l[0]) * openk, -R - l[0]), P((full + l[0]) * openk, R + l[0]), IM_COL32(15, 15, 15, int(l[1] * sa + 0.5f)), (R + l[0]) * U);
        dl->AddRectFilled(P(-half, -R), P(half, R), dark, std::min(R, half) * U);

        // the gleam: its picture fills the words' place less 100 at each end and slides 200 to the right while it fades 0 -> 0.8 -> 0.
        // The cycle runs 1.3 times as fast, twice, the gleam from 1 s to 3 s of it
        float len = one_gleam_cycle(tier), gt = (a - one_gleam_start(tier)) * ONE_GLEAM_SPEED, c = fmodf(gt, len);
        if (tier && h < 0 && gt > 0 && gt < 2 * len) {
            if (art.gleam && c > 1 && c < 3) {
                float gx = -100 + 200 * seg(c, 1, 3), gw = 2 * full - 300;
                dl->AddImage(art.gleam, P(gx - gw / 2, -R), P(gx + gw / 2, R), ImVec2(0, 0), ImVec2(1, 1), white(0.8f * (1 - std::fabs(c - 2))));
            }
            // the mythic's sparkles: six 14 x 24 pictures on the capsule's top and bottom edges, lit left to right behind the gleam
            // (x from the centre, or from the right end of the words' place for the last two)
            const float right = full - R;
            const float SP[6][7] = {{-122.5f, -R, 1, 1.85f, 2.75f, 2, 1.6f},      {-89, R, 1.3f, 2.1f, 2.9f, 1.6f, 2.4f},           {-15, -R, 1.4f, 2.35f, 3.3f, 3, 3},
                                    {23, R, 1.75f, 2.55f, 3.4f, 2.2f, 1.6f},     {right - 112, -R, 1.95f, 2.85f, 3.75f, 2.4f, 1.8f}, {right - 87, R, 2.2f, 3.05f, 3.95f, 2.7f, 3}};
            for (int i = 0; tier == 2 && art.sparkle && i < 6; i++) {
                float k = c < SP[i][3] ? seg(c, SP[i][2], SP[i][3]) : 1 - seg(c, SP[i][3], SP[i][4]);  // 0 -> 1 -> 0
                picture(dl, art.sparkle, P(SP[i][0], SP[i][1]), 14 * (0.3f + (SP[i][5] - 0.3f) * k) * U, 24 * (0.3f + (SP[i][6] - 0.3f) * k) * U, white(k));
            }
        }
    }

    // the rings at the centre, back to front: light, medium, dark, medium
    const Spline RISE{0.54f, 0, 0.81f, 1}, FALL{0.36f, 0.12f, 0.5f, 1};
    const struct {
        float delay, gone;
        ImU32 col;
    } rings[4] = {{0, 0.967f, light}, {0.1f, 1.067f, mid}, {0.2f, 1.167f, dark}, {0.3f, 1.433f, mid}};
    auto disc = [&](ImVec2 c, float r, ImU32 col) {
        if (r * U > 0.25f) dl->AddCircleFilled(c, r * U, col, 64);
    };
    for (auto& rg : rings)
        if (a < rg.gone) disc(C, R * one_pop(a, rg.delay, 0.533f, 0.334f, 1.12f, RISE, FALL), rg.col);
    // on the way out they come back full size under the icon and shrink in turn: dark first, light last
    if (h >= 0.933f) disc(C, R * 0.97f * (1 - back_in(seg(h, 0.933f, 1.633f))), light);
    if (h >= 0.9f && h < 1.5f) disc(C, R * 0.97f * (1 - back_in(seg(h, 0.9f, 1.5f))), mid);
    if (h >= 0.267f && h < 1.366f) disc(C, R * (1 - exp_in(seg(h, 0.866f, 1.366f))), dark);
    // the icon's disc: grows from 0.667 s to 1.433 s; fades over the first 0.8 s of Hide
    disc(ic, R * bez(seg(a, 0.667f, 1.433f), FALL), alpha(mid, 1 - exp_in(seg(h, 0, 0.8f))));

    const float gl = a - 0.7f;  // seconds since the icon started to pop in
    // the mythic's pulses: the 130-pixel star outline grows from 60% to 135% in 0.667 s while it fades; a second follows 0.35 s later
    for (float delay : {0.0f, 0.35f})
        if (tier == 2 && art.pulse && gl > delay) {
            float size = 130 * (0.6f + 0.75f * seg(gl - delay, 0, 0.667f)) * U;
            picture(dl, art.pulse, ic, size, size, white(pulse_alpha(gl - delay)));
        }

    // the icon: nothing until 0.7 s, 120% at 1.067 s, 100% at 1.333 s; on the way out it holds until 0.767 s and is gone by 1.367 s.
    // Its flip books run at 30 a second: the intro from the start (fading in over 0.667 s), the idle loop from idle_at, the outro
    // from Hide; the mythic one has a layer of highlights over each
    const float is = one_pop(a, 0.7f, 0.367f, 0.266f, 1.2f, {0.52f, 0, 0.59f, 1}, {0.34f, 0.05f, 0.69f, 1}) * (1 - exp_in(seg(h, 0.767f, 1.367f)));
    if (is > 0.01f) {
        const SpriteSheet &in = art.intro[tier], &loop = art.idle[tier], &out = art.outro[tier];
        const auto& sp = ONE_SPRITES[tier];
        const float side = sp.cell * is * U;
        if (in.tex && loop.tex && out.tex) {
            int which = h >= 0 ? 2 : a < sp.idle_at ? 0 : 1;
            const SpriteSheet& sh = which == 2 ? out : which == 0 ? in : loop;
            const SpriteSheet& hi = which == 2 ? art.outro_hi : which == 0 ? art.intro_hi : art.idle_hi;
            int i = which == 2 ? held(int(h * 30), sh.frames, -1, 0)
                    : which == 0 ? held(int(a * 30), sh.frames, sp.hold_at, sp.hold_for) : int((a - sp.idle_at) * 30) % std::max(1, sh.frames);
            ImU32 col = white(which == 0 ? exp_in(seg(a, 0, 0.667f)) : 1);
            cell(dl, sh, i, ic, side, side, col);
            if (tier == 2) cell(dl, hi, i, ic, side, side, col);
        } else {
            icon_mid(dl, g, 56 * is * U, ic.x, ic.y, white(0.95f), tier == 2 ? 0xe65fu : tier ? 0xead5u : uint32_t(I_TROPHY));
        }
    }

    if (tier == 2 && gl > 0 && h < 0.733f) {
        // the ripple: 2 s in, the 200-pixel ring grows to 120% over 1.5 s and is gone by 1.4 s
        if (art.ripple && a > 2) picture(dl, art.ripple, ic, 240 * seg(a - 2, 0, 1.5f) * U, 240 * seg(a - 2, 0, 1.5f) * U, white(1 - seg(a - 2, 0, 1.4f)));
        // the glow: two 270-pixel pictures of rays over the icon. A turns once in 9 s, B the other way in 7 s; each breathes between
        // 92% and 108% of its size and 55% and 85% of its opacity. The layer fades in over 0.9 s, eases to 80% by 4.5 s, and leaves
        // fading and shrinking to 75% over 0.733 s
        float out = seg(h, 0, 0.733f), ga = seg(gl, 0, 0.9f) * (1 - 0.2f * seg(gl, 3.3f, 4.5f)) * (1 - exp_in(out));
        float size_a = 270 * (0.92f + 0.16f * back_and_forth(gl, 2.5f)) * (1 - 0.25f * out), size_b = 270 * (1.08f - 0.16f * back_and_forth(gl, 2)) * (1 - 0.25f * out);
        turned(dl, art.glow_a, ic, size_a * U, gl * 40 * PI / 180, white(ga * (0.55f + 0.3f * back_and_forth(gl, 3.3f))));
        turned(dl, art.glow_b, ic, size_b * U, -gl * 2 * PI / 7, white(ga * (0.85f - 0.3f * back_and_forth(gl, 2.3f))));
    }

    if (tier) laurels(dl, ic, U, tier, a, h, light);

    // the words fade in from 1.733 s to 2.333 s while they slide in from 900 to the right. At ONE_ROTATE they leave upwards and the
    // description comes from below. At ONE_HINT they are cut and the hint fades in, in the middle of the place right of the icon and
    // 3 lower; it fades and slides left as the toast hides. Baselines: the two lines 4.1 above and 24.7 below the middle; a
    // description 11.2 below, or 5.7 above and 28.3 below for two lines; the hint 9.7 below
    const bool hinting = a >= ONE_HINT;
    const float top = hinting || h >= 0 ? 0 : seg(a, 1.733f, 2.333f);
    if (half > R) {
        dl->PushClipRect(P(ix + R, -R), P(half - 40, R), true);
        if (top > 0) {
            const float tx = ix + R + 18 + 900 * (1 - exp_out(seg(a, 1.333f, 2.333f)));
            const bool rot = !n.desc.empty();
            const float fy = rot ? -100 * exp_in(seg(a, ONE_ROTATE, ONE_ROTATE + 0.267f)) : 0;       // the two lines leave upwards
            const float by = 100 * (1 - exp_out(seg(a, ONE_ROTATE + 0.233f, ONE_ROTATE + 0.833f)));  // the description arrives from below
            line(tx, -4.1f + fy, white(top), title.c_str());
            if (score) {
                ImVec2 gc = P(tx + 13, 16 + fy);
                dl->AddCircleFilled(gc, 13 * U, white(top), 32);
                ImFont* gf = g.text_bold ? g.text_bold : g.text;
                float gs = 17 * U, gw = gf->CalcTextSizeA(gs, FLT_MAX, 0, "G").x;
                dl->AddText(gf, gs, ImVec2(gc.x - gw / 2, caps_top(g.bold_m, gs, gc.y)), alpha(dark, top), "G");
            }
            line(tx + mark, 24.7f + fy, white(top), body.c_str());
            if (rot) {
                line(tx, (back2.empty() ? 11.2f : -5.7f) + by, white(top), back1.c_str());
                if (!back2.empty()) line(tx, 28.3f + by, white(top), back2.c_str());
            }
        }
        if (hinting) {
            float fade = seg(a, ONE_HINT + 0.066f, ONE_HINT + 0.267f) * (1 - exp_in(seg(h, 0, 0.5f)));
            float hx = 35 - 100 * exp_in(seg(h, 0, 0.6f)) - hint_w / 2;
            line(hx, 9.7f, white(fade), "Hold");
            emblem(dl, g, P(hx + hw_a + hgap + hed / 2, 0.5f), hed * U, dark, 0, fade, I_GAMES);
            line(hx + hw_a + hgap + hed + hgap, 9.7f, white(fade), "to open");
        }
        dl->PopClipRect();
    }
}
