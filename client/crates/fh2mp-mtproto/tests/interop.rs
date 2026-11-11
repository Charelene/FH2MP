//! The Rust client against the Go chat server (native/cmd/fh2mp-mt-testserver: the real internal/mt server with in-memory accounts and
//! updates). The server binary is FH2MP_MT_TESTSERVER, else it is built with `go build`; without Go the test is skipped.

use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use base64::Engine;
use ed25519_dalek::SigningKey;
use fh2mp_mtproto::tl::{enums, functions};
use fh2mp_mtproto::{Account, Config, Error, Event, ServerKey, start};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};
use tokio::sync::mpsc::Receiver;
use tokio::time::timeout;

fn server_binary() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("FH2MP_MT_TESTSERVER") {
        return Some(p.into());
    }
    let native = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../native");
    let out = std::env::temp_dir().join(format!("fh2mp-mt-testserver-{}{}", std::process::id(), std::env::consts::EXE_SUFFIX));
    let st = std::process::Command::new("go")
        .args(["build", "-o"])
        .arg(&out)
        .arg("./cmd/fh2mp-mt-testserver")
        .current_dir(native)
        .status()
        .ok()?;
    st.success().then_some(out)
}

struct Server {
    _child: Child,
    stdin: ChildStdin,
    out: tokio::io::Lines<BufReader<ChildStdout>>,
    addr: String,
    key: ServerKey,
}

impl Server {
    async fn start(accounts: &[(&SigningKey, i64, &str)]) -> Option<Server> {
        let Some(bin) = server_binary() else {
            eprintln!("skipped: no Go toolchain and no FH2MP_MT_TESTSERVER");
            return None;
        };
        let mut cmd = Command::new(bin);
        for (k, x, tag) in accounts {
            let hex: String = k.verifying_key().to_bytes().iter().map(|b| format!("{b:02x}")).collect();
            cmd.arg("-account").arg(format!("{hex}={x:x}:{tag}"));
        }
        let mut child = cmd
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .kill_on_drop(true)
            .spawn()
            .expect("start the test server");
        let stdin = child.stdin.take().unwrap();
        let mut out = BufReader::new(child.stdout.take().unwrap()).lines();
        let addr = out.next_line().await.unwrap().unwrap().strip_prefix("listening ").unwrap().to_string();
        let der = out.next_line().await.unwrap().unwrap().strip_prefix("pubkey ").unwrap().to_string();
        let der = base64::engine::general_purpose::STANDARD.decode(der).unwrap();
        let pem = format!(
            "-----BEGIN RSA PUBLIC KEY-----\n{}\n-----END RSA PUBLIC KEY-----\n",
            base64::engine::general_purpose::STANDARD.encode(der)
        );
        Some(Server {
            _child: child,
            stdin,
            out,
            addr,
            key: ServerKey::from_pem(&pem).unwrap(),
        })
    }

    async fn cmd(&mut self, line: &str) {
        self.stdin.write_all(format!("{line}\n").as_bytes()).await.unwrap();
        let done = self.out.next_line().await.unwrap().unwrap();
        assert!(done.starts_with("done"), "server answered {done:?}");
    }

    fn config(&self, obfuscated: bool) -> Config {
        let mut c = Config::new(self.addr.clone(), self.key.clone());
        c.obfuscated = obfuscated;
        c.max_backoff = Duration::from_millis(500);
        c
    }
}

fn account(seed: u8) -> SigningKey {
    SigningKey::from_bytes(&[seed; 32])
}

async fn next(ev: &mut Receiver<Event>) -> Event {
    timeout(Duration::from_secs(20), ev.recv())
        .await
        .expect("no event in 20 s")
        .expect("event stream ended")
}

fn notice(e: &Event) -> Option<&str> {
    match e {
        Event::Update(enums::Update::Notice(n)) => Some(&n.text),
        _ => None,
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn sign_in_push_catch_up_and_end() {
    let (a, b, stranger) = (account(1), account(2), account(3));
    let Some(mut srv) = Server::start(&[(&a, 0x9000000000a01, "Alpha"), (&b, 0x9000000000b02, "Bravo")]).await else {
        return;
    };

    // Sign-in over obfuscated2 and a live push.
    let (ca, mut ea) = start(
        srv.config(true),
        Account {
            key: a.clone(),
            device_id: 1,
        },
        None,
    );
    match next(&mut ea).await {
        Event::Connected { user, pts } => {
            assert_eq!((user.id, user.gamertag.as_str(), pts), (0x9000000000a01, "Alpha", 0));
        }
        e => panic!("first event {e:?}"),
    }
    srv.cmd("notify 9000000000a01 hello").await;
    let e = next(&mut ea).await;
    assert_eq!(notice(&e), Some("hello"), "got {e:?}");
    assert_eq!(ca.pts(), 1);
    let enums::updates::State::State(st) = ca.invoke(&functions::updates::GetState {}).await.unwrap();
    assert_eq!(st.pts, 1);

    // Two notices while Bravo is away; it catches up from pts 0 in order (plain intermediate framing this time).
    srv.cmd("notify 9000000000b02 one").await;
    srv.cmd("notify 9000000000b02 two").await;
    let (cb, mut eb) = start(
        srv.config(false),
        Account {
            key: b.clone(),
            device_id: 2,
        },
        Some(0),
    );
    let e1 = next(&mut eb).await;
    let e2 = next(&mut eb).await;
    assert_eq!((notice(&e1), notice(&e2)), (Some("one"), Some("two")), "got {e1:?} {e2:?}");
    assert!(matches!(next(&mut eb).await, Event::Connected { pts: 2, .. }));
    assert_eq!(cb.pts(), 2);

    // The server ends Alpha's session: the client stops and says why.
    srv.cmd("end 9000000000a01 banned").await;
    match next(&mut ea).await {
        Event::SignedOut { reason } => assert_eq!(reason, "banned"),
        e => panic!("after end: {e:?}"),
    }
    assert!(matches!(ca.invoke(&functions::updates::GetState {}).await, Err(Error::Disconnected(_))));

    // A key without an account is refused, and the client does not retry.
    let (_cs, mut es) = start(srv.config(true), Account { key: stranger, device_id: 3 }, None);
    match next(&mut es).await {
        Event::Refused {
            error: Error::Rpc { message, .. },
        } => assert_eq!(message, "ACCOUNT_UNKNOWN"),
        e => panic!("stranger: {e:?}"),
    }
}

#[tokio::test(flavor = "multi_thread")]
async fn wrong_pinned_key_fails_the_exchange() {
    let a = account(4);
    let Some(srv) = Server::start(&[(&a, 0x9000000000c03, "Charlie")]).await else {
        return;
    };
    // Another 2048-bit key: the server lists its own fingerprint, which the client does not pin.
    let other = ServerKey::from_parts(&[0xc5; 256], &[1, 0, 1]).unwrap();
    let mut cfg = srv.config(true);
    cfg.server_key = other;
    let (c, mut ev) = start(cfg, Account { key: a, device_id: 4 }, None);
    match next(&mut ev).await {
        Event::Disconnected { why, .. } => assert!(why.contains("fingerprints"), "{why}"),
        e => panic!("{e:?}"),
    }
    c.stop();
}
