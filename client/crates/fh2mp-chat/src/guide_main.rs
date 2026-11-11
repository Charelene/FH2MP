//! fh2mp-guide, the in-game Guide on its own (plan phase 3): nothing on screen, no taskbar button and no console until the Guide button is
//! held for a second (or Home is pressed in the game). Then its one window, borderless, transparent and on top, covers the game's client
//! area and shows the Guide (guide.rs); B or another press of the Guide button hides it again. Friends and Messages ask a running chat core
//! (fh2mp-chat) to show itself; nothing else of the chat appears. One Guide per launcher config dir (instance.rs, "guide"); `--dev` uses the
//! dev launcher's dir, `--quit` closes a running Guide, `--preview` shows it at once in a normal window. Log: `<config>/logs/guide.log`.

#![windows_subsystem = "windows"]

#[path = "guide.rs"]
mod guide;
#[allow(dead_code)] // only config_dir is used here; the rest is the chat core's
#[path = "identity.rs"]
mod identity;
#[allow(dead_code)]
#[path = "instance.rs"]
mod instance;
#[path = "pad.rs"]
mod pad;

use std::fs::OpenOptions;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, channel};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use eframe::egui::{self, Key};

use crate::pad::PadEvent;

/// Where the hidden window waits: one pixel big at the primary screen's corner, because eframe shows a window after its first frame
/// whatever the builder says (the next frames hide it). Not off every screen: a window on no monitor kept a wrong scale factor and the
/// Guide filled two thirds of a 4K screen at 125 %.
const PARKED: egui::Pos2 = egui::pos2(0.0, 0.0);

struct GuideApp {
    guide: guide::Guide,
    pad: Receiver<PadEvent>,
    shown: Arc<AtomicBool>,
    quit: Receiver<()>,
    t0: Instant,
    config: PathBuf,
    gamertag: String,
    preview: bool,
    pending_open: bool,
    hide_frames: u8,
    quitting: bool,
}

/// The player's gamertag from the launcher's settings, for the Guide's header; "FH2MP" without one.
fn gamertag(config: &std::path::Path) -> String {
    std::fs::read(config.join("settings.json"))
        .ok()
        .and_then(|raw| serde_json::from_slice::<serde_json::Value>(&raw).ok())
        .and_then(|v| v.get("gamertag").and_then(|g| g.as_str()).map(str::to_string))
        .filter(|g| !g.is_empty())
        .unwrap_or_else(|| "FH2MP".into())
}

impl GuideApp {
    fn now(&self) -> f64 {
        self.t0.elapsed().as_secs_f64()
    }

    fn open(&mut self, ctx: &egui::Context) {
        let now = self.now();
        self.guide.toggle(now);
        let root = egui::ViewportId::ROOT;
        if self.preview {
            ctx.send_viewport_cmd_to(root, egui::ViewportCommand::OuterPosition(egui::pos2(80.0, 80.0)));
            ctx.send_viewport_cmd_to(root, egui::ViewportCommand::InnerSize(egui::vec2(1280.0, 720.0)));
        } else {
            let ppp = ctx.pixels_per_point();
            let r = pad::game_window()
                .map(|(x, y, w, h)| egui::Rect::from_min_size(egui::pos2(x as f32 / ppp, y as f32 / ppp), egui::vec2(w as f32 / ppp, h as f32 / ppp)))
                .unwrap_or_else(|| {
                    let size = ctx.input(|i| i.viewport().monitor_size).unwrap_or(egui::vec2(1280.0, 720.0));
                    egui::Rect::from_min_size(egui::Pos2::ZERO, size)
                });
            tracing::info!(
                x = r.min.x,
                y = r.min.y,
                w = r.width(),
                h = r.height(),
                "guide: opened over the game window"
            );
            ctx.send_viewport_cmd_to(root, egui::ViewportCommand::OuterPosition(r.min));
            ctx.send_viewport_cmd_to(root, egui::ViewportCommand::InnerSize(r.size()));
        }
        ctx.send_viewport_cmd_to(root, egui::ViewportCommand::Visible(true));
        ctx.send_viewport_cmd_to(root, egui::ViewportCommand::Focus);
        ctx.request_repaint();
    }

    fn close(&mut self, ctx: &egui::Context) {
        tracing::info!("guide: closed");
        let root = egui::ViewportId::ROOT;
        ctx.send_viewport_cmd_to(root, egui::ViewportCommand::Visible(false));
        ctx.send_viewport_cmd_to(root, egui::ViewportCommand::OuterPosition(PARKED));
        ctx.send_viewport_cmd_to(root, egui::ViewportCommand::InnerSize(egui::vec2(1.0, 1.0)));
    }

    fn act(&mut self, a: guide::Action) {
        if a == guide::Action::OpenChat {
            let config = self.config.clone();
            std::thread::spawn(move || match instance::send(&config, &instance::Request::Open(Some("club".into()))) {
                Ok(()) => tracing::info!("guide: asked the chat core to show itself"),
                Err(e) => tracing::info!(error = %e, "guide: no chat core to show"),
            });
        }
    }

    fn event(&mut self, ctx: &egui::Context, e: PadEvent) {
        let was = self.guide.open;
        if !was {
            if e == PadEvent::Toggle {
                self.open(ctx);
            }
            return;
        }
        let facts = guide::Facts {
            gamertag: &self.gamertag.clone(),
            friends_online: 0,
            requests: 0,
        };
        let action = self.guide.handle(e, &facts, self.now());
        if !self.guide.open {
            self.close(ctx);
        }
        if let Some(a) = action {
            self.act(a);
        }
    }
}

impl eframe::App for GuideApp {
    fn clear_color(&self, _visuals: &egui::Visuals) -> [f32; 4] {
        [0.0; 4]
    }

    fn logic(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if self.hide_frames > 0 {
            self.hide_frames -= 1;
            ctx.send_viewport_cmd_to(egui::ViewportId::ROOT, egui::ViewportCommand::Visible(false));
            ctx.request_repaint();
        }
        if self.quit.try_recv().is_ok() {
            tracing::info!("guide: quit requested by a later start");
            self.quitting = true;
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
        let events: Vec<_> = self.pad.try_iter().collect();
        for e in events {
            self.event(ctx, e);
        }
        if std::mem::take(&mut self.pending_open) && !self.guide.open {
            self.open(ctx);
        }
        self.shown.store(self.guide.open, Ordering::Relaxed);
        if self.guide.open {
            ctx.request_repaint();
        }
    }

    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        if !self.guide.open {
            return;
        }
        let ctx = ui.ctx().clone();
        let keys = [
            (Key::ArrowUp, PadEvent::Up),
            (Key::ArrowDown, PadEvent::Down),
            (Key::ArrowLeft, PadEvent::Left),
            (Key::ArrowRight, PadEvent::Right),
            (Key::Enter, PadEvent::A),
            (Key::Space, PadEvent::A),
            (Key::Escape, PadEvent::B),
            (Key::Backspace, PadEvent::B),
            (Key::Q, PadEvent::Lb),
            (Key::E, PadEvent::Rb),
        ];
        for (k, e) in keys {
            if ctx.input(|i| i.key_pressed(k)) {
                self.event(&ctx, e);
            }
        }
        if !self.guide.open {
            return;
        }
        let tag = self.gamertag.clone();
        let facts = guide::Facts {
            gamertag: &tag,
            friends_online: 0,
            requests: 0,
        };
        let now = self.now();
        if let Some(a) = self.guide.draw(ui, &facts, now) {
            if !self.guide.open {
                self.close(&ctx);
            }
            self.act(a);
        }
        if ctx.input(|i| i.viewport().close_requested()) && !self.quitting {
            ctx.send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.guide.open = false;
            self.close(&ctx);
        }
    }
}

fn main() -> eframe::Result {
    let args: Vec<String> = std::env::args().collect();
    let dev = args.iter().any(|a| a == "--dev") || std::env::var("FH2MP_DEV").as_deref() == Ok("1");
    let preview = args.iter().any(|a| a == "--preview");
    let config = identity::config_dir(dev);
    let logs = config.join("logs");
    let _ = std::fs::create_dir_all(&logs);
    let filter = tracing_subscriber::EnvFilter::try_from_env("FH2MP_GUIDE_LOG").unwrap_or_else(|_| "info".into());
    match OpenOptions::new().create(true).append(true).open(logs.join("guide.log")) {
        Ok(f) => tracing_subscriber::fmt()
            .with_env_filter(filter)
            .with_ansi(false)
            .with_writer(Mutex::new(f))
            .init(),
        Err(_) => tracing_subscriber::fmt().with_env_filter(filter).init(),
    }
    let server = match instance::claim_named(&config, "guide") {
        Ok(instance::Claim::First(s)) => Some(s),
        Ok(instance::Claim::Running) => {
            let req = if args.iter().any(|a| a == "--quit") {
                instance::Request::Quit
            } else {
                instance::Request::Open(None)
            };
            let _ = instance::send_named(&config, "guide", &req);
            tracing::info!(request = ?req, "a Guide already runs: request handed to it");
            return Ok(());
        }
        Err(e) => {
            tracing::warn!(error = %e, "guide: single-instance lock unavailable");
            None
        }
    };
    if args.iter().any(|a| a == "--quit") {
        return Ok(());
    }
    tracing::info!(version = env!("CARGO_PKG_VERSION"), dev, "guide starting");

    let (pad_tx, pad_rx) = channel();
    let (quit_tx, quit_rx) = channel();
    let shown = Arc::new(AtomicBool::new(false));
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("FH2MP Guide")
            .with_decorations(preview)
            .with_transparent(true)
            .with_taskbar(preview)
            .with_always_on_top()
            .with_position(if preview { egui::pos2(80.0, 80.0) } else { PARKED })
            .with_inner_size(if preview { [1280.0, 720.0] } else { [1.0, 1.0] }),
        ..Default::default()
    };
    eframe::run_native(
        "FH2MP Guide",
        options,
        Box::new(move |cc| {
            if !guide::fonts(&cc.egui_ctx) {
                tracing::info!("guide: Segoe UI not found, the default font is used");
            }
            let (ctx, flag) = (cc.egui_ctx.clone(), shown.clone());
            pad::start(pad_tx.clone(), move || ctx.request_repaint(), move || flag.load(Ordering::Relaxed));
            if let Some(server) = server {
                let ctx = cc.egui_ctx.clone();
                server.serve(move |r| {
                    match r {
                        instance::Request::Quit => {
                            let _ = quit_tx.send(());
                        }
                        instance::Request::Open(_) => {
                            let _ = pad_tx.send(PadEvent::Toggle);
                        }
                    }
                    ctx.request_repaint();
                });
            }
            Ok(Box::new(GuideApp {
                guide: guide::Guide::default(),
                pad: pad_rx,
                shown,
                quit: quit_rx,
                t0: Instant::now(),
                gamertag: gamertag(&config),
                config,
                preview,
                pending_open: preview,
                hide_frames: if preview { 0 } else { 3 },
                quitting: false,
            }))
        }),
    )
}
