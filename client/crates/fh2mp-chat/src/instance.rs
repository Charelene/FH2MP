//! One chat core per launcher config dir. The first start holds `<config>/chat/instance.lock` for its whole life and answers on a loopback
//! port that it writes, with a random secret, to `<config>/chat/instance.json`. A later start (the launcher's Chats button: `--open club`, or
//! `--quit` when the launcher exits) hands its request to the running core and exits. Only a process that can read the user's config dir knows
//! the secret, and the port listens on 127.0.0.1 only.

use std::fs::{File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

const MAGIC: &str = "FH2MP-CHAT-1";
const MAX_LINE: u64 = 512;

/// What a later start asks the running core to do.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Request {
    /// Show and focus the window, at a chat when one is named (`club`, `requests`, `friend:<xuid hex>`).
    Open(Option<String>),
    /// Close the core (the launcher is closing).
    Quit,
}

impl Request {
    fn to_line(&self) -> String {
        match self {
            Request::Open(None) => "open".into(),
            Request::Open(Some(t)) => format!("open {t}"),
            Request::Quit => "quit".into(),
        }
    }

    fn parse(text: &str) -> Option<Request> {
        let mut parts = text.splitn(2, ' ');
        match (parts.next()?, parts.next()) {
            ("open", None) => Some(Request::Open(None)),
            ("open", Some(t)) if !t.is_empty() && t.len() <= 64 && t.chars().all(|c| c.is_ascii_alphanumeric() || c == ':' || c == '_') => {
                Some(Request::Open(Some(t.to_string())))
            }
            ("quit", None) => Some(Request::Quit),
            _ => None,
        }
    }
}

/// The result of trying to be the one core.
pub enum Claim {
    /// This process is the core: serve requests with [`Server::serve`].
    First(Server),
    /// Another core holds the lock.
    Running,
}

/// The running core's side: the held lock and the loopback listener.
pub struct Server {
    _lock: File,
    listener: TcpListener,
    secret: [u8; 32],
}

fn paths(config: &Path, name: &str) -> (PathBuf, PathBuf) {
    let dir = config.join(name);
    (dir.join("instance.lock"), dir.join("instance.json"))
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn same(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// Takes the lock, or says another core has it.
pub fn claim(config: &Path) -> io::Result<Claim> {
    claim_named(config, "chat")
}

/// The same for another program of this crate (the Guide: "guide"), with its own lock and port file in `<config>/<name>/`.
pub fn claim_named(config: &Path, name: &str) -> io::Result<Claim> {
    let (lock_path, info_path) = paths(config, name);
    std::fs::create_dir_all(lock_path.parent().unwrap_or(config))?;
    let lock = OpenOptions::new().create(true).truncate(false).write(true).open(&lock_path)?;
    match lock.try_lock() {
        Ok(()) => {}
        Err(std::fs::TryLockError::WouldBlock) => return Ok(Claim::Running),
        Err(std::fs::TryLockError::Error(e)) => return Err(e),
    }
    let listener = TcpListener::bind(SocketAddr::from((Ipv4Addr::LOCALHOST, 0)))?;
    let mut secret = [0u8; 32];
    getrandom::fill(&mut secret).map_err(|e| io::Error::other(e.to_string()))?;
    let port = listener.local_addr()?.port();
    let info = serde_json::json!({"v": 1, "port": port, "secret": hex(&secret), "pid": std::process::id()});
    std::fs::write(&info_path, info.to_string())?;
    Ok(Claim::First(Server {
        _lock: lock,
        listener,
        secret,
    }))
}

/// Hands a request to the running core. The core may have taken the lock a moment ago and not yet written its port: retried for 3 s.
pub fn send(config: &Path, req: &Request) -> io::Result<()> {
    send_named(config, "chat", req)
}

/// Hands a request to the running program `name` (see [`claim_named`]).
pub fn send_named(config: &Path, name: &str, req: &Request) -> io::Result<()> {
    let (_, info_path) = paths(config, name);
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        match try_send(&info_path, req) {
            Ok(()) => return Ok(()),
            Err(e) if Instant::now() >= deadline => return Err(e),
            Err(_) => std::thread::sleep(Duration::from_millis(100)),
        }
    }
}

fn try_send(info_path: &Path, req: &Request) -> io::Result<()> {
    let raw = std::fs::read(info_path)?;
    let v: serde_json::Value = serde_json::from_slice(&raw).map_err(|e| io::Error::other(e.to_string()))?;
    let port = v
        .get("port")
        .and_then(|p| p.as_u64())
        .filter(|p| *p > 0 && *p <= 65535)
        .ok_or_else(|| io::Error::other("no port"))?;
    let secret = v.get("secret").and_then(|s| s.as_str()).ok_or_else(|| io::Error::other("no secret"))?;
    let mut s = TcpStream::connect_timeout(&SocketAddr::from((Ipv4Addr::LOCALHOST, port as u16)), Duration::from_secs(1))?;
    s.set_read_timeout(Some(Duration::from_secs(2)))?;
    s.write_all(format!("{MAGIC} {secret} {}\n", req.to_line()).as_bytes())?;
    let mut answer = String::new();
    BufReader::new(s).take(16).read_line(&mut answer)?;
    if answer.trim() == "ok" {
        Ok(())
    } else {
        Err(io::Error::other(format!("the running chat core answered {:?}", answer.trim())))
    }
}

impl Server {
    /// Answers requests on its own thread; `on` gets each valid one. Wrong secrets and malformed lines get "no" and are logged.
    pub fn serve(self, on: impl Fn(Request) + Send + 'static) {
        std::thread::Builder::new()
            .name("chat-instance".into())
            .spawn(move || {
                for conn in self.listener.incoming() {
                    let Ok(mut conn) = conn else { continue };
                    let _ = conn.set_read_timeout(Some(Duration::from_secs(2)));
                    let mut line = String::new();
                    let read = BufReader::new(&mut conn).take(MAX_LINE).read_line(&mut line);
                    let req = read.ok().and_then(|_| self.check(line.trim_end()));
                    let _ = conn.write_all(if req.is_some() { b"ok\n" } else { b"no\n" });
                    match req {
                        Some(r) => {
                            tracing::info!(request = ?r, "request from a later start of the chat core");
                            on(r);
                        }
                        None => tracing::warn!(bytes = line.len(), "instance request refused: wrong secret or malformed"),
                    }
                }
            })
            .expect("instance thread");
    }

    fn check(&self, line: &str) -> Option<Request> {
        let rest = line.strip_prefix(MAGIC)?.strip_prefix(' ')?;
        let (secret, req) = rest.split_once(' ')?;
        if !same(secret.as_bytes(), hex(&self.secret).as_bytes()) {
            return None;
        }
        Request::parse(req)
    }
}

/// Reads `--open [target]`, `--quit` and `--background` from the command line: (the request for a running core, start hidden).
pub fn from_args(args: &[String]) -> (Option<Request>, bool) {
    let mut req = None;
    let mut background = false;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--open" => {
                let target = args.get(i + 1).filter(|t| !t.starts_with("--")).cloned();
                if target.is_some() {
                    i += 1;
                }
                req = Some(Request::Open(target));
            }
            "--quit" => req = Some(Request::Quit),
            "--background" => background = true,
            _ => {}
        }
        i += 1;
    }
    (req, background)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    fn args(a: &[&str]) -> Vec<String> {
        std::iter::once("fh2mp-chat").chain(a.iter().copied()).map(String::from).collect()
    }

    #[test]
    fn command_line() {
        assert_eq!(from_args(&args(&[])), (None, false));
        assert_eq!(from_args(&args(&["--open"])), (Some(Request::Open(None)), false));
        assert_eq!(
            from_args(&args(&["--open", "club", "--dev"])),
            (Some(Request::Open(Some("club".into()))), false)
        );
        assert_eq!(from_args(&args(&["--open", "--dev"])), (Some(Request::Open(None)), false));
        assert_eq!(from_args(&args(&["--background", "--dev"])), (None, true));
        assert_eq!(from_args(&args(&["--quit"])), (Some(Request::Quit), false));
    }

    #[test]
    fn request_lines() {
        for r in [Request::Open(None), Request::Open(Some("friend:9000027d5236b".into())), Request::Quit] {
            assert_eq!(Request::parse(&r.to_line()), Some(r));
        }
        assert_eq!(Request::parse("open ../etc"), None);
        assert_eq!(Request::parse("open a b"), None);
        assert_eq!(Request::parse("quit now"), None);
        assert_eq!(Request::parse("delete"), None);
    }

    // The second start reaches the first through the lock and the port file; a wrong secret is refused.
    #[test]
    fn second_start_hands_over() {
        let dir = std::env::temp_dir().join(format!("fh2mp-chat-instance-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let Claim::First(server) = claim(&dir).expect("claim") else {
            panic!("first claim found a running core")
        };
        assert!(matches!(claim(&dir).expect("second claim"), Claim::Running));
        let (tx, rx) = mpsc::channel();
        server.serve(move |r| {
            let _ = tx.send(r);
        });
        send(&dir, &Request::Open(Some("club".into()))).expect("send");
        assert_eq!(rx.recv_timeout(Duration::from_secs(3)).unwrap(), Request::Open(Some("club".into())));

        let raw = std::fs::read(dir.join("chat").join("instance.json")).unwrap();
        let v: serde_json::Value = serde_json::from_slice(&raw).unwrap();
        let port = v["port"].as_u64().unwrap() as u16;
        let mut s = TcpStream::connect((Ipv4Addr::LOCALHOST, port)).unwrap();
        s.write_all(format!("{MAGIC} {} quit\n", "00".repeat(32)).as_bytes()).unwrap();
        let mut answer = String::new();
        BufReader::new(s).read_line(&mut answer).unwrap();
        assert_eq!(answer.trim(), "no");
        assert!(rx.recv_timeout(Duration::from_millis(300)).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
