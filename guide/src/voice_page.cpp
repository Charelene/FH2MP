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

// Laid out as the 360's own Voice and Notifications pages (OptionsVoice.xur, OptionsNotifications.xur, in its units): the rows on the
// left, each switch a tick box (15 square in 185,189,194 around white, 18.5 in, its tick 51,51,51; the word 40 in), the volume a slider
// (a 272 x 52 block: the word and the level on its first line, the track 254 x 15 from 27.5 down, a 2 x 20 marker where the fill ends;
// the fill takes the accent while the slider has the focus), and on the description pane what the row in focus does (17.3 high, 18 in,
// from 12 down, 247 wide).
void Guide::draw_voice(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Voice", title_op);
    float x = L + slide, unit = v.sz(uu(1));
    ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + 431, PAGE_B);
    panel_fill(dl, p0, p1, pop);
    dl->PushClipRect(p0, p1, true);
    const float RH = 42, TOP = 96;
    std::vector<Opt> opts = {{"Microphone", 0, true, 0}, {"Voice Chat", 0, true, 0}};
    option_list(dl, a, v, x, TOP, 431, RH, uu(16), uu(40), opts, voice_row_ < 2 ? voice_row_ : -1, voice_bar_.value(now), pop);
    for (int i = 0; i < 2; i++) {
        ImVec2 b = v.pt(x + uu(18.46f), TOP + i * RH + uu(7.44f));
        dl->AddRectFilled(b, ImVec2(b.x + 15.03f * unit, b.y + 15.03f * unit), alpha(IM_COL32(185, 189, 194, 255), pop));
        dl->AddRectFilled(ImVec2(b.x + unit, b.y + unit), ImVec2(b.x + 14.03f * unit, b.y + 14.03f * unit), alpha(WHITE, pop));
        if (i == 0 ? voice_mic_ : voice_chat_) {
            ImVec2 t(b.x + 2.96f * unit, b.y + 3.35f * unit);
            ImVec2 tick[3] = {ImVec2(t.x + 0.9f * unit, t.y + 4.4f * unit), ImVec2(t.x + 3.6f * unit, t.y + 7.3f * unit), ImVec2(t.x + 8.0f * unit, t.y + 1.0f * unit)};
            dl->AddPolyline(tick, 3, IM_COL32(51, 51, 51, int(217 * pop)), 0, 1.9f * unit);
        }
    }
    // the volume: five steps, shown as the slider's level 1 to 5
    bool foc = voice_row_ == 2;
    float bt = TOP + 2 * RH + uu(11), frac = (voice_volume_ + 1) / 5.0f;
    dl->AddRectFilled(v.pt(x + uu(8), bt), v.pt(x + uu(280), bt + uu(52)), alpha(IM_COL32(235, 235, 235, 255), pop * 15 / 255));
    ImVec2 lp = v.pt(x + uu(17), bt + uu(12.5f)), vp = v.pt(x + uu(270), bt + uu(12.5f));
    text_mid(dl, a.text, a.text_m, v.sz(uu(16)), lp.x, lp.y, alpha(TEXT, pop), "Volume");
    text_mid(dl, a.text, a.text_m, v.sz(uu(16)), vp.x, vp.y, alpha(TEXT, pop), std::to_string(voice_volume_ + 1).c_str(), true);
    ImVec2 t0 = v.pt(x + uu(17), bt + uu(27.5f)), t1 = v.pt(x + uu(271), bt + uu(42.5f));
    ImU32 th = alpha(blend(DISABLED, TEXT, 0.04f), pop), tl = alpha(blend(DISABLED, TEXT, 0.13f), pop);
    dl->AddRectFilledMultiColor(t0, t1, th, th, tl, tl);
    ImU32 fh = alpha(foc ? blend(ACCENT, WHITE, 0.12f) : blend(TEXT, PANEL, 0.2f), pop), fl = alpha(foc ? ACCENT : TEXT, pop);
    float fx = t0.x + (t1.x - t0.x) * frac;
    dl->AddRectFilledMultiColor(t0, ImVec2(fx, t1.y), fh, fh, fl, fl);
    dl->AddRectFilled(ImVec2(fx - 2 * unit, t0.y - 2.5f * unit), ImVec2(fx, t1.y + 2.5f * unit), alpha(IM_COL32(235, 235, 235, 255), pop));
    dl->PopClipRect();

    side_fill(dl, v.pt(x + 431, 96), v.pt(x + W, PAGE_B), pop);
    const char* words[3] = {"Microphone turns your microphone on or off. When it is off, other players can't hear you.",
                            "Voice Chat turns all voice on or off. When it is off, you hear no one and no one hears you.",
                            "Volume sets how loud other players sound."};
    const float ts = uu(17.333f), lh = 21.25f / 17.333f;
    float y = 96 + uu(12);
    y += wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), "Set options for voice chat.") + ts * lh;
    wrapped(dl, a, v, ts, lh, x + 431 + uu(18), y, uu(247), alpha(SIDE_TEXT, pop), words[std::clamp(voice_row_, 0, 2)]);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
}
