//! Which chat server: prod or dev (`--dev`, as the dev launcher), each with its pinned RSA key. FH2MP_CHAT_ADDR and FH2MP_CHAT_KEY (a PEM
//! file) override them for a local server. The device id of this install lives next to the launcher's settings.

use std::path::Path;

use fh2mp_mtproto::ServerKey;

/// A chat server and how this install talks to it.
#[derive(Clone, Debug)]
pub struct Realm {
    pub name: &'static str,
    pub addr: String,
    pub key: ServerKey,
}

// The realms' pinned keys are made with ops/mt-keygen.sh when the chat goes to dev and prod (board item "MTProto chat P7"); until then a
// realm without a key is only reachable through FH2MP_CHAT_ADDR + FH2MP_CHAT_KEY.
const PROD_ADDR: &str = "api.forza.scuffed.af:5070";
const DEV_ADDR: &str = "dev-api.forza.scuffed.af:5170";
const PROD_KEY: Option<&str> = None;
const DEV_KEY: Option<&str> = None;

/// The realm to use, or why there is none.
pub fn pick(dev: bool) -> Result<Realm, String> {
    let name = if dev { "dev" } else { "prod" };
    let addr = std::env::var("FH2MP_CHAT_ADDR").unwrap_or_else(|_| (if dev { DEV_ADDR } else { PROD_ADDR }).to_string());
    let pem = match std::env::var_os("FH2MP_CHAT_KEY") {
        Some(p) => std::fs::read_to_string(&p).map_err(|e| format!("FH2MP_CHAT_KEY {}: {e}", Path::new(&p).display()))?,
        None => match if dev { DEV_KEY } else { PROD_KEY } {
            Some(k) => k.to_string(),
            None => return Err(format!("the {name} chat server is not set up yet")),
        },
    };
    let key = ServerKey::from_pem(&pem).map_err(|e| format!("chat server key: {e}"))?;
    Ok(Realm { name, addr, key })
}

/// This install's device id (random, kept in `<config dir>/chat/device.json`), signed into every sign-in so the server can tell a second PC
/// from a reconnect.
pub fn device_id(config: &Path) -> i64 {
    let dir = config.join("chat");
    let path = dir.join("device.json");
    if let Ok(raw) = std::fs::read(&path)
        && let Ok(v) = serde_json::from_slice::<serde_json::Value>(&raw)
        && let Some(id) = v.get("device").and_then(|d| d.as_str()).and_then(|s| u64::from_str_radix(s, 16).ok())
    {
        return id as i64;
    }
    let mut b = [0u8; 8];
    getrandom::fill(&mut b).expect("random device id");
    let id = u64::from_le_bytes(b);
    let _ = std::fs::create_dir_all(&dir);
    if let Err(e) = std::fs::write(&path, format!(r#"{{"v": 1, "device": "{id:016x}"}}"#)) {
        tracing::warn!(error = %e, path = %path.display(), "device id not saved; the next start uses a new one");
    }
    id as i64
}
