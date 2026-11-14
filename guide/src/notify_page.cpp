// Settings > Notifications: how an unlocked achievement is shown. Guide is the Guide's own toast (achievements.cpp, the default); Xbox
// One is the same toast as the console and the Xbox app draw it; Xbox 360 is the 360's own notification with a rare and a mythic
// version made for it. The choice applies at once; the overlay keeps it for the next start (data\bin\fh2mp-guide-notify.txt).
#include <algorithm>

#include "ui.h"

using namespace ui;

namespace {

// The rows in their order, each with its style (the number the overlay keeps) and what the description pane says of it.
const struct {
    const char* name;
    int style;
    const char* words;
} STYLES[] = {{"Guide", 0, "Guide is the Guide's own achievement toast, in your theme's colour, with its rare and mythic versions."},
              {"Xbox One", 2, "Xbox One is the Xbox One's achievement toast, in the console's colours, with laurels on its rare and mythic versions."},
              {"Xbox 360", 1, "Xbox 360 is the Xbox 360's achievement notification, with rare and mythic versions made for it."}};
const int COUNT = int(sizeof STYLES / sizeof STYLES[0]);

}  // namespace

void Guide::open_notify(double now) {
    notify_row_ = 0;
    for (int i = 0; i < COUNT; i++)
        if (STYLES[i].style == ach_style_) notify_row_ = i;
    notify_bar_.set(float(notify_row_));
    push(Page::Notifications, now);
}

void Guide::handle_notify(PadEvent e, std::vector<GuideRequest>& out, double now) {
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        int r = std::clamp(notify_row_ + (e == PadEvent::Up ? -1 : 1), 0, COUNT - 1);
        if (r != notify_row_) notify_row_ = r, notify_bar_.go(float(r), now, 0.14f);
        break;
    }
    case PadEvent::A: {
        ach_style_ = STYLES[notify_row_].style;
        GuideRequest r;
        r.action = GuideAction::SetNotify, r.id = ach_style_;
        out.push_back(r);
        break;
    }
    case PadEvent::B: pop(now); break;
    default: break;
    }
}

// Laid out as the 360's pages of choices are (OptionsOnlineStatus.xur, OptionsVoice.xur, in its units): a heading 16 high, 19 in, then
// 28-unit rows from 24 under the pane's top, each with a radio button 17.3 across, 16.75 in (185,189,194 round 235, the chosen one's dot
// 15,15,15 and 8 across) and its word 38 in; what the row in focus is on the description pane.
void Guide::draw_notify(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Notifications", title_op);
    float x = L + slide, unit = v.sz(uu(1));
    panel_fill(dl, v.pt(x, 96), v.pt(x + 431, PAGE_B), pop);
    ImVec2 hp = v.pt(x + uu(19), 96 + uu(13));
    text_mid(dl, a.text, a.text_m, v.sz(uu(16)), hp.x, hp.y, alpha(TEXT, pop), "Achievements");
    const float TOP = 96 + uu(24), RH = uu(28);
    dl->AddLine(v.pt(x, TOP - 0.5f), v.pt(x + 431, TOP - 0.5f), alpha(LINE, pop), v.sz(1));
    std::vector<Opt> opts;
    for (auto& st : STYLES) opts.push_back({st.name, 0, true, 0});
    option_list(dl, a, v, x, TOP, 431, RH, uu(16), uu(38), opts, notify_row_, notify_bar_.value(now), pop);
    for (int i = 0; i < COUNT; i++) {
        ImVec2 c = v.pt(x + uu(16.75f + 8.635f), TOP + i * RH + uu(14));
        dl->AddCircleFilled(c, 8.635f * unit, alpha(IM_COL32(185, 189, 194, 225), pop), 32);
        dl->AddCircleFilled(c, 7.135f * unit, alpha(IM_COL32(235, 235, 235, 225), pop), 32);
        if (STYLES[i].style == ach_style_) dl->AddCircleFilled(c, 4.05f * unit, alpha(IM_COL32(15, 15, 15, 225), pop), 24);
    }

    side_fill(dl, v.pt(x + 431, 96), v.pt(x + W, PAGE_B), pop);
    const float ts = uu(17.333f), lh = 21.25f / 17.333f;
    float y = 96 + uu(12);
    y += wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), "Choose how an achievement is shown when you unlock one.") + ts * lh;
    wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), STYLES[std::clamp(notify_row_, 0, COUNT - 1)].words);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
}
