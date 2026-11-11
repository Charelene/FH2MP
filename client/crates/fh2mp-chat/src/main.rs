//! fh2mp-chat, the FH2MP chat core: signs the account in to the chat server (MTProto, our own schema) with the launcher's key and shows
//! friends with presence and the club chat in its own window. `--dev` uses the dev realm and the dev launcher's key and settings, as the
//! dev launcher does. Log: `<launcher config dir>/logs/chat.log`.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod app;
mod identity;
mod net;
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
    tracing::info!(version = env!("CARGO_PKG_VERSION"), dev, "chat core starting");

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
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("FH2MP Chat")
            .with_inner_size([900.0, 620.0])
            .with_min_inner_size([640.0, 420.0]),
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
            Ok(Box::new(app::App::new(cmd_tx, ui_rx, realm_name, blocked)))
        }),
    );
    tracing::info!("chat core closing");
    rt.shutdown_timeout(std::time::Duration::from_secs(2));
    result
}
