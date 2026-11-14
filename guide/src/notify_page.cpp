// The Settings blade's pages of one choice. Notifications: how an unlocked achievement is shown (Guide is the Guide's own toast,
// achievements.cpp, the default; Xbox One the same toast as the console and the Xbox app draw it; Xbox 360 the 360's own notification
// with a rare and a mythic version made for it). System: how the Guide itself is laid out (Xbox 360, its blades, the default; Xbox One,
// its panel, one.h). A choice applies at once; the overlay keeps it for the next start (data\bin\fh2mp-guide-notify.txt and
// fh2mp-guide-layout.txt).
#include <algorithm>

#include "one.h"
#include "ui.h"

using namespace ui;

namespace {

struct Choice {
    const char* name;
    int value;  // the number the overlay keeps
    const char* words;
};
struct ChoicePage {
    const char *title, *heading, *intro;
    const Choice* list;
    int count;
};
const Choice STYLES[] = {{"Guide", 0, "Guide is the Guide's own achievement toast, in your theme's colour, with its rare and mythic versions."},
                         {"Xbox One", 2, "Xbox One is the Xbox One's achievement toast, in the console's colours, with laurels on its rare and mythic versions."},
                         {"Xbox 360", 1, "Xbox 360 is the Xbox 360's achievement notification, with rare and mythic versions made for it."}};
const Choice LAYOUTS[] = {{"Xbox 360", 0, "Xbox 360 is the Xbox 360's Guide: blades side by side, and pages in its frame."},
                          {"Xbox One", 1, "Xbox One is the Xbox One's guide: a panel at the left of the screen with tabs along its top."}};
const ChoicePage PAGES[2] = {{"Notifications", "Achievements", "Choose how an achievement is shown when you unlock one.", STYLES, 3},
                             {"System", "Guide", "Choose how the Guide is laid out.", LAYOUTS, 2}};

}  // namespace

void Guide::open_choices(int which, double now) {
    choice_page_ = which == 1 ? 1 : 0;
    const ChoicePage& pg = PAGES[choice_page_];
    int chosen = choice_page_ ? guide_style_ : ach_style_;
    notify_row_ = 0;
    for (int i = 0; i < pg.count; i++)
        if (pg.list[i].value == chosen) notify_row_ = i;
    notify_bar_.set(float(notify_row_));
    push(Page::Notifications, now);
}

void Guide::handle_notify(PadEvent e, std::vector<GuideRequest>& out, double now) {
    const ChoicePage& pg = PAGES[choice_page_];
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        int r = std::clamp(notify_row_ + (e == PadEvent::Up ? -1 : 1), 0, pg.count - 1);
        if (r != notify_row_) notify_row_ = r, notify_bar_.go(float(r), now, 0.14f);
        break;
    }
    case PadEvent::A: {
        GuideRequest r;
        r.id = pg.list[notify_row_].value;
        if (choice_page_) set_guide_style(int(r.id)), r.action = GuideAction::SetLayout;
        else ach_style_ = int(r.id), r.action = GuideAction::SetNotify;
        out.push_back(r);
        break;
    }
    case PadEvent::B: pop(now); break;
    default: break;
    }
}

// Laid out as the 360's pages of choices are (OptionsOnlineStatus.xur, OptionsVoice.xur, in its units): a heading 16 high, 19 in, then
// 28-unit rows from 24 under the pane's top, each with a radio button 17.3 across, 16.75 in (185,189,194 round 235, the chosen one's dot
// 15,15,15 and 8 across) and its word 38 in; what the row in focus is on the description pane. In the Xbox One layout: the heading in
// grey, one-line entries with a ring at their right end (the chosen one's filled in the accent), and the words under the list.
void Guide::draw_notify(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    const ChoicePage& pg = PAGES[choice_page_];
    const int chosen = choice_page_ ? guide_style_ : ach_style_, row = std::clamp(notify_row_, 0, pg.count - 1);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    if (guide_style_ == 1) {
        using namespace one;
        Panel p = one_frame(dl, a, origin, s, op, now, pg.title);
        p.text(p.L(PX + 32, PY + LIST + 20), 23, p.lcol(GREY1), pg.heading);
        const float top = PY + LIST + SEG_H;
        p.focus(top + notify_bar_.value(now) * LINE_H, LINE_H);
        for (int i = 0; i < pg.count; i++) {
            p.line(top + i * LINE_H, 0, pg.list[i].name, "", true);
            ImVec2 c = p.L(PX + 490, top + i * LINE_H + LINE_H / 2);
            dl->AddCircle(c, 13 * p.U, p.lcol(WHITE, 0.8f), 32, 2 * p.U);
            if (pg.list[i].value == chosen) dl->AddCircleFilled(c, 7.5f * p.U, p.lcol(ACCENT), 24);
        }
        float y = top + pg.count * LINE_H + 16;
        p.divider(y);
        y += 24;
        y += p.para(PX + 32, y, 23, p.lcol(GREY1), pg.intro, PW - 64) + 16;
        p.para(PX + 32, y, 23, p.lcol(GREY1), pg.list[row].words, PW - 64);
        return p.hints(hints, 2);
    }
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, pg.title, title_op);
    float x = L + slide, unit = v.sz(uu(1));
    panel_fill(dl, v.pt(x, 96), v.pt(x + 431, PAGE_B), pop);
    ImVec2 hp = v.pt(x + uu(19), 96 + uu(13));
    text_mid(dl, a.text, a.text_m, v.sz(uu(16)), hp.x, hp.y, alpha(TEXT, pop), pg.heading);
    const float TOP = 96 + uu(24), RH = uu(28);
    dl->AddLine(v.pt(x, TOP - 0.5f), v.pt(x + 431, TOP - 0.5f), alpha(LINE, pop), v.sz(1));
    std::vector<Opt> opts;
    for (int i = 0; i < pg.count; i++) opts.push_back({pg.list[i].name, 0, true, 0});
    option_list(dl, a, v, x, TOP, 431, RH, uu(16), uu(38), opts, notify_row_, notify_bar_.value(now), pop);
    for (int i = 0; i < pg.count; i++) {
        ImVec2 c = v.pt(x + uu(16.75f + 8.635f), TOP + i * RH + uu(14));
        dl->AddCircleFilled(c, 8.635f * unit, alpha(IM_COL32(185, 189, 194, 225), pop), 32);
        dl->AddCircleFilled(c, 7.135f * unit, alpha(IM_COL32(235, 235, 235, 225), pop), 32);
        if (pg.list[i].value == chosen) dl->AddCircleFilled(c, 4.05f * unit, alpha(IM_COL32(15, 15, 15, 225), pop), 24);
    }

    side_fill(dl, v.pt(x + 431, 96), v.pt(x + W, PAGE_B), pop);
    const float ts = uu(17.333f), lh = 21.25f / 17.333f;
    float y = 96 + uu(12);
    y += wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), pg.intro) + ts * lh;
    wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), pg.list[row].words);
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
}
