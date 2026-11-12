// Settings > Voice: the player's microphone (off = muted, and the game shows them without one), voice chat as a whole (off = nothing is
// heard and nothing is sent) and how loud the others are. Each change applies at once; the overlay keeps it for the next start.
#include <algorithm>

#include "ui.h"

using namespace ui;

void Guide::open_voice(double now) {
    voice_row_ = 0;
    voice_bar_.set(0);
    push(Page::Voice, now);
}

void Guide::handle_voice(PadEvent e, std::vector<GuideRequest>& out, double now) {
    auto changed = [&] {
        GuideRequest r;
        r.action = GuideAction::SetVoice, r.id = (voice_mic_ ? 1 : 0) | (voice_chat_ ? 2 : 0) | (int64_t(voice_volume_) << 2);
        out.push_back(r);
    };
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        int r = std::clamp(voice_row_ + (e == PadEvent::Up ? -1 : 1), 0, 2);
        if (r != voice_row_) voice_row_ = r, voice_bar_.go(float(r), now, 0.14f);
        break;
    }
    case PadEvent::Left:
    case PadEvent::Right:
        if (voice_row_ == 2) {
            int v = std::clamp(voice_volume_ + (e == PadEvent::Left ? -1 : 1), 0, 4);
            if (v != voice_volume_) voice_volume_ = v, changed();
        }
        break;
    case PadEvent::A:
        if (voice_row_ == 0) voice_mic_ = !voice_mic_;
        else if (voice_row_ == 1) voice_chat_ = !voice_chat_;
        else voice_volume_ = (voice_volume_ + 1) % 5;
        changed();
        break;
    case PadEvent::B: pop(now); break;
    default: break;
    }
}

void Guide::draw_voice(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Voice", title_op);
    float x = L + slide;
    ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + W, 607);
    panel_fill(dl, p0, p1, pop);
    dl->PushClipRect(p0, p1, true);
    const float RH = 56, TOP = 100;
    std::vector<Opt> opts = {{"Microphone", 0, true, I_MIC}, {"Voice Chat", 0, true, I_CHAT}, {"Volume", 0, true, I_VOLUME}};
    option_list(dl, a, v, x, TOP, W, RH, 24, 27, opts, voice_row_, voice_bar_.value(now), pop, 30, 34, 14);
    for (int i = 0; i < 3; i++) {
        float cy = TOP + i * RH + RH * 0.5f;
        ImU32 fg = alpha(i == voice_row_ ? INK : TEXT, pop);
        if (i < 2) {
            bool on = i == 0 ? voice_mic_ : voice_chat_;
            ImVec2 tp = v.pt(x + W - 27, cy);
            text_mid(dl, a.text, a.text_m, v.sz(24), tp.x, tp.y, on ? fg : alpha(i == voice_row_ ? INK : MUTED2, pop), on ? "On" : "Off", true);
        } else {
            // five steps, the ones up to the level filled
            for (int k = 0; k < 5; k++) {
                float bx = x + W - 27 - (5 - k) * 22.0f, bh = 10.0f + k * 5.0f;
                ImVec2 b0 = v.pt(bx, cy + 15 - bh), b1 = v.pt(bx + 14, cy + 15);
                if (k <= voice_volume_) dl->AddRectFilled(b0, b1, fg);
                else dl->AddRect(b0, b1, alpha(i == voice_row_ ? INK : MUTED2, pop * 0.7f), 0, 0, v.sz(1));
            }
        }
    }
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}
