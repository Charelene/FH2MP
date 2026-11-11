//! The chat client: keeps one signed-in connection up (reconnecting with a fresh temporary key each time), signs in with the account key,
//! answers requests and turns the server's pushes into an ordered event stream, catching up with updates.getDifference after gaps.

use std::sync::Arc;
use std::sync::atomic::{AtomicI32, Ordering};
use std::time::Duration;

use ed25519_dalek::{Signer, SigningKey};
use fh2mp_tl::{Deserializable, LAYER, RemoteCall, enums, functions, types};
use tokio::sync::{mpsc, watch};
use tracing::{info, warn};

use crate::Error;
use crate::conn::{self, ConnConfig, Handle, hex8};
use crate::key::{ServerKey, sign_message};

/// Where and how to reach the server.
#[derive(Clone, Debug)]
pub struct Config {
    /// host:port of the chat server.
    pub addr: String,
    /// The realm's pinned server key.
    pub server_key: ServerKey,
    /// The dc id the server expects in the key exchange (1).
    pub dc: i32,
    /// Lifetime asked for each temporary key, seconds (the server may cap it).
    pub key_ttl: i32,
    /// obfuscated2 around the intermediate framing (looks like random bytes on the wire).
    pub obfuscated: bool,
    /// Sent in help.init, for the server's log lines.
    pub app_version: String,
    pub platform: String,
    pub connect_timeout: Duration,
    pub ping_every: Duration,
    /// Longest wait between reconnect attempts.
    pub max_backoff: Duration,
}

impl Config {
    pub fn new(addr: impl Into<String>, server_key: ServerKey) -> Self {
        Self {
            addr: addr.into(),
            server_key,
            dc: 1,
            key_ttl: 24 * 3600,
            obfuscated: true,
            app_version: env!("CARGO_PKG_VERSION").into(),
            platform: std::env::consts::OS.into(),
            connect_timeout: Duration::from_secs(10),
            ping_every: Duration::from_secs(60),
            max_backoff: Duration::from_secs(30),
        }
    }
}

/// The account that signs in: its Ed25519 key (the launcher's identity seed) and this install's device id.
#[derive(Clone)]
pub struct Account {
    pub key: SigningKey,
    pub device_id: i64,
}

/// What the client reports, in order.
#[derive(Debug)]
pub enum Event {
    /// Signed in and caught up; requests flow from here on.
    Connected { user: types::User, pts: i32 },
    /// One update, stored ones in pts order.
    Update(enums::Update),
    /// Stored updates the client never got were dropped by the server (differenceTooLong): whatever was built from them must be reloaded.
    /// The local pts is now `pts`.
    Resync { pts: i32 },
    /// The connection was lost; the client tries again in `retry_in`.
    Disconnected { why: String, retry_in: Duration },
    /// The server ended this session (another sign-in of the account, a ban, a revoke). The client stops and does not sign in again by itself.
    SignedOut { reason: String },
    /// The sign-in was refused (unknown account, banned, bad signature). The client stops.
    Refused { error: Error },
}

/// Handle to a running client; cheap to clone.
#[derive(Clone)]
pub struct Client {
    cur: watch::Receiver<Option<Handle>>,
    pts: Arc<AtomicI32>,
    stop: Arc<watch::Sender<bool>>,
}

/// Starts the client; `pts` is the last update sequence number the caller has (None: start from the server's current state).
pub fn start(cfg: Config, account: Account, pts: Option<i32>) -> (Client, mpsc::Receiver<Event>) {
    let (cur_tx, cur_rx) = watch::channel(None);
    let (ev_tx, ev_rx) = mpsc::channel(256);
    let (stop_tx, stop_rx) = watch::channel(false);
    let pts_cell = Arc::new(AtomicI32::new(pts.unwrap_or(-1)));
    let sup = Supervisor {
        cfg,
        account,
        ev: ev_tx,
        cur: cur_tx,
        pts: pts_cell.clone(),
        stop: stop_rx,
    };
    tokio::spawn(sup.run());
    (
        Client {
            cur: cur_rx,
            pts: pts_cell,
            stop: Arc::new(stop_tx),
        },
        ev_rx,
    )
}

impl Client {
    /// Sends a request once the client is signed in and returns its answer. A request in flight when the connection drops fails with
    /// `Error::Disconnected` (it may or may not have run on the server). Wrap in `tokio::time::timeout` for a deadline.
    pub async fn invoke<R: RemoteCall>(&self, req: &R) -> Result<R::Return, Error> {
        let body = req.to_bytes();
        if body.len() > MAX_REQUEST {
            return Err(Error::Protocol(format!("request of {} bytes, at most {MAX_REQUEST}", body.len())));
        }
        let h = {
            let mut cur = self.cur.clone();
            let h = cur
                .wait_for(|h| h.is_some())
                .await
                .map_err(|_| Error::Disconnected("the client stopped".into()))?;
            h.clone().unwrap()
        };
        let ans = h.call(body).await?;
        R::Return::from_bytes(&ans).map_err(|e| Error::Protocol(format!("answer does not decode: {e}")))
    }

    /// The last update sequence number applied (-1 before the first sign-in when none was given).
    pub fn pts(&self) -> i32 {
        self.pts.load(Ordering::SeqCst)
    }

    /// Stops the client: the connection closes, no reconnect.
    pub fn stop(&self) {
        let _ = self.stop.send(true);
    }
}

const MAX_REQUEST: usize = 1 << 20;

enum Ended {
    Lost { why: String, was_up: bool },
    SignedOut(String),
    Refused(Error),
    Stopped,
}

struct Supervisor {
    cfg: Config,
    account: Account,
    ev: mpsc::Sender<Event>,
    cur: watch::Sender<Option<Handle>>,
    pts: Arc<AtomicI32>,
    stop: watch::Receiver<bool>,
}

async fn call<R: RemoteCall>(h: &Handle, req: &R) -> Result<R::Return, Error> {
    let ans = h.call(req.to_bytes()).await?;
    R::Return::from_bytes(&ans).map_err(|e| Error::Protocol(format!("answer does not decode: {e}")))
}

impl Supervisor {
    async fn run(mut self) {
        let mut backoff = Duration::from_secs(1);
        loop {
            let ended = self.session().await;
            let _ = self.cur.send(None);
            match ended {
                Ended::Stopped => return,
                Ended::SignedOut(reason) => {
                    info!(%reason, "chat session ended by the server; not signing in again");
                    let _ = self.ev.send(Event::SignedOut { reason }).await;
                    return;
                }
                Ended::Refused(error) => {
                    warn!(%error, "chat sign-in refused; stopping");
                    let _ = self.ev.send(Event::Refused { error }).await;
                    return;
                }
                Ended::Lost { why, was_up } => {
                    if was_up {
                        backoff = Duration::from_secs(1);
                    }
                    warn!(%why, retry_in = ?backoff, "chat connection lost");
                    let _ = self.ev.send(Event::Disconnected { why, retry_in: backoff }).await;
                    tokio::select! {
                        _ = tokio::time::sleep(backoff) => {}
                        _ = self.stop.changed() => return,
                    }
                    backoff = (backoff * 2).min(self.cfg.max_backoff);
                }
            }
        }
    }

    async fn emit(&self, e: Event) {
        let _ = self.ev.send(e).await;
    }

    async fn session(&mut self) -> Ended {
        if *self.stop.borrow() {
            return Ended::Stopped;
        }
        let cc = ConnConfig {
            addr: self.cfg.addr.clone(),
            server_key: self.cfg.server_key.clone(),
            dc: self.cfg.dc,
            key_ttl: self.cfg.key_ttl,
            obfuscated: self.cfg.obfuscated,
            connect_timeout: self.cfg.connect_timeout,
            ping_every: self.cfg.ping_every,
        };
        let mut c = match conn::open(&cc).await {
            Ok(c) => c,
            Err(e) => {
                return Ended::Lost {
                    why: e.to_string(),
                    was_up: false,
                };
            }
        };
        let h = c.handle.clone();
        let lost = |e: Error| Ended::Lost {
            why: e.to_string(),
            was_up: false,
        };
        if let Err(e) = call(
            &h,
            &functions::help::Init {
                layer: LAYER,
                app_version: self.cfg.app_version.clone(),
                platform: self.cfg.platform.clone(),
            },
        )
        .await
        {
            return lost(e);
        }
        let enums::auth::Challenge::Challenge(ch) = match call(&h, &functions::auth::GetChallenge {}).await {
            Ok(ch) => ch,
            Err(e) => return lost(e),
        };
        let pubkey = self.account.key.verifying_key().to_bytes();
        let dev = self.account.device_id.to_le_bytes();
        let msg = sign_message(&ch.realm, "mtproto", &ch.challenge, &[&pubkey, &c.key_id, &dev]);
        let sig = self.account.key.sign(&msg).to_bytes();
        let req = functions::auth::SignIn {
            challenge: ch.challenge,
            pubkey: pubkey.to_vec(),
            signature: sig.to_vec(),
            device_id: self.account.device_id,
        };
        let user = match call(&h, &req).await {
            Ok(enums::auth::Authorization::Authorization(a)) => {
                let enums::User::User(u) = a.user;
                u
            }
            Err(e) => {
                let refused =
                    matches!(&e, Error::Rpc { code: 401 | 403, .. }) || matches!(&e, Error::Rpc { message, .. } if message == "ACCOUNT_UNKNOWN");
                return if refused { Ended::Refused(e) } else { lost(e) };
            }
        };
        info!(xuid = format_args!("{:#x}", user.id), gamertag = %user.gamertag, key = %hex8(&c.key_id), "chat signed in");
        let mut pts = self.pts.load(Ordering::SeqCst);
        if pts < 0 {
            match call(&h, &functions::updates::GetState {}).await {
                Ok(enums::updates::State::State(s)) => pts = s.pts,
                Err(e) => return lost(e),
            }
        } else if let Err(e) = self.catch_up(&h, &mut pts).await {
            return e;
        }
        self.pts.store(pts, Ordering::SeqCst);
        let _ = self.cur.send(Some(h.clone()));
        self.emit(Event::Connected { user, pts }).await;

        loop {
            tokio::select! {
                u = c.updates.recv() => {
                    let Some(body) = u else { continue };
                    if let Err(e) = self.push(&h, &body, &mut pts).await {
                        return e;
                    }
                    self.pts.store(pts, Ordering::SeqCst);
                }
                why = &mut c.closed => {
                    return Ended::Lost { why: why.unwrap_or_else(|_| "the connection task ended".into()), was_up: true };
                }
                _ = self.stop.changed() => return Ended::Stopped,
            }
        }
    }

    /// One pushed `Updates`: applied when it follows our pts, dropped when we have it, else we catch up.
    async fn push(&self, h: &Handle, body: &[u8], pts: &mut i32) -> Result<(), Ended> {
        match enums::Updates::from_bytes(body) {
            Ok(enums::Updates::Updates(u)) => {
                if u.pts <= *pts {
                    return Ok(());
                }
                if *pts + u.pts_count == u.pts {
                    for x in u.updates {
                        self.emit(Event::Update(x)).await;
                    }
                    *pts = u.pts;
                    return Ok(());
                }
                info!(local = *pts, pushed = u.pts, count = u.pts_count, "update gap; catching up");
                self.catch_up(h, pts).await
            }
            Ok(enums::Updates::UpdateShort(s)) => match s.update {
                enums::Update::LoggedOut(l) => Err(Ended::SignedOut(l.reason)),
                other => {
                    self.emit(Event::Update(other)).await;
                    Ok(())
                }
            },
            Ok(enums::Updates::TooLong) => self.catch_up(h, pts).await,
            Err(e) => {
                let id = body.get(..4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).unwrap_or(0);
                warn!(constructor = format_args!("{id:08x}"), error = %e, "a pushed message is not an Updates of layer {LAYER}; ignored");
                Ok(())
            }
        }
    }

    /// updates.getDifference from pts until the server says we are current.
    async fn catch_up(&self, h: &Handle, pts: &mut i32) -> Result<(), Ended> {
        let lost = |e: Error| Ended::Lost {
            why: e.to_string(),
            was_up: true,
        };
        loop {
            match call(h, &functions::updates::GetDifference { pts: *pts, limit: 100 })
                .await
                .map_err(lost)?
            {
                enums::updates::Difference::Empty(e) => {
                    *pts = e.pts;
                    return Ok(());
                }
                enums::updates::Difference::Difference(d) => {
                    for x in d.new_updates {
                        self.emit(Event::Update(x)).await;
                    }
                    let enums::updates::State::State(s) = d.state;
                    *pts = s.pts;
                    return Ok(());
                }
                enums::updates::Difference::Slice(s) => {
                    for x in s.new_updates {
                        self.emit(Event::Update(x)).await;
                    }
                    let enums::updates::State::State(st) = s.intermediate_state;
                    *pts = st.pts;
                }
                enums::updates::Difference::TooLong(t) => {
                    warn!(
                        local = *pts,
                        server = t.pts,
                        "stored updates were dropped before this client got them; resync"
                    );
                    *pts = t.pts;
                    self.emit(Event::Resync { pts: t.pts }).await;
                    return Ok(());
                }
            }
        }
    }
}
