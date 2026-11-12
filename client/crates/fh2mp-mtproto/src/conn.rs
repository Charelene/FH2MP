//! One TCP connection to the chat server: transport header, temporary auth key, then grammers' sans-IO encrypted state driven by one task.
//! Requests come in over a channel and are answered through oneshots; messages the server pushes leave as raw `Updates` bodies.

use std::collections::{HashMap, VecDeque};
use std::time::Duration;

use grammers_crypto::AuthKey;
use grammers_mtproto::authentication::Finished;
use grammers_mtproto::connection::State;
use grammers_mtproto::mtp::{Deserialization, DeserializeError, Encrypted, Plain};
use grammers_mtproto::transport::{self, Intermediate, Obfuscated, Transport};
use grammers_tl_types::{self as mtl, Deserializable, Serializable};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::{mpsc, oneshot};
use tokio::time::{interval, timeout};
use tracing::{debug, warn};

use crate::Error;
use crate::auth;
use crate::key::ServerKey;

type Tr = Box<dyn Transport + Send>;

/// What a connection needs.
#[derive(Clone, Debug)]
pub struct ConnConfig {
    pub addr: String,
    pub server_key: ServerKey,
    pub dc: i32,
    pub key_ttl: i32,
    pub obfuscated: bool,
    pub connect_timeout: Duration,
    pub ping_every: Duration,
}

struct Req {
    body: Vec<u8>,
    reply: oneshot::Sender<Result<Vec<u8>, Error>>,
}

/// A handle to send requests on one connection.
#[derive(Clone)]
pub(crate) struct Handle {
    tx: mpsc::Sender<Req>,
}

impl Handle {
    pub(crate) async fn call(&self, body: Vec<u8>) -> Result<Vec<u8>, Error> {
        let (reply, rx) = oneshot::channel();
        self.tx
            .send(Req { body, reply })
            .await
            .map_err(|_| Error::Disconnected("the connection is closed".into()))?;
        rx.await
            .map_err(|_| Error::Disconnected("the connection ended before the answer".into()))?
    }
}

/// An open connection.
pub(crate) struct Conn {
    pub key_id: [u8; 8],
    pub handle: Handle,
    pub updates: mpsc::Receiver<Vec<u8>>,
    pub closed: oneshot::Receiver<String>,
}

/// Opens a connection and makes a fresh temporary key on it.
pub(crate) async fn open(cfg: &ConnConfig) -> Result<Conn, Error> {
    let mut sock = timeout(cfg.connect_timeout, TcpStream::connect(&cfg.addr))
        .await
        .map_err(|_| Error::Io(format!("connect to {}: no answer in {:?}", cfg.addr, cfg.connect_timeout)))?
        .map_err(|e| Error::Io(format!("connect to {}: {e}", cfg.addr)))?;
    sock.set_nodelay(true).ok();
    let tr: Tr = if cfg.obfuscated {
        Box::new(Obfuscated::new(Intermediate::new()))
    } else {
        Box::new(Intermediate::new())
    };
    let mut st = State::new(tr, Plain::new());

    let (req, s1) = auth::step1().map_err(xerr)?;
    let ans = plain_call(&mut sock, &mut st, req.to_bytes()).await?;
    let (req, s2) = auth::step2(
        s1,
        mtl::enums::ResPq::from_bytes(&ans).map_err(derr)?,
        &cfg.server_key,
        cfg.dc,
        cfg.key_ttl,
    )
    .map_err(xerr)?;
    let ans = plain_call(&mut sock, &mut st, req.to_bytes()).await?;
    let (req, s3) = auth::step3(s2, mtl::enums::ServerDhParams::from_bytes(&ans).map_err(derr)?).map_err(xerr)?;
    let ans = plain_call(&mut sock, &mut st, req.to_bytes()).await?;
    let fin = auth::create_key(s3, mtl::enums::SetClientDhParamsAnswer::from_bytes(&ans).map_err(derr)?).map_err(xerr)?;
    let key_id = AuthKey::from_bytes(fin.auth_key).key_id();
    debug!(key = %hex8(&key_id), time_offset = fin.time_offset, "temporary auth key made");
    let st = st.into_encrypted(Finished {
        auth_key: fin.auth_key,
        time_offset: fin.time_offset,
        first_salt: fin.first_salt,
    });

    let (tx, rx) = mpsc::channel(256);
    let (up_tx, up_rx) = mpsc::channel(1024);
    let (closed_tx, closed_rx) = oneshot::channel();
    let ping_every = cfg.ping_every;
    tokio::spawn(async move {
        let mut io = Io {
            sock,
            st,
            rx,
            ups: up_tx,
            pending: HashMap::new(),
            queue: VecDeque::new(),
            containers: HashMap::new(),
            overflow: false,
        };
        let why = io.run(ping_every).await;
        for (_, p) in io.pending.drain() {
            if let Some(r) = p.reply {
                let _ = r.send(Err(Error::Disconnected(why.clone())));
            }
        }
        for (_, p) in io.queue.drain(..) {
            if let Some(r) = p.reply {
                let _ = r.send(Err(Error::Disconnected(why.clone())));
            }
        }
        let _ = closed_tx.send(why);
    });
    Ok(Conn {
        key_id,
        handle: Handle { tx },
        updates: up_rx,
        closed: closed_rx,
    })
}

fn xerr(e: auth::Error) -> Error {
    Error::Exchange(e.to_string())
}

fn derr(e: mtl::deserialize::Error) -> Error {
    Error::Exchange(format!("answer does not decode: {e}"))
}

pub(crate) fn hex8(b: &[u8; 8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn transport_refusal(e: &DeserializeError) -> Option<String> {
    match e {
        DeserializeError::Transport(transport::Error::BadStatus { status }) => Some(match status {
            404 => "the server does not know the auth key (-404): it expired or the server restarted".to_string(),
            429 => "the server refused: too many connections or key exchanges from this address (-429)".to_string(),
            s => format!("the server sent transport error -{s}"),
        }),
        _ => None,
    }
}

/// Writes what the state has packed, then reads until one transport packet was processed.
async fn round(sock: &mut TcpStream, st: &mut State<Tr, Plain>) -> Result<Vec<Deserialization>, Error> {
    loop {
        let (_, out) = st.io_buffers();
        if out.is_empty() {
            break;
        }
        let out = out.to_vec();
        sock.write_all(&out).await.map_err(|e| Error::Io(format!("write: {e}")))?;
        st.handle_net_write(out.len());
    }
    loop {
        if let Some(r) = st.unpack_next() {
            return r.map_err(|e| Error::Exchange(transport_refusal(&e).unwrap_or_else(|| format!("deserialize: {e}"))));
        }
        let (inp, _) = st.io_buffers();
        let n = timeout(Duration::from_secs(15), sock.read(inp))
            .await
            .map_err(|_| Error::Exchange("no answer for 15 s during the key exchange".into()))?
            .map_err(|e| Error::Io(format!("read: {e}")))?;
        st.handle_net_read(n)
            .map_err(|_| Error::Exchange("the server closed the connection during the key exchange".into()))?;
    }
}

/// One unencrypted request of the key exchange.
async fn plain_call(sock: &mut TcpStream, st: &mut State<Tr, Plain>, body: Vec<u8>) -> Result<Vec<u8>, Error> {
    st.buffer_request(&body)
        .ok_or_else(|| Error::Exchange("plain request did not fit".into()))?;
    st.pack_outgoing_buffer();
    for _ in 0..4 {
        for d in round(sock, st).await? {
            if let Deserialization::RpcResult(r) = d {
                return Ok(r.body);
            }
        }
    }
    Err(Error::Exchange("no answer to a key exchange step".into()))
}

struct Pending {
    body: Vec<u8>,
    reply: Option<oneshot::Sender<Result<Vec<u8>, Error>>>,
    retries: u8,
}

struct Io {
    sock: TcpStream,
    st: State<Tr, Encrypted>,
    rx: mpsc::Receiver<Req>,
    ups: mpsc::Sender<Vec<u8>>,
    pending: HashMap<i64, Pending>,
    queue: VecDeque<(i64, Pending)>,
    containers: HashMap<i64, Vec<i64>>,
    overflow: bool,
}

/// updatesTooLong#e317af7e: handed on when pushes were dropped because the reader fell behind, so it catches up with getDifference.
const UPDATES_TOO_LONG: [u8; 4] = 0xe317af7eu32.to_le_bytes();

impl Io {
    async fn run(&mut self, ping_every: Duration) -> String {
        let mut rbuf = vec![0u8; 64 * 1024];
        let mut ping = interval(ping_every);
        ping.tick().await;
        let mut ping_id: i64 = 0;
        loop {
            if let Err(why) = self.flush().await {
                return why;
            }
            tokio::select! {
                r = self.rx.recv() => match r {
                    Some(req) => self.queue.push_back((0, Pending { body: req.body, reply: Some(req.reply), retries: 0 })),
                    None => return "the client was dropped".into(),
                },
                n = self.sock.read(&mut rbuf) => {
                    let n = match n {
                        Ok(0) => return "the server closed the connection".into(),
                        Ok(n) => n,
                        Err(e) => return format!("read failed: {e}"),
                    };
                    if let Err(why) = self.feed(&rbuf[..n]).await {
                        return why;
                    }
                }
                _ = ping.tick() => {
                    ping_id += 1;
                    let body = mtl::functions::PingDelayDisconnect { ping_id, disconnect_delay: (ping_every.as_secs() as i32) + 15 }.to_bytes();
                    self.queue.push_back((0, Pending { body, reply: None, retries: 0 }));
                }
            }
        }
    }

    /// Moves queued requests into the MTP buffer and writes until nothing is left to send.
    async fn flush(&mut self) -> Result<(), String> {
        loop {
            let mut packed = Vec::new();
            while let Some((_, p)) = self.queue.front() {
                match self.st.buffer_request(&p.body) {
                    Some(id) => {
                        let (_, p) = self.queue.pop_front().unwrap();
                        packed.push(id.into_inner());
                        self.pending.insert(id.into_inner(), p);
                    }
                    None => break,
                }
            }
            if let Some(cid) = self.st.pack_outgoing_buffer()
                && (packed.len() > 1 || (packed.len() == 1 && packed[0] != cid.into_inner()))
            {
                self.containers.insert(cid.into_inner(), packed);
                if self.containers.len() > 512 {
                    let oldest = *self.containers.keys().min().unwrap();
                    self.containers.remove(&oldest);
                }
            }
            let mut wrote = false;
            loop {
                let (_, out) = self.st.io_buffers();
                if out.is_empty() {
                    break;
                }
                let n = match self.sock.write(out).await {
                    Ok(0) => return Err("the socket took no bytes".into()),
                    Ok(n) => n,
                    Err(e) => return Err(format!("write failed: {e}")),
                };
                self.st.handle_net_write(n);
                wrote = true;
            }
            if !wrote || self.queue.is_empty() {
                return Ok(());
            }
        }
    }

    /// Hands received bytes to the state and processes every complete packet.
    async fn feed(&mut self, mut data: &[u8]) -> Result<(), String> {
        while !data.is_empty() {
            let (inp, _) = self.st.io_buffers();
            let n = inp.len().min(data.len());
            if n == 0 {
                return Err("receive buffer full (a frame larger than 1 MiB)".into());
            }
            inp[..n].copy_from_slice(&data[..n]);
            data = &data[n..];
            self.st.handle_net_read(n).map_err(|e| e.to_string())?;
            while let Some(r) = self.st.unpack_next() {
                match r {
                    Ok(ds) => {
                        for d in ds {
                            self.handle(d)?;
                        }
                    }
                    Err(e) => return Err(transport_refusal(&e).unwrap_or_else(|| format!("a server message does not decode: {e}"))),
                }
            }
        }
        Ok(())
    }

    fn answer(&mut self, msg_id: i64, r: Result<Vec<u8>, Error>) {
        if let Some(Pending { reply: Some(tx), .. }) = self.pending.remove(&msg_id) {
            let _ = tx.send(r);
        }
    }

    fn handle(&mut self, d: Deserialization) -> Result<(), String> {
        match d {
            Deserialization::RpcResult(r) => self.answer(r.msg_id.into_inner(), Ok(r.body)),
            Deserialization::RpcError(e) => self.answer(
                e.msg_id.into_inner(),
                Err(Error::Rpc {
                    code: e.error.error_code,
                    message: e.error.error_message.clone(),
                }),
            ),
            Deserialization::BadMessage(b) => {
                // The id can name one request or the container it went in.
                let ids = self
                    .containers
                    .remove(&b.msg_id.into_inner())
                    .unwrap_or_else(|| vec![b.msg_id.into_inner()]);
                for id in ids {
                    let Some(mut p) = self.pending.remove(&id) else { continue };
                    if b.retryable() && p.retries < 3 {
                        p.retries += 1;
                        debug!(code = b.code, why = b.description(), "server refused a message; sending it again");
                        self.queue.push_front((0, p));
                    } else if let Some(tx) = p.reply {
                        let _ = tx.send(Err(Error::Protocol(format!("bad_msg_notification {}: {}", b.code, b.description()))));
                    }
                }
                if b.fatal() {
                    return Err(format!(
                        "bad_msg_notification {} ({}): the session is out of step, reconnecting",
                        b.code,
                        b.description()
                    ));
                }
            }
            Deserialization::Update(body) => {
                if self.overflow && self.ups.try_send(UPDATES_TOO_LONG.to_vec()).is_ok() {
                    self.overflow = false;
                }
                if !self.overflow && self.ups.try_send(body).is_err() {
                    warn!("update reader fell behind; pushes dropped until it catches up with getDifference");
                    self.overflow = true;
                }
            }
            Deserialization::OwnUpdate { .. } => {}
            Deserialization::Failure(f) => self.answer(
                f.msg_id.into_inner(),
                Err(Error::Protocol(format!("answer does not decode: {}", f.error))),
            ),
        }
        Ok(())
    }
}
