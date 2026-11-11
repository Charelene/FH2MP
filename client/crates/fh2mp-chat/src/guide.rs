//! The Guide, v0 (plan phase 3): the Xbox 360 style overlay from the design canvas (https://claude.ai/artifact/GYsZgmPY8hSP7A4iGQaAyH,
//! artboard "1 · Guide"), drawn by this core in its own borderless, transparent, always-on-top window over the game. Three blades (Games,
//! the player, Settings), rows with a sliding focus bar, the hint bar and a 360 style toast. The canvas is a 1280x720 design; everything
//! here is in those units and scaled to the window. Rows that do nothing yet show a toast. Pad and keys come from pad.rs.

use std::time::Instant;

use eframe::egui::{self, Align2, Color32, CornerRadius, FontFamily, FontId, Pos2, Rect, Stroke, Vec2};

use crate::pad::PadEvent;

// Theme "xbox" of the canvas.
const ACCENT: Color32 = Color32::from_rgb(0x10, 0x8a, 0x10);
const DARK: Color32 = Color32::from_rgb(0x0a, 0x4f, 0x0a);
const PANEL: Color32 = Color32::from_rgb(0x2a, 0x2b, 0x2e);
const LINE: Color32 = Color32::from_rgb(0x3a, 0x3b, 0x3f);
const TAB: Color32 = Color32::from_rgb(0x45, 0x47, 0x4c);
const RAISED: Color32 = Color32::from_rgb(0x34, 0x36, 0x3a);
const MUTED: Color32 = Color32::from_rgb(0xb4, 0xb6, 0xbb);
const TAB_TEXT: Color32 = Color32::from_rgb(0xd4, 0xd6, 0xda);
const MUTED3: Color32 = Color32::from_rgb(0xc9, 0xcc, 0xd2);

/// What a row does when it is chosen.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Action {
    /// Open the chat window (friends, messages for now).
    OpenChat,
    /// Nothing yet: a toast says so.
    Soon,
    /// Close the Guide (back to the game).
    Resume,
}

struct Row {
    label: String,
    meta: String,
    action: Action,
}

/// An eased value: moves from `from` to `to` over `secs`, ease-out (the canvas' cubic-bezier(.1,.9,.2,1) is close to a quartic ease-out).
#[derive(Clone, Copy)]
struct Tween {
    from: f32,
    to: f32,
    start: f64,
    secs: f32,
}

impl Tween {
    fn at(v: f32) -> Self {
        Tween {
            from: v,
            to: v,
            start: 0.0,
            secs: 0.0,
        }
    }
    fn go(&mut self, to: f32, now: f64, secs: f32) {
        if (to - self.to).abs() > f32::EPSILON {
            *self = Tween {
                from: self.value(now),
                to,
                start: now,
                secs,
            };
        }
    }
    fn value(&self, now: f64) -> f32 {
        if self.secs <= 0.0 {
            return self.to;
        }
        let t = (((now - self.start) as f32) / self.secs).clamp(0.0, 1.0);
        let e = 1.0 - (1.0 - t).powi(4);
        self.from + (self.to - self.from) * e
    }
    fn moving(&self, now: f64) -> bool {
        self.secs > 0.0 && ((now - self.start) as f32) < self.secs
    }
}

pub struct Guide {
    pub open: bool,
    blade: usize,
    focus: [usize; 3],
    opened: f64,
    switched: f64,
    switch_dir: f32,
    bar: [Tween; 3],
    widths: [Tween; 3],
    toast: Option<(String, String, f64)>,
    clock_base: Instant,
}

const BLADES: [&str; 3] = ["Games", "", "Settings"];

impl Default for Guide {
    fn default() -> Self {
        Guide {
            open: false,
            blade: 1,
            focus: [0, 0, 0],
            opened: 0.0,
            switched: -10.0,
            switch_dir: 1.0,
            bar: [Tween::at(0.0); 3],
            widths: [Tween::at(64.0), Tween::at(832.0), Tween::at(64.0)],
            toast: None,
            clock_base: Instant::now(),
        }
    }
}

/// The player's numbers the rows show.
pub struct Facts<'a> {
    pub gamertag: &'a str,
    pub friends_online: usize,
    pub requests: usize,
}

fn rows(blade: usize, f: &Facts) -> Vec<Row> {
    let r = |label: &str, meta: String, action| Row {
        label: label.into(),
        meta,
        action,
    };
    match blade {
        0 => vec![
            r("Forza Horizon 2", "Now playing".into(), Action::Resume),
            r("Achievements", String::new(), Action::Soon),
        ],
        1 => vec![
            r(
                "Friends",
                if f.friends_online > 0 {
                    format!("{} online", f.friends_online)
                } else {
                    String::new()
                },
                Action::OpenChat,
            ),
            r("Party", String::new(), Action::Soon),
            r(
                "Messages",
                if f.requests > 0 { format!("{} new", f.requests) } else { String::new() },
                Action::OpenChat,
            ),
            r("Beacons & Activity", String::new(), Action::Soon),
            r("Private Chat", String::new(), Action::Soon),
            r("Invite to Game", String::new(), Action::Soon),
        ],
        _ => vec![
            r("Profile", String::new(), Action::Soon),
            r("Theme", "Xbox".into(), Action::Soon),
            r("Voice", String::new(), Action::Soon),
            r("Notifications", String::new(), Action::Soon),
            r("Controller", String::new(), Action::Soon),
            r("System", String::new(), Action::Soon),
        ],
    }
}

/// Adds Segoe UI (the 360 / Xbox One look) as the Guide's font family when Windows has it; else the Guide uses egui's own font.
pub fn fonts(ctx: &egui::Context) -> bool {
    let path = std::env::var("WINDIR").map(|w| format!("{w}\\Fonts\\segoeui.ttf")).unwrap_or_default();
    let Ok(bytes) = std::fs::read(&path) else {
        return false;
    };
    ctx.add_font(egui::epaint::text::FontInsert::new(
        "segoeui",
        egui::FontData::from_owned(bytes),
        vec![egui::epaint::text::InsertFontFamily {
            family: FontFamily::Name("guide".into()),
            priority: egui::epaint::text::FontPriority::Highest,
        }],
    ));
    true
}

impl Guide {
    pub fn toggle(&mut self, now: f64) {
        self.open = !self.open;
        if self.open {
            self.opened = now;
            self.toast = None;
        }
    }

    fn switch(&mut self, to: usize, now: f64) {
        if to == self.blade || to >= 3 {
            return;
        }
        self.switch_dir = if to > self.blade { 1.0 } else { -1.0 };
        self.blade = to;
        self.switched = now;
        for (i, w) in self.widths.iter_mut().enumerate() {
            w.go(if i == to { 832.0 } else { 64.0 }, now, 0.3);
        }
    }

    fn say(&mut self, title: &str, detail: &str, now: f64) {
        self.toast = Some((title.into(), detail.into(), now));
    }

    /// One pad or key event while open -> what the app must do (open the chat window), if anything.
    pub fn handle(&mut self, e: PadEvent, f: &Facts, now: f64) -> Option<Action> {
        let n = rows(self.blade, f).len();
        let b = self.blade;
        match e {
            PadEvent::Toggle => self.toggle(now),
            PadEvent::Up | PadEvent::Down => {
                let cur = self.focus[b];
                let next = if e == PadEvent::Up {
                    cur.saturating_sub(1)
                } else {
                    (cur + 1).min(n.saturating_sub(1))
                };
                self.focus[b] = next;
                self.bar[b].go(next as f32, now, 0.14);
            }
            PadEvent::Left | PadEvent::Lb => self.switch(b.saturating_sub(1), now),
            PadEvent::Right | PadEvent::Rb => self.switch((b + 1).min(2), now),
            PadEvent::B => self.open = false,
            PadEvent::A => {
                let row = rows(b, f).into_iter().nth(self.focus[b])?;
                match row.action {
                    Action::Resume => self.open = false,
                    Action::OpenChat => {
                        self.open = false;
                        return Some(Action::OpenChat);
                    }
                    Action::Soon => self.say(&row.label, "Coming in a later build", now),
                }
            }
            _ => {}
        }
        None
    }

    /// Draws the open Guide into `ui` (its own window over the game). -> an action chosen with the mouse.
    #[allow(clippy::needless_range_loop)] // the blade index also picks the widths, focus and bar of the same blade
    pub fn draw(&mut self, ui: &mut egui::Ui, f: &Facts, now: f64) -> Option<Action> {
        let full = ui.max_rect();
        let s = (full.height() / 720.0).min(full.width() / 1280.0).max(0.3);
        let origin = full.center() - Vec2::new(640.0, 360.0) * s;
        let at = |x: f32, y: f32| origin + Vec2::new(x, y) * s;
        let family = if ui.ctx().fonts(|fo| fo.families().contains(&FontFamily::Name("guide".into()))) {
            FontFamily::Name("guide".into())
        } else {
            FontFamily::Proportional
        };
        let font = |px: f32| FontId::new(px * s, family.clone());
        let p = ui.painter().clone();
        let mut chosen = None;

        // open: dim the game, the Guide grows in from 92 % (0.26 s)
        let k = (((now - self.opened) as f32) / 0.26).clamp(0.0, 1.0);
        let grow = 1.0 - (1.0 - k).powi(4);
        p.rect_filled(full, 0.0, Color32::from_black_alpha((150.0 * grow) as u8));
        let zoom = 0.92 + 0.08 * grow;
        let alpha = grow;
        let center = at(640.0, 360.0);
        let z = |q: Pos2| center + (q - center) * zoom;
        let fade = |c: Color32| c.gamma_multiply(alpha);

        // header: blade name, the player, the clock
        let title = if self.blade == 1 {
            f.gamertag.to_string()
        } else {
            BLADES[self.blade].to_string()
        };
        p.text(z(at(187.0, 61.0)), Align2::LEFT_TOP, &title, font(22.0), fade(Color32::WHITE));
        let clock = chrono::Local::now().format("%-I:%M %p").to_string();
        let _ = self.clock_base;
        p.text(z(at(1090.0, 61.0)), Align2::RIGHT_TOP, clock, font(22.0), fade(Color32::WHITE));

        // blades: the chosen one wide (96 label + 736 rows), the others 64; 500 tall chosen, 452 the others, centred on y = 346
        let mut x = 160.0;
        let pointer = ui.input(|i| i.pointer.hover_pos());
        let clicked = ui.input(|i| i.pointer.primary_clicked());
        for bi in 0..3 {
            let w = self.widths[bi].value(now);
            let on = bi == self.blade;
            let h = if on { 500.0 } else { 452.0 };
            let top = 96.0 + (500.0 - h) / 2.0;
            let label_w: f32 = if on { 96.0 } else { 64.0 };
            let lr = Rect::from_min_max(z(at(x, top)), z(at(x + label_w.min(w), top + h)));
            p.rect_filled(lr, 0.0, fade(if on { RAISED } else { TAB }));
            let name = if bi == 1 { f.gamertag.to_string() } else { BLADES[bi].to_string() };
            let g = p.layout_no_wrap(name, font(26.0), fade(if on { Color32::WHITE } else { TAB_TEXT }));
            let tx = lr.center().x + g.size().y / 2.0;
            p.add(egui::epaint::TextShape::new(Pos2::new(tx, lr.top() + 24.0 * s), g, Color32::WHITE).with_angle(std::f32::consts::FRAC_PI_2));
            if !on && clicked && pointer.is_some_and(|q| lr.contains(q)) {
                self.switch(bi, now);
            }
            if w > label_w + 1.0 {
                let body = Rect::from_min_max(z(at(x + label_w, top)), z(at(x + w, top + h)));
                p.rect_filled(body, 0.0, fade(PANEL));
                let clip = p.with_clip_rect(body);
                let list = rows(bi, f);
                let since = (now - self.switched) as f32;
                // rows slide in from the side the player moved to (150 units, 0.32 s), one after another (28 ms apart)
                for (i, row) in list.iter().enumerate() {
                    let d = (since - 0.04 - i as f32 * 0.028).clamp(0.0, 0.32) / 0.32;
                    let slide = if on && since < 0.6 {
                        (1.0 - (1.0 - d).powi(4) - 1.0) * -150.0 * self.switch_dir
                    } else {
                        0.0
                    };
                    let row_alpha = if on && since < 0.6 { d.min(1.0) } else { 1.0 };
                    let y = top + i as f32 * 66.0;
                    let rr = Rect::from_min_max(z(at(x + label_w + slide, y)), z(at(x + w + slide, y + 66.0)));
                    if on && pointer.is_some_and(|q| rr.contains(q)) && self.focus[bi] != i {
                        self.focus[bi] = i;
                        self.bar[bi].go(i as f32, now, 0.14);
                    }
                    if on
                        && clicked
                        && pointer.is_some_and(|q| rr.contains(q))
                        && let Some(a) = self.handle(PadEvent::A, f, now)
                    {
                        chosen = Some(a);
                    }
                    if on && i == self.focus[bi] {
                        let bar_y = top + self.bar[bi].value(now) * 66.0;
                        let br = Rect::from_min_max(z(at(x + label_w, bar_y)), z(at(x + w, bar_y + 66.0)));
                        clip.rect_filled(br, 0.0, fade(ACCENT));
                        clip.line_segment([br.left_bottom(), br.right_bottom()], Stroke::new(1.0 * s, fade(DARK)));
                    } else {
                        clip.line_segment([rr.left_bottom(), rr.right_bottom()], Stroke::new(1.0 * s, fade(LINE)));
                    }
                    let ink = |c: Color32| fade(c).gamma_multiply(row_alpha);
                    clip.text(
                        rr.left_center() + Vec2::new(30.0 * s, 0.0),
                        Align2::LEFT_CENTER,
                        &row.label,
                        font(26.0),
                        ink(Color32::WHITE),
                    );
                    if !row.meta.is_empty() {
                        let meta_c = if on && i == self.focus[bi] { Color32::WHITE } else { MUTED };
                        clip.text(
                            rr.right_center() - Vec2::new(30.0 * s, 0.0),
                            Align2::RIGHT_CENTER,
                            &row.meta,
                            font(20.0),
                            ink(meta_c),
                        );
                    }
                }
            }
            x += w;
        }

        // hint bar
        let mut hx = 187.0;
        for (key, label, ring) in [
            ("A", "Select", Color32::from_rgb(0x5f, 0xd1, 0x1c)),
            ("B", "Back", Color32::from_rgb(0xff, 0x4a, 0x4f)),
            ("LB", "", MUTED3),
            ("RB", "Switch", MUTED3),
        ] {
            let wide = key.len() > 1;
            let kr = Rect::from_min_size(
                z(at(hx, 606.0)),
                Vec2::new(if wide { 34.0 } else { 20.0 }, if wide { 22.0 } else { 20.0 }) * s * zoom,
            );
            p.rect_filled(kr, CornerRadius::same(if wide { (6.0 * s) as u8 } else { 255 }), fade(Color32::BLACK));
            p.rect_stroke(
                kr,
                CornerRadius::same(if wide { (6.0 * s) as u8 } else { 255 }),
                Stroke::new(2.0 * s, fade(ring)),
                egui::StrokeKind::Inside,
            );
            p.text(kr.center(), Align2::CENTER_CENTER, key, font(if wide { 11.0 } else { 12.0 }), fade(ring));
            hx += if wide { 34.0 } else { 20.0 } + 7.0;
            if !label.is_empty() {
                let g = p.layout_no_wrap(label.to_string(), font(19.0), fade(Color32::WHITE));
                let lw = g.size().x / s / zoom;
                p.galley(z(at(hx, 605.0)), g, Color32::WHITE);
                hx += lw + 22.0;
            } else {
                hx += 0.0;
            }
        }

        self.draw_toast(&p, s, origin, now, &font);
        let busy = self.widths.iter().any(|w| w.moving(now))
            || self.bar.iter().any(|b| b.moving(now))
            || now - self.opened < 0.3
            || now - self.switched < 0.7;
        if busy || self.toast.is_some() {
            ui.ctx().request_repaint();
        }
        chosen
    }

    /// The 360 toast at the bottom centre: the icon square zooms in, slides left while the text unfolds, stays, folds back (6.3 s).
    fn draw_toast(&mut self, p: &egui::Painter, s: f32, origin: Pos2, now: f64, font: &dyn Fn(f32) -> FontId) {
        let Some((title, detail, at)) = self.toast.clone() else { return };
        let t = (now - at) as f32;
        if t > 6.3 {
            self.toast = None;
            return;
        }
        let ease = |a: f32, b: f32| {
            let k = ((t - a) / (b - a)).clamp(0.0, 1.0);
            k * k * (3.0 - 2.0 * k)
        };
        let zoom = (ease(0.0, 0.3) - ease(5.85, 6.3)).clamp(0.0, 1.0);
        let open = (ease(0.6, 1.05) - ease(5.4, 5.85)).clamp(0.0, 1.0);
        let base = origin + Vec2::new(474.0, 600.0) * s;
        let panel_w = 269.0 * s * open;
        let shift = 134.5 * s * (1.0 - open);
        if panel_w > 1.0 {
            let pr = Rect::from_min_size(base + Vec2::new(64.0 * s + shift, 0.0), Vec2::new(panel_w, 64.0 * s));
            p.rect_filled(pr, 0.0, PANEL);
            let clip = p.with_clip_rect(pr);
            clip.text(
                pr.left_top() + Vec2::new(22.0 * s, 10.0 * s),
                Align2::LEFT_TOP,
                title.to_uppercase(),
                font(17.0),
                ACCENT,
            );
            clip.text(
                pr.left_top() + Vec2::new(22.0 * s, 33.0 * s),
                Align2::LEFT_TOP,
                detail.to_uppercase(),
                font(17.0),
                Color32::WHITE,
            );
        }
        let side = 64.0 * s * zoom;
        let sq = Rect::from_center_size(base + Vec2::new(32.0 * s + shift, 32.0 * s), Vec2::splat(side));
        p.rect_filled(sq, 0.0, ACCENT);
        p.text(sq.center(), Align2::CENTER_CENTER, "i", font(34.0 * zoom.max(0.01)), Color32::WHITE);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts() -> Facts<'static> {
        Facts {
            gamertag: "Charelene",
            friends_online: 3,
            requests: 0,
        }
    }

    #[test]
    fn navigation() {
        let mut g = Guide::default();
        g.toggle(0.0);
        assert!(g.open);
        assert_eq!(g.blade, 1);
        g.handle(PadEvent::Down, &facts(), 0.1);
        g.handle(PadEvent::Down, &facts(), 0.2);
        assert_eq!(g.focus[1], 2);
        for _ in 0..20 {
            g.handle(PadEvent::Down, &facts(), 0.3);
        }
        assert_eq!(g.focus[1], 5); // stops at the last row
        g.handle(PadEvent::Rb, &facts(), 0.4);
        assert_eq!(g.blade, 2);
        g.handle(PadEvent::Rb, &facts(), 0.5);
        assert_eq!(g.blade, 2); // no blade past Settings
        g.handle(PadEvent::Lb, &facts(), 0.6);
        g.handle(PadEvent::Lb, &facts(), 0.7);
        assert_eq!(g.blade, 0);
        assert_eq!(g.handle(PadEvent::A, &facts(), 0.8), None); // "Forza Horizon 2": back to the game
        assert!(!g.open);
    }

    #[test]
    fn friends_opens_the_chat_and_soon_rows_toast() {
        let mut g = Guide::default();
        g.toggle(0.0);
        assert_eq!(g.handle(PadEvent::A, &facts(), 0.1), Some(Action::OpenChat));
        assert!(!g.open);
        g.toggle(1.0);
        g.handle(PadEvent::Down, &facts(), 1.1);
        assert_eq!(g.handle(PadEvent::A, &facts(), 1.2), None); // Party: not yet
        assert!(g.open && g.toast.as_ref().is_some_and(|t| t.0 == "Party"));
        g.handle(PadEvent::B, &facts(), 1.3);
        assert!(!g.open);
    }

    #[test]
    fn tween_eases_to_its_target() {
        let mut t = Tween::at(0.0);
        t.go(10.0, 1.0, 0.5);
        assert!(t.value(1.0) < 0.01);
        assert!(t.value(1.25) > 5.0 && t.value(1.25) < 10.0);
        assert!((t.value(2.0) - 10.0).abs() < 1e-4);
        assert!(!t.moving(2.0));
    }
}
