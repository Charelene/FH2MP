//! fh2mp-chat, the FH2MP chat core: signs the account in to the chat server (MTProto, our own schema) with the launcher's key and shows
//! friends with presence and the club chat in its own window. `--dev` uses the dev realm and the dev launcher's key and settings, as the
//! dev launcher does. Log: `<launcher config dir>/logs/chat.log`.
//!
//! One core runs per config dir (instance.rs). `--background` starts it hidden and keeps it running when its window is closed (the launcher
//! starts it this way so the in-game Guide can reach it during play); `--open [club|requests|friend:<xuid>]` shows the running core's window,
//! `--quit` closes it. The in-game Guide (guide.rs, pad.rs) opens over the game on a 1 s hold of the Guide button or Home; `--guide-preview`
//! shows it in a normal window, FH2MP_NO_GUIDE=1 turns it off.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod guide;
mod identity;
mod instance;
#[allow(dead_code)] // the chat link server that uses it comes with the Guide shell (plan phase 3)
mod link_wire;
mod net;
mod pad;
mod realm;

use std::fs::OpenOptions;
use std::sync::Mutex;

use eframe::egui;
use fh2mp_mtproto::{Account, Config};

fn main() -> eframe::Result {
    let dev = std::env::args().any(|a| a == "--dev") || std::env::var("FH2MP_DEV").as_deref() == Ok("1");
    let config = identity::config_dir(dev);
    let logs = config.join("logs");
    let _ = std::fs::create_dir_all(&logs);
    let filter = tracing_subscriber::EnvFilter::try_from_env("FH2MP_CHAT_LOG").unwrap_or_else(|_| "info".into());
    match OpenOptions::new().create(true).append(true).open(logs.join("chat.log")) {
        Ok(f) => tracing_subscriber::fmt()
            .with_env_filter(filter)
            .with_ansi(false)
            .with_writer(Mutex::new(f))
            .init(),
        Err(_) => tracing_subscriber::fmt().with_env_filter(filter).init(),
    }
    let args: Vec<String> = std::env::args().collect();
    let (request, background) = instance::from_args(&args);
    let server = match instance::claim(&config) {
        Ok(instance::Claim::First(server)) => Some(server),
        Ok(instance::Claim::Running) => {
            let req = request.unwrap_or(instance::Request::Open(None));
            match instance::send(&config, &req) {
                Ok(()) => tracing::info!(request = ?req, "a chat core already runs: request handed to it, this start exits"),
                Err(e) => tracing::error!(request = ?req, error = %e, "a chat core already runs but did not take the request"),
            }
            return Ok(());
        }
        Err(e) => {
            tracing::warn!(error = %e, "single-instance lock unavailable: this core runs without it and cannot be reached by a later start");
            None
        }
    };
    if request == Some(instance::Request::Quit) {
        tracing::info!("--quit and no chat core running: nothing to close");
        return Ok(());
    }
    tracing::info!(version = env!("CARGO_PKG_VERSION"), dev, background, "chat core starting");

    let mut blocked = None;
    let mut session = None;
    let realm_name = if dev { "dev" } else { "prod" };
    match (identity::load(dev), realm::pick(dev)) {
        (Ok(Some((key, source))), Ok(realm)) => {
            tracing::info!(realm = realm.name, addr = %realm.addr, key_from = ?source, server_key = ?realm.key, "account key and realm found");
            let mut cfg = Config::new(realm.addr.clone(), realm.key.clone());
            cfg.app_version = format!("fh2mp-chat {}", env!("CARGO_PKG_VERSION"));
            session = Some((
                cfg,
                Account {
                    key,
                    device_id: realm::device_id(&config),
                },
            ));
        }
        (Ok(None), _) => {
            tracing::warn!("no account key on this PC (keyring or identity.key): nothing to sign in with");
            blocked = Some(app::Blocked::NoAccount);
        }
        (Err(e), _) => {
            tracing::error!(error = %e, "account key unreadable");
            blocked = Some(app::Blocked::NoServer(format!("the account key on this PC cannot be read ({e})")));
        }
        (_, Err(e)) => {
            tracing::warn!(error = %e, "no chat server for this realm");
            blocked = Some(app::Blocked::NoServer(e));
        }
    }

    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .expect("tokio runtime");
    let handle = rt.handle().clone();
    let (cmd_tx, cmd_rx) = tokio::sync::mpsc::unbounded_channel();
    let (ui_tx, ui_rx) = std::sync::mpsc::channel();
    let (inst_tx, inst_rx) = std::sync::mpsc::channel();
    let guide_preview = args.iter().any(|a| a == "--guide-preview");
    let guide_on = std::env::var("FH2MP_NO_GUIDE").as_deref() != Ok("1");
    let (pad_tx, pad_rx) = std::sync::mpsc::channel();
    let guide_shown = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let open_at = match &request {
        Some(instance::Request::Open(t)) => t.clone(),
        _ => None,
    };
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("FH2MP Chat")
            .with_inner_size([900.0, 620.0])
            .with_min_inner_size([640.0, 420.0])
            .with_transparent(true) // the Guide uses this window over the game (app.rs enter_guide); the chat paints it opaque
            .with_visible(!background || request.is_some()),
        ..Default::default()
    };
    let result = eframe::run_native(
        "FH2MP Chat",
        options,
        Box::new(move |cc| {
            app::style(&cc.egui_ctx);
            if let Some((cfg, account)) = session {
                handle.spawn(net::run(cfg, account, cmd_rx, ui_tx, cc.egui_ctx.clone()));
            }
            if let Some(server) = server {
                let ctx = cc.egui_ctx.clone();
                server.serve(move |r| {
                    let _ = inst_tx.send(r);
                    ctx.request_repaint();
                });
            }
            let mut app = app::App::new(cmd_tx, ui_rx, realm_name, blocked);
            app.set_instance(inst_rx, background, open_at);
            if guide_on || guide_preview {
                if !guide::fonts(&cc.egui_ctx) {
                    tracing::info!("guide: Segoe UI not found, the Guide uses the default font");
                }
                if guide_on {
                    let (ctx, shown) = (cc.egui_ctx.clone(), guide_shown.clone());
                    pad::start(
                        pad_tx,
                        move || ctx.request_repaint(),
                        move || shown.load(std::sync::atomic::Ordering::Relaxed),
                    );
                }
                app.set_guide(pad_rx, guide_shown, guide_preview);
            }
            Ok(Box::new(app))
        }),
    );
    tracing::info!("chat core closing");
    rt.shutdown_timeout(std::time::Duration::from_secs(2));
    result
}
