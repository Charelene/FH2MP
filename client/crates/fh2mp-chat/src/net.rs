//! The bridge between the window and the chat session: the session runs on a tokio runtime, the window sends `Cmd`s and reads `Ui` events
//! (each one wakes the window up).

use std::sync::mpsc as stdmpsc;
use std::time::Duration;

use eframe::egui;
use fh2mp_mtproto::tl::{enums, functions, types};
use fh2mp_mtproto::{Account, Client, Config, Error, Event, start};
use tokio::sync::mpsc;
use tokio::time::timeout;

/// What the window asks for.
#[derive(Debug)]
pub enum Cmd {
    RefreshFriends,
    AddFriend(String),
    Respond {
        user: i64,
        accept: bool,
    },
    RemoveFriend(i64),
    LoadClub {
        club: i64,
        after: i64,
    },
    SendClub {
        club: i64,
        random_id: i64,
        text: String,
    },
    ReadClub {
        club: i64,
        max_id: i64,
    },
    Gamerpic(i64),
    /// Sign in again after the server ended the session or refused it.
    Reconnect,
}

/// The session's state, as the window shows it.
#[derive(Clone, Debug, PartialEq)]
pub enum Status {
    Connecting,
    Online { xuid: i64, gamertag: String },
    Reconnecting { why: String, retry_in: Duration },
    SignedOut { reason: String },
    Refused { why: String },
}

/// What the window is told.
#[derive(Debug)]
pub enum Ui {
    Status(Status),
    Friends(types::friends::List),
    Presence {
        user: i64,
        presence: enums::Presence,
    },
    /// A page of club history; `fresh` = it replaces what is shown (first load), else it follows the last message.
    ClubPage {
        club: i64,
        messages: Vec<types::ClubMessage>,
        read_max_id: i64,
        fresh: bool,
    },
    /// One new club message (pushed, or our own answered post).
    ClubMessage(types::ClubMessage),
    /// A post failed; the text goes back into the input.
    SendFailed {
        text: String,
        why: String,
    },
    Gamerpic {
        user: i64,
        version: i64,
        png: Vec<u8>,
    },
    Notice(String),
    Error(String),
}

#[derive(Clone)]
struct Out {
    tx: stdmpsc::Sender<Ui>,
    ctx: egui::Context,
}

impl Out {
    fn send(&self, u: Ui) {
        let _ = self.tx.send(u);
        self.ctx.request_repaint();
    }
}

/// Runs the session until the window closes (the command channel ends).
pub async fn run(cfg: Config, account: Account, mut cmds: mpsc::UnboundedReceiver<Cmd>, tx: stdmpsc::Sender<Ui>, ctx: egui::Context) {
    let out = Out { tx, ctx };
    loop {
        out.send(Ui::Status(Status::Connecting));
        let (client, mut ev) = start(cfg.clone(), account.clone(), None);
        let ended = loop {
            tokio::select! {
                e = ev.recv() => match e {
                    Some(Event::Connected { user, .. }) => out.send(Ui::Status(Status::Online { xuid: user.id, gamertag: user.gamertag })),
                    Some(Event::Update(u)) => update(&client, &out, u),
                    Some(Event::Resync { .. }) => spawn_cmd(&client, &out, Cmd::RefreshFriends),
                    Some(Event::Disconnected { why, retry_in }) => out.send(Ui::Status(Status::Reconnecting { why, retry_in })),
                    Some(Event::SignedOut { reason }) => break Some(Status::SignedOut { reason }),
                    Some(Event::Refused { error }) => break Some(Status::Refused { why: refusal(&error) }),
                    None => break Some(Status::Refused { why: "the chat session ended".into() }),
                },
                c = cmds.recv() => match c {
                    Some(Cmd::Reconnect) => {}
                    Some(c) => spawn_cmd(&client, &out, c),
                    None => {
                        client.stop();
                        break None;
                    }
                },
            }
        };
        client.stop();
        let Some(status) = ended else { return };
        out.send(Ui::Status(status));
        // Stopped until the player asks to sign in again (never by itself: another PC may hold the session now).
        loop {
            match cmds.recv().await {
                Some(Cmd::Reconnect) => break,
                Some(_) => out.send(Ui::Error("not signed in to the chat".into())),
                None => return,
            }
        }
    }
}

fn refusal(e: &Error) -> String {
    match e {
        Error::Rpc { message, .. } if message == "ACCOUNT_UNKNOWN" => "this account is not known on the chat server (sign up in the launcher)".into(),
        Error::Rpc { message, .. } if message == "ACCOUNT_BANNED" => "this account is banned".into(),
        Error::Rpc { message, .. } if message == "SIGNATURE_INVALID" => "the chat server did not accept this PC's account key".into(),
        other => other.to_string(),
    }
}

fn update(client: &Client, out: &Out, u: enums::Update) {
    match u {
        enums::Update::UserPresence(p) => out.send(Ui::Presence {
            user: p.user_id,
            presence: p.presence,
        }),
        enums::Update::ClubMessage(m) => {
            let enums::ClubMessage::Message(m) = m.message;
            out.send(Ui::ClubMessage(m));
        }
        enums::Update::Friends => spawn_cmd(client, out, Cmd::RefreshFriends),
        enums::Update::Notice(n) => out.send(Ui::Notice(n.text)),
        enums::Update::LoggedOut(_) => {}
    }
}

const CALL_WAIT: Duration = Duration::from_secs(20);

fn spawn_cmd(client: &Client, out: &Out, c: Cmd) {
    let (client, out) = (client.clone(), out.clone());
    tokio::spawn(async move {
        if let Err(why) = run_cmd(&client, &out, c).await {
            out.send(Ui::Error(why));
        }
    });
}

async fn call<R: fh2mp_mtproto::tl::RemoteCall>(client: &Client, req: &R) -> Result<R::Return, String> {
    match timeout(CALL_WAIT, client.invoke(req)).await {
        Ok(Ok(r)) => Ok(r),
        Ok(Err(Error::Rpc { message, .. })) => Err(friendly(&message)),
        Ok(Err(e)) => Err(e.to_string()),
        Err(_) => Err(format!("no answer in {} s", CALL_WAIT.as_secs())),
    }
}

/// The server's error names in plain words.
fn friendly(code: &str) -> String {
    match code {
        "PLAYER_NOT_FOUND" => "No player has that gamertag or ID.",
        "SELF_REQUEST" => "That is your own account.",
        "ALREADY_FRIENDS" => "You are already friends.",
        "REQUEST_LIMIT" => "Cancel an outgoing request before sending another.",
        "REQUEST_GONE" => "This friend request is no longer available.",
        "CHAT_UNAVAILABLE" => "Club chat needs club membership.",
        "BAD_MESSAGE" => "Messages must contain 1–2000 characters.",
        "FLOOD_WAIT_60" => "Too many requests, wait a minute.",
        other => return format!("the chat server said {other}"),
    }
    .into()
}

async fn run_cmd(client: &Client, out: &Out, c: Cmd) -> Result<(), String> {
    match c {
        Cmd::RefreshFriends => {
            let enums::friends::List::List(l) = call(client, &functions::friends::Get {}).await?;
            out.send(Ui::Friends(l));
        }
        Cmd::AddFriend(query) => {
            call(client, &functions::friends::Add { query }).await?;
            out.send(Ui::Notice("Friend request sent.".into()));
        }
        Cmd::Respond { user, accept } => {
            if accept {
                call(client, &functions::friends::Accept { user_id: user }).await?;
            } else {
                call(client, &functions::friends::Decline { user_id: user }).await?;
            }
        }
        Cmd::RemoveFriend(user) => {
            call(client, &functions::friends::Remove { user_id: user }).await?;
        }
        Cmd::LoadClub { club, after } => {
            let enums::clubs::History::History(h) = call(
                client,
                &functions::clubs::GetHistory {
                    club_id: club,
                    after_id: after,
                    limit: 100,
                },
            )
            .await?;
            let messages = h.messages.into_iter().map(|enums::ClubMessage::Message(m)| m).collect();
            out.send(Ui::ClubPage {
                club,
                messages,
                read_max_id: h.read_max_id,
                fresh: after == 0,
            });
        }
        Cmd::SendClub { club, random_id, text } => match call(
            client,
            &functions::clubs::SendMessage {
                club_id: club,
                random_id,
                text: text.clone(),
            },
        )
        .await
        {
            Ok(enums::ClubMessage::Message(m)) => out.send(Ui::ClubMessage(m)),
            Err(why) => out.send(Ui::SendFailed { text, why }),
        },
        Cmd::ReadClub { club, max_id } => {
            call(client, &functions::clubs::ReadHistory { club_id: club, max_id }).await?;
        }
        Cmd::Gamerpic(user) => {
            let enums::users::Gamerpic::Gamerpic(g) = call(client, &functions::users::GetGamerpic { user_id: user }).await?;
            out.send(Ui::Gamerpic {
                user,
                version: g.version,
                png: g.png,
            });
        }
        Cmd::Reconnect => {}
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    //! The bridge against the Go test server's demo mode (native/cmd/fh2mp-mt-testserver -demo): sign-in, friends, club history, a post,
    //! a pushed post and a presence change, with a headless egui context. Skipped without Go and without FH2MP_MT_TESTSERVER.
    use super::*;
    use std::io::{BufRead, BufReader, Write};
    use std::process::{Command, Stdio};

    fn server() -> Option<std::path::PathBuf> {
        if let Ok(p) = std::env::var("FH2MP_MT_TESTSERVER") {
            return Some(p.into());
        }
        let native = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../native");
        let out = std::env::temp_dir().join(format!("fh2mp-mt-demo-{}{}", std::process::id(), std::env::consts::EXE_SUFFIX));
        let ok = Command::new("go")
            .args(["build", "-o"])
            .arg(&out)
            .arg("./cmd/fh2mp-mt-testserver")
            .current_dir(native)
            .status()
            .ok()?
            .success();
        ok.then_some(out)
    }

    fn wait(rx: &stdmpsc::Receiver<Ui>, what: &str, mut f: impl FnMut(&Ui) -> bool) {
        let end = std::time::Instant::now() + Duration::from_secs(20);
        while std::time::Instant::now() < end {
            if let Ok(u) = rx.recv_timeout(Duration::from_millis(200))
                && f(&u)
            {
                return;
            }
        }
        panic!("no {what} in 20 s");
    }

    #[test]
    fn bridge_against_the_demo_server() {
        let Some(bin) = server() else {
            eprintln!("skipped: no Go toolchain and no FH2MP_MT_TESTSERVER");
            return;
        };
        let pem = std::env::temp_dir().join(format!("fh2mp-mt-demo-{}.pem", std::process::id()));
        let mut child = Command::new(bin)
            .arg("-demo")
            .arg("-pubout")
            .arg(&pem)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let mut stdin = child.stdin.take().unwrap();
        let mut lines = BufReader::new(child.stdout.take().unwrap()).lines();
        let addr = lines.next().unwrap().unwrap().strip_prefix("listening ").unwrap().to_string();
        let _ = lines.next();
        let key = fh2mp_mtproto::ServerKey::from_pem(&std::fs::read_to_string(&pem).unwrap()).unwrap();
        let rt = tokio::runtime::Runtime::new().unwrap();
        let (cmd, cmds) = mpsc::unbounded_channel();
        let (tx, rx) = stdmpsc::channel();
        let account = Account {
            key: ed25519_dalek::SigningKey::from_bytes(&[7; 32]),
            device_id: 9,
        };
        rt.spawn(run(Config::new(addr, key), account, cmds, tx, egui::Context::default()));

        wait(&rx, "sign-in", |u| matches!(u, Ui::Status(Status::Online { .. })));
        cmd.send(Cmd::RefreshFriends).unwrap();
        wait(&rx, "friend list", |u| matches!(u, Ui::Friends(l) if l.friends.len() == 3));
        cmd.send(Cmd::LoadClub { club: 42, after: 0 }).unwrap();
        wait(
            &rx,
            "club history",
            |u| matches!(u, Ui::ClubPage { messages, fresh: true, .. } if messages.len() == 3),
        );
        cmd.send(Cmd::SendClub {
            club: 42,
            random_id: 1,
            text: "hello from rust".into(),
        })
        .unwrap();
        wait(&rx, "own post", |u| matches!(u, Ui::ClubMessage(m) if m.text == "hello from rust"));
        writeln!(stdin, "say Pixelwolf21378 ready when you are").unwrap();
        wait(&rx, "pushed post", |u| matches!(u, Ui::ClubMessage(m) if m.text == "ready when you are"));
        writeln!(stdin, "presence DebugFox roadtrip").unwrap();
        wait(
            &rx,
            "presence change",
            |u| matches!(u, Ui::Presence { presence: enums::Presence::Playing(p), .. } if p.lobby_kind == "roadtrip"),
        );
        cmd.send(Cmd::AddFriend("nobody".into())).unwrap();
        wait(&rx, "add refusal", |u| matches!(u, Ui::Error(e) if e.contains("No player")));
        drop(cmd);
        child.kill().ok();
        child.wait().ok();
        std::fs::remove_file(&pem).ok();
    }
}
