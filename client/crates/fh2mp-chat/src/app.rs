//! The chat window: friends with presence on the left, the open conversation in the middle, the launcher's colours.

use std::collections::{HashMap, HashSet};
use std::sync::mpsc as stdmpsc;
use std::time::{Duration, Instant};

use eframe::egui::{self, Color32, CornerRadius, Key, KeyboardShortcut, Modifiers, RichText, Stroke, TextureHandle, Vec2};
use fh2mp_mtproto::tl::{enums, types};
use tokio::sync::mpsc;

use crate::net::{Cmd, Status, Ui};

// The launcher's palette (launcher/fh2mp_launcher/ui/theme.py).
const BASE: Color32 = Color32::from_rgb(0x24, 0x27, 0x40);
const MANTLE: Color32 = Color32::from_rgb(0x1f, 0x21, 0x34);
const CRUST: Color32 = Color32::from_rgb(0x1a, 0x16, 0x30);
const SURFACE0: Color32 = Color32::from_rgb(0x36, 0x3a, 0x54);
const SURFACE1: Color32 = Color32::from_rgb(0x4a, 0x4f, 0x72);
const OVERLAY0: Color32 = Color32::from_rgb(0x7c, 0x82, 0x9f);
const TEXT: Color32 = Color32::from_rgb(0xd5, 0xd9, 0xf5);
const SUBTEXT: Color32 = Color32::from_rgb(0xb5, 0xba, 0xd8);
const MAUVE: Color32 = Color32::from_rgb(0xc9, 0xa0, 0xf7);
const BLUE: Color32 = Color32::from_rgb(0x8a, 0xad, 0xf4);
const GREEN: Color32 = Color32::from_rgb(0x8f, 0xe0, 0xa0);
const GOLD: Color32 = Color32::from_rgb(0xf2, 0xd4, 0x79);
const RED: Color32 = Color32::from_rgb(0xf2, 0x89, 0x9a);

pub fn style(ctx: &egui::Context) {
    ctx.set_theme(egui::Theme::Dark);
    let mut v = egui::Visuals::dark();
    v.panel_fill = BASE;
    v.window_fill = MANTLE;
    v.extreme_bg_color = CRUST;
    v.faint_bg_color = MANTLE;
    v.override_text_color = Some(TEXT);
    v.hyperlink_color = BLUE;
    v.selection.bg_fill = SURFACE1;
    v.selection.stroke = Stroke::new(1.0, MAUVE);
    v.error_fg_color = RED;
    v.warn_fg_color = GOLD;
    for w in [
        &mut v.widgets.inactive,
        &mut v.widgets.hovered,
        &mut v.widgets.active,
        &mut v.widgets.open,
    ] {
        w.corner_radius = CornerRadius::same(8);
    }
    v.widgets.inactive.weak_bg_fill = SURFACE0;
    v.widgets.inactive.bg_fill = SURFACE0;
    v.widgets.hovered.weak_bg_fill = SURFACE1;
    v.widgets.hovered.bg_fill = SURFACE1;
    v.widgets.active.weak_bg_fill = MAUVE;
    v.widgets.noninteractive.bg_stroke = Stroke::new(1.0, Color32::from_rgb(0x31, 0x34, 0x4e));
    ctx.set_visuals_of(egui::Theme::Dark, v);
}

#[derive(Clone, Copy, PartialEq)]
enum View {
    Club,
    Friend(i64),
    Requests,
}

struct Club {
    id: i64,
    tag: String,
    name: String,
    members: i32,
    messages: Vec<types::ClubMessage>,
    loaded: bool,
    read_max: i64,
    marked: i64,
}

/// What a blocked window says instead of the chat.
pub enum Blocked {
    NoAccount,
    NoServer(String),
}

pub struct App {
    cmd: mpsc::UnboundedSender<Cmd>,
    rx: stdmpsc::Receiver<Ui>,
    realm: &'static str,
    blocked: Option<Blocked>,
    status: Status,
    me: Option<types::Contact>,
    friends: Vec<types::Contact>,
    incoming: Vec<types::Contact>,
    outgoing: Vec<types::Contact>,
    club: Option<Club>,
    view: View,
    input: String,
    add: String,
    pics: HashMap<i64, (i64, TextureHandle)>,
    asked: HashSet<i64>,
    toast: Option<(String, bool, Instant)>,
    focused: bool,
}

fn user(c: &types::Contact) -> &types::User {
    let enums::User::User(u) = &c.user;
    u
}

fn club_of(c: &types::Contact) -> Option<&types::Club> {
    c.club.as_ref().map(|enums::Club::Club(cl)| cl)
}

fn presence_label(p: &enums::Presence) -> (String, Color32) {
    match p {
        enums::Presence::Playing(pl) => match pl.lobby_kind.as_str() {
            "freeroam" => ("In Free Roam".into(), MAUVE),
            "roadtrip" => ("On a Road Trip".into(), MAUVE),
            _ => ("Playing".into(), MAUVE),
        },
        enums::Presence::Online => ("Online".into(), GREEN),
        enums::Presence::Offline => ("Offline".into(), OVERLAY0),
    }
}

fn presence_rank(p: &enums::Presence) -> u8 {
    match p {
        enums::Presence::Playing(_) => 0,
        enums::Presence::Online => 1,
        enums::Presence::Offline => 2,
    }
}

fn clock(unix: i32) -> String {
    use chrono::TimeZone;
    match chrono::Local.timestamp_opt(unix as i64, 0).single() {
        Some(t) => {
            let today = chrono::Local::now().date_naive();
            if t.date_naive() == today {
                t.format("%H:%M").to_string()
            } else {
                t.format("%d %b %H:%M").to_string()
            }
        }
        None => String::new(),
    }
}

fn random_id() -> i64 {
    let mut b = [0u8; 8];
    let _ = getrandom::fill(&mut b);
    i64::from_le_bytes(b)
}

impl App {
    pub fn new(cmd: mpsc::UnboundedSender<Cmd>, rx: stdmpsc::Receiver<Ui>, realm: &'static str, blocked: Option<Blocked>) -> Self {
        Self {
            cmd,
            rx,
            realm,
            blocked,
            status: Status::Connecting,
            me: None,
            friends: vec![],
            incoming: vec![],
            outgoing: vec![],
            club: None,
            view: View::Club,
            input: String::new(),
            add: String::new(),
            pics: HashMap::new(),
            asked: HashSet::new(),
            toast: None,
            focused: true,
        }
    }

    fn send(&self, c: Cmd) {
        let _ = self.cmd.send(c);
    }

    fn toast(&mut self, text: impl Into<String>, error: bool) {
        self.toast = Some((text.into(), error, Instant::now()));
    }

    fn my_xuid(&self) -> i64 {
        match &self.status {
            Status::Online { xuid, .. } => *xuid,
            _ => self.me.as_ref().map(|m| user(m).id).unwrap_or(0),
        }
    }

    /// Applies what the session reported since the last frame.
    fn pump(&mut self, ctx: &egui::Context) {
        while let Ok(u) = self.rx.try_recv() {
            match u {
                Ui::Status(s) => {
                    if let Status::Online { .. } = s {
                        // Signed in (again): reload friends, and the club from the last message we have.
                        self.send(Cmd::RefreshFriends);
                        if let Some(c) = &self.club {
                            let after = if c.loaded { c.messages.last().map(|m| m.id).unwrap_or(0) } else { 0 };
                            self.send(Cmd::LoadClub { club: c.id, after });
                        }
                    }
                    self.status = s;
                }
                Ui::Friends(l) => self.friends_list(l),
                Ui::Presence { user: x, presence } => {
                    for f in self.friends.iter_mut().filter(|f| user(f).id == x) {
                        f.presence = presence.clone();
                    }
                    self.sort_friends();
                }
                Ui::ClubPage {
                    club,
                    messages,
                    read_max_id,
                    fresh,
                } => {
                    if let Some(c) = self.club.as_mut().filter(|c| c.id == club) {
                        if fresh {
                            c.messages = messages;
                        } else {
                            let last = c.messages.last().map(|m| m.id).unwrap_or(0);
                            c.messages.extend(messages.into_iter().filter(|m| m.id > last));
                        }
                        c.loaded = true;
                        c.read_max = c.read_max.max(read_max_id);
                    }
                }
                Ui::ClubMessage(m) => {
                    let mine = {
                        let enums::User::User(s) = &m.sender;
                        s.id
                    } == self.my_xuid();
                    if let Some(c) = self.club.as_mut().filter(|c| c.id == m.club_id)
                        && !c.messages.iter().any(|x| x.id == m.id)
                    {
                        c.messages.push(m);
                        c.messages.sort_by_key(|x| x.id);
                        if !mine && !self.focused {
                            ctx.send_viewport_cmd(egui::ViewportCommand::RequestUserAttention(egui::UserAttentionType::Informational));
                        }
                    }
                }
                Ui::SendFailed { text, why } => {
                    if self.input.is_empty() {
                        self.input = text;
                    }
                    self.toast(format!("Not sent: {why}"), true);
                }
                Ui::Gamerpic { user: x, version, png } => {
                    if let Ok(img) = image::load_from_memory_with_format(&png, image::ImageFormat::Png) {
                        let rgba = img.to_rgba8();
                        let size = [rgba.width() as usize, rgba.height() as usize];
                        let tex = ctx.load_texture(
                            format!("pic-{x}"),
                            egui::ColorImage::from_rgba_unmultiplied(size, rgba.as_raw()),
                            egui::TextureOptions::LINEAR,
                        );
                        self.pics.insert(x, (version, tex));
                    }
                }
                Ui::Notice(t) => self.toast(t, false),
                Ui::Error(t) => self.toast(t, true),
            }
        }
    }

    fn friends_list(&mut self, l: types::friends::List) {
        let enums::Contact::Contact(me) = l.me;
        let unwrap = |v: Vec<enums::Contact>| v.into_iter().map(|enums::Contact::Contact(c)| c).collect::<Vec<_>>();
        self.friends = unwrap(l.friends);
        self.incoming = unwrap(l.incoming);
        self.outgoing = unwrap(l.outgoing);
        match club_of(&me) {
            Some(cl) => {
                let same = self.club.as_ref().is_some_and(|c| c.id == cl.id);
                if !same {
                    self.club = Some(Club {
                        id: cl.id,
                        tag: cl.tag.clone(),
                        name: cl.name.clone(),
                        members: cl.members,
                        messages: vec![],
                        loaded: false,
                        read_max: 0,
                        marked: 0,
                    });
                    self.send(Cmd::LoadClub { club: cl.id, after: 0 });
                } else if let Some(c) = self.club.as_mut() {
                    c.members = cl.members;
                    c.name = cl.name.clone();
                }
            }
            None => self.club = None,
        }
        self.me = Some(me);
        self.sort_friends();
        if let View::Friend(x) = self.view
            && !self.friends.iter().any(|f| user(f).id == x)
        {
            self.view = View::Club;
        }
    }

    fn sort_friends(&mut self) {
        self.friends.sort_by(|a, b| {
            presence_rank(&a.presence)
                .cmp(&presence_rank(&b.presence))
                .then(user(a).gamertag.to_lowercase().cmp(&user(b).gamertag.to_lowercase()))
        });
    }

    fn pic(&mut self, ui: &mut egui::Ui, x: i64, version: i64, size: f32) {
        match self.pics.get(&x) {
            Some((v, tex)) => {
                ui.add(
                    egui::Image::new(tex)
                        .fit_to_exact_size(Vec2::splat(size))
                        .corner_radius(CornerRadius::same((size / 2.0) as u8)),
                );
                if *v != version && self.asked.insert(x) {
                    self.send(Cmd::Gamerpic(x));
                }
            }
            None => {
                let (rect, _) = ui.allocate_exact_size(Vec2::splat(size), egui::Sense::hover());
                ui.painter().circle_filled(rect.center(), size / 2.0, SURFACE0);
                if self.asked.insert(x) {
                    self.send(Cmd::Gamerpic(x));
                }
            }
        }
    }

    fn unread(&self) -> usize {
        let me = self.my_xuid();
        self.club.as_ref().map_or(0, |c| {
            c.messages
                .iter()
                .filter(|m| {
                    m.id > c.read_max && {
                        let enums::User::User(s) = &m.sender;
                        s.id != me
                    }
                })
                .count()
        })
    }

    fn side(&mut self, ui: &mut egui::Ui) {
        ui.add_space(10.0);
        if let Some(me) = self.me.clone() {
            let u = user(&me).clone();
            ui.horizontal(|ui| {
                self.pic(ui, u.id, me.gamerpic_version, 36.0);
                ui.vertical(|ui| {
                    ui.label(RichText::new(&u.gamertag).strong().size(15.0));
                    let (t, c) = presence_label(&me.presence);
                    ui.label(RichText::new(t).color(c).size(12.0));
                });
            });
            ui.add_space(8.0);
            ui.separator();
        }
        if let Some((tag, name)) = self.club.as_ref().map(|c| (c.tag.clone(), c.name.clone())) {
            let unread = self.unread();
            let label = if unread > 0 {
                format!("[{tag}] {name}   ({unread})")
            } else {
                format!("[{tag}] {name}")
            };
            if ui.selectable_label(self.view == View::Club, RichText::new(label).strong()).clicked() {
                self.view = View::Club;
            }
        }
        if !self.incoming.is_empty() || !self.outgoing.is_empty() {
            let n = self.incoming.len();
            let label = if n > 0 {
                format!("Friend requests ({n})")
            } else {
                "Friend requests".into()
            };
            if ui
                .selectable_label(
                    self.view == View::Requests,
                    RichText::new(label).color(if n > 0 { GOLD } else { SUBTEXT }),
                )
                .clicked()
            {
                self.view = View::Requests;
            }
        }
        ui.add_space(6.0);
        ui.label(RichText::new(format!("FRIENDS  {}", self.friends.len())).size(11.0).color(OVERLAY0));
        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .max_height(ui.available_height() - 70.0)
            .show(ui, |ui| {
                for f in self.friends.clone() {
                    let u = user(&f).clone();
                    let selected = self.view == View::Friend(u.id);
                    let resp = ui
                        .scope(|ui| {
                            ui.horizontal(|ui| {
                                self.pic(ui, u.id, f.gamerpic_version, 28.0);
                                ui.vertical(|ui| {
                                    ui.label(RichText::new(&u.gamertag).color(if selected { MAUVE } else { TEXT }));
                                    let (t, c) = presence_label(&f.presence);
                                    ui.label(RichText::new(t).color(c).size(11.0));
                                });
                            })
                        })
                        .response
                        .interact(egui::Sense::click());
                    if resp.clicked() {
                        self.view = View::Friend(u.id);
                    }
                    ui.add_space(4.0);
                }
                if self.friends.is_empty() {
                    ui.label(RichText::new("No friends yet. Add one by gamertag below.").color(OVERLAY0).size(12.0));
                }
            });
        ui.separator();
        ui.horizontal(|ui| {
            let r = ui.add(
                egui::TextEdit::singleline(&mut self.add)
                    .hint_text("Add friend: gamertag or ID")
                    .desired_width(ui.available_width() - 44.0),
            );
            let go = ui.button("Add").clicked() || (r.lost_focus() && ui.input(|i| i.key_pressed(Key::Enter)));
            if go && !self.add.trim().is_empty() {
                self.send(Cmd::AddFriend(self.add.trim().to_string()));
                self.add.clear();
            }
        });
    }

    fn club_view(&mut self, ui: &mut egui::Ui) {
        let Some(c) = self.club.as_ref() else {
            ui.centered_and_justified(|ui| {
                ui.label(RichText::new("You are not in a club. Join one in the game, and its chat shows up here.").color(OVERLAY0))
            });
            return;
        };
        let (id, tag, name, members) = (c.id, c.tag.clone(), c.name.clone(), c.members);
        ui.horizontal(|ui| {
            ui.label(RichText::new(format!("[{tag}] {name}")).strong().size(17.0));
            ui.label(RichText::new(format!("{members} members")).color(OVERLAY0));
        });
        ui.separator();
        let me = self.my_xuid();
        let input_h = 64.0;
        let msgs = self.club.as_ref().map(|c| c.messages.clone()).unwrap_or_default();
        egui::ScrollArea::vertical()
            .stick_to_bottom(true)
            .auto_shrink([false, false])
            .max_height(ui.available_height() - input_h)
            .show(ui, |ui| {
                if msgs.is_empty() {
                    ui.label(RichText::new("No messages in the last 7 days. Say hi!").color(OVERLAY0));
                }
                let mut prev: Option<(i64, i32)> = None;
                for m in &msgs {
                    let enums::User::User(s) = &m.sender;
                    let grouped = prev.is_some_and(|(p, t)| p == s.id && m.date - t < 300);
                    if !grouped {
                        ui.add_space(6.0);
                        ui.horizontal(|ui| {
                            self.pic(ui, s.id, 0, 22.0);
                            ui.label(RichText::new(&s.gamertag).strong().color(if s.id == me { MAUVE } else { BLUE }));
                            ui.label(RichText::new(clock(m.date)).size(11.0).color(OVERLAY0));
                        });
                    }
                    ui.horizontal_wrapped(|ui| {
                        ui.add_space(28.0);
                        ui.label(&m.text);
                    });
                    prev = Some((s.id, m.date));
                }
            });
        // Read up to the newest message while the club is on screen and the window has focus.
        if self.focused
            && let Some(c) = self.club.as_mut()
            && let Some(last) = c.messages.last().map(|m| m.id)
            && last > c.read_max.max(c.marked)
        {
            c.marked = last;
            c.read_max = last;
            let _ = self.cmd.send(Cmd::ReadClub { club: c.id, max_id: last });
        }
        ui.separator();
        let online = matches!(self.status, Status::Online { .. });
        ui.horizontal(|ui| {
            let edit = egui::TextEdit::multiline(&mut self.input)
                .hint_text(if online {
                    "Message the club (Enter sends, Shift+Enter: new line)"
                } else {
                    "Not connected"
                })
                .desired_rows(2)
                .desired_width(ui.available_width() - 64.0)
                .return_key(KeyboardShortcut::new(Modifiers::SHIFT, Key::Enter));
            let r = ui.add_enabled(online, edit);
            let enter = r.has_focus() && ui.input(|i| i.key_pressed(Key::Enter) && !i.modifiers.shift);
            if (ui.add_enabled(online, egui::Button::new("Send")).clicked() || enter) && !self.input.trim().is_empty() {
                let text = std::mem::take(&mut self.input).trim().to_string();
                self.send(Cmd::SendClub {
                    club: id,
                    random_id: random_id(),
                    text,
                });
                r.request_focus();
            }
        });
    }

    fn friend_view(&mut self, ui: &mut egui::Ui, x: i64) {
        let Some(f) = self.friends.iter().find(|f| user(f).id == x).cloned() else {
            return;
        };
        let u = user(&f).clone();
        ui.add_space(20.0);
        ui.vertical_centered(|ui| {
            self.pic(ui, u.id, f.gamerpic_version, 96.0);
            ui.add_space(8.0);
            ui.label(RichText::new(&u.gamertag).strong().size(20.0));
            let (t, c) = presence_label(&f.presence);
            ui.label(RichText::new(t).color(c));
            let club = club_of(&f).map(|cl| format!("  ·  [{}] {}", cl.tag, cl.name)).unwrap_or_default();
            ui.label(RichText::new(format!("Level {}{club}", f.level)).color(SUBTEXT));
            ui.add_space(24.0);
            ui.label(RichText::new("Direct messages arrive with end-to-end encryption in the next part of the chat.").color(OVERLAY0));
            ui.add_space(24.0);
            if ui.button(RichText::new("Remove friend").color(RED)).clicked() {
                self.send(Cmd::RemoveFriend(u.id));
            }
        });
    }

    fn requests_view(&mut self, ui: &mut egui::Ui) {
        ui.label(RichText::new("Friend requests").strong().size(17.0));
        ui.separator();
        for f in self.incoming.clone() {
            let u = user(&f).clone();
            ui.horizontal(|ui| {
                self.pic(ui, u.id, f.gamerpic_version, 28.0);
                ui.label(&u.gamertag);
                if ui.button("Accept").clicked() {
                    self.send(Cmd::Respond { user: u.id, accept: true });
                }
                if ui.button("Decline").clicked() {
                    self.send(Cmd::Respond { user: u.id, accept: false });
                }
            });
        }
        if !self.outgoing.is_empty() {
            ui.add_space(12.0);
            ui.label(RichText::new("SENT").size(11.0).color(OVERLAY0));
            for f in self.outgoing.clone() {
                let u = user(&f).clone();
                ui.horizontal(|ui| {
                    self.pic(ui, u.id, f.gamerpic_version, 28.0);
                    ui.label(&u.gamertag);
                    if ui.button("Cancel").clicked() {
                        self.send(Cmd::Respond { user: u.id, accept: false });
                    }
                });
            }
        }
    }

    fn status_line(&self) -> (String, Color32) {
        match &self.status {
            Status::Connecting => ("Connecting…".into(), SUBTEXT),
            Status::Online { .. } => ("Connected".into(), GREEN),
            Status::Reconnecting { why, retry_in } => (format!("Reconnecting in {} s ({why})", retry_in.as_secs().max(1)), GOLD),
            Status::SignedOut { reason } => (format!("Signed out: {reason}"), RED),
            Status::Refused { why } => (format!("Not signed in: {why}"), RED),
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.focused = ctx.input(|i| i.viewport().focused.unwrap_or(true));
        self.pump(&ctx);
        egui::Panel::top("bar")
            .frame(egui::Frame::new().fill(CRUST).inner_margin(8))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.label(RichText::new("FH2MP Chat").strong().color(MAUVE).size(16.0));
                    if self.realm != "prod" {
                        ui.label(RichText::new(self.realm.to_uppercase()).color(CRUST).background_color(GOLD).size(11.0));
                    }
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let (t, c) = self.status_line();
                        ui.label(RichText::new(t).color(c).size(12.0));
                        if matches!(self.status, Status::SignedOut { .. } | Status::Refused { .. }) && ui.small_button("Sign in here").clicked() {
                            self.send(Cmd::Reconnect);
                        }
                    });
                });
            });
        if let Some(b) = &self.blocked {
            let text = match b {
                Blocked::NoAccount => {
                    "There is no FH2MP account on this PC yet. Sign up in the launcher first, then open the chat again.".to_string()
                }
                Blocked::NoServer(why) => format!("The chat cannot start: {why}."),
            };
            egui::CentralPanel::default().show(ui, |ui| {
                ui.centered_and_justified(|ui| ui.label(RichText::new(text).size(15.0).color(SUBTEXT)));
            });
            return;
        }
        egui::Panel::left("friends")
            .default_size(250.0)
            .resizable(false)
            .frame(egui::Frame::new().fill(MANTLE).inner_margin(10))
            .show(ui, |ui| self.side(ui));
        egui::CentralPanel::default()
            .frame(egui::Frame::new().fill(BASE).inner_margin(12))
            .show(ui, |ui| {
                match self.view {
                    View::Club => self.club_view(ui),
                    View::Friend(x) => self.friend_view(ui, x),
                    View::Requests => self.requests_view(ui),
                }
                if let Some((t, err, at)) = &self.toast {
                    if at.elapsed() < Duration::from_secs(5) {
                        egui::Area::new(egui::Id::unique("toast"))
                            .anchor(egui::Align2::CENTER_BOTTOM, [0.0, -80.0])
                            .show(ui.ctx(), |ui| {
                                egui::Frame::new().fill(SURFACE0).corner_radius(8).inner_margin(10).show(ui, |ui| {
                                    ui.label(RichText::new(t).color(if *err { RED } else { TEXT }));
                                });
                            });
                        ui.ctx().request_repaint_after(Duration::from_millis(500));
                    } else {
                        self.toast = None;
                    }
                }
            });
        if matches!(self.status, Status::Reconnecting { .. }) {
            ctx.request_repaint_after(Duration::from_secs(1));
        }
    }
}
