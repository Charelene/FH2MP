//! The account key, read the way the launcher stores it (launcher/fh2mp_launcher/keystore.py), read-only: python-keyring under service
//! `fh2mp-launcher`, account `identity-v1` (dev launcher: `identity-v1-dev`), else the private file `<config dir>/identity.key`
//! (`{"v": 1, "seed": "<b64url>"}`). The seed never leaves this process and is never logged.

use std::path::{Path, PathBuf};

use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ed25519_dalek::SigningKey;
use zeroize::Zeroizing;

pub const SERVICE: &str = "fh2mp-launcher";

pub fn account(dev: bool) -> &'static str {
    if dev { "identity-v1-dev" } else { "identity-v1" }
}

/// The launcher's config folder (launcher/fh2mp_launcher/paths.py config_dir), FH2MP_LAUNCHER_HOME first.
pub fn config_dir(dev: bool) -> PathBuf {
    if let Some(h) = std::env::var_os("FH2MP_LAUNCHER_HOME") {
        return PathBuf::from(h);
    }
    #[cfg(windows)]
    {
        let base = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_else(home);
        base.join(if dev { "FH2MP-Dev" } else { "FH2MP" })
    }
    #[cfg(target_os = "macos")]
    {
        home().join("Library/Application Support").join(if dev { "FH2MP-Dev" } else { "FH2MP" })
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let base = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .unwrap_or_else(|| home().join(".config"));
        base.join(if dev { "fh2mp-dev" } else { "fh2mp" })
    }
}

fn home() -> PathBuf {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Where the key came from, for the status line.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Source {
    Keyring,
    File,
}

/// Loads the account key: the keyring first (as the launcher does), then the private file. Ok(None) = no account on this PC yet.
pub fn load(dev: bool) -> Result<Option<(SigningKey, Source)>, String> {
    if std::env::var_os("FH2MP_NO_KEYRING").is_none() {
        match read_seed(SERVICE, account(dev)) {
            Ok(Some(seed)) => return Ok(Some((SigningKey::from_bytes(&seed), Source::Keyring))),
            Ok(None) => {}
            Err(e) => tracing::warn!(error = %e, "keyring entry unreadable; trying the private file"),
        }
    }
    match read_file(&config_dir(dev))? {
        Some(seed) => Ok(Some((SigningKey::from_bytes(&seed), Source::File))),
        None => Ok(None),
    }
}

fn decode_seed(text: &str) -> Result<Zeroizing<[u8; 32]>, String> {
    let raw = Zeroizing::new(URL_SAFE_NO_PAD.decode(text.trim()).map_err(|e| format!("not base64url: {e}"))?);
    let seed: [u8; 32] = raw
        .as_slice()
        .try_into()
        .map_err(|_| format!("seed is {} bytes, expected 32", raw.len()))?;
    Ok(Zeroizing::new(seed))
}

/// The private-file fallback: `<config dir>/identity.key` = {"v": 1, "seed": "<b64url>"}.
pub fn read_file(dir: &Path) -> Result<Option<Zeroizing<[u8; 32]>>, String> {
    let raw = match std::fs::read(dir.join("identity.key")) {
        Ok(r) => Zeroizing::new(r),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(format!("identity.key: {e}")),
    };
    let v: serde_json::Value = serde_json::from_slice(&raw).map_err(|_| "identity.key is not JSON".to_string())?;
    let seed = v.get("seed").and_then(|s| s.as_str()).ok_or("identity.key has no seed")?;
    decode_seed(seed).map(Some)
}

/// Windows: python-keyring's WinVaultKeyring writes a generic credential with TargetName = service and UserName = account; when a second
/// account is saved under the same service the older one moves to the target "account@service". The blob is the password in UTF-16-LE.
#[cfg(windows)]
fn read_credential(target: &str) -> Option<(String, Zeroizing<Vec<u8>>)> {
    use windows_sys::Win32::Security::Credentials::{CRED_TYPE_GENERIC, CREDENTIALW, CredFree, CredReadW};
    let wide: Vec<u16> = target.encode_utf16().chain(std::iter::once(0)).collect();
    let mut cred: *mut CREDENTIALW = std::ptr::null_mut();
    // SAFETY: wide is NUL-terminated; on success CredReadW hands back a buffer that is read here and then freed with CredFree.
    unsafe {
        if CredReadW(wide.as_ptr(), CRED_TYPE_GENERIC, 0, &mut cred) == 0 || cred.is_null() {
            return None;
        }
        let c = &*cred;
        let mut user = String::new();
        if !c.UserName.is_null() {
            let mut n = 0;
            while *c.UserName.add(n) != 0 {
                n += 1;
            }
            user = String::from_utf16_lossy(std::slice::from_raw_parts(c.UserName, n));
        }
        let blob = Zeroizing::new(std::slice::from_raw_parts(c.CredentialBlob, c.CredentialBlobSize as usize).to_vec());
        CredFree(cred as *const _);
        Some((user, blob))
    }
}

#[cfg(windows)]
fn read_seed(service: &str, user: &str) -> Result<Option<Zeroizing<[u8; 32]>>, String> {
    let blob = match read_credential(service) {
        Some((u, b)) if u == user => b,
        _ => match read_credential(&format!("{user}@{service}")) {
            Some((u, b)) if u == user => b,
            Some((u, _)) => return Err(format!("compound entry {user}@{service} carries user {u:?}")),
            None => return Ok(None),
        },
    };
    if blob.len() % 2 != 0 {
        return Err(format!("blob of {} bytes is not UTF-16", blob.len()));
    }
    let units: Vec<u16> = blob.as_chunks::<2>().0.iter().map(|p| u16::from_le_bytes(*p)).collect();
    let text = Zeroizing::new(String::from_utf16(&units).map_err(|_| "blob is not valid UTF-16".to_string())?);
    decode_seed(&text).map(Some)
}

/// Linux: python-keyring's SecretService backend: default collection, attributes {service, username}, first item, password in UTF-8.
#[cfg(target_os = "linux")]
fn read_seed(service: &str, user: &str) -> Result<Option<Zeroizing<[u8; 32]>>, String> {
    use secret_service::blocking::SecretService;
    use secret_service::{EncryptionType, Error};
    use std::collections::HashMap;
    let ss = SecretService::connect(EncryptionType::Dh).map_err(|e| format!("secret service: {e}"))?;
    let coll = match ss.get_default_collection() {
        Ok(c) => c,
        Err(Error::NoResult) => return Ok(None),
        Err(e) => return Err(format!("default collection: {e}")),
    };
    coll.ensure_unlocked().map_err(|e| format!("unlock collection: {e}"))?;
    let items = coll
        .search_items(HashMap::from([("service", service), ("username", user)]))
        .map_err(|e| format!("search: {e}"))?;
    let Some(item) = items.first() else { return Ok(None) };
    item.ensure_unlocked().map_err(|e| format!("unlock item: {e}"))?;
    let secret = Zeroizing::new(item.get_secret().map_err(|e| format!("get secret: {e}"))?);
    let text = std::str::from_utf8(&secret).map_err(|_| "secret is not UTF-8".to_string())?;
    decode_seed(text).map(Some)
}

#[cfg(not(any(windows, target_os = "linux")))]
fn read_seed(_service: &str, _user: &str) -> Result<Option<Zeroizing<[u8; 32]>>, String> {
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn file_fallback_reads_the_launchers_format() {
        let dir = std::env::temp_dir().join(format!("fh2mp-chat-id-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // RFC 8032 test 1: seed 9d61b19d..., public key d75a9801...
        let seed = [
            0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69,
            0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
        ];
        std::fs::write(
            dir.join("identity.key"),
            format!(r#"{{"v": 1, "seed": "{}"}}"#, URL_SAFE_NO_PAD.encode(seed)),
        )
        .unwrap();
        let got = read_file(&dir).unwrap().unwrap();
        let pk = SigningKey::from_bytes(&got).verifying_key().to_bytes();
        assert_eq!(pk[..4], [0xd7, 0x5a, 0x98, 0x01]);
        std::fs::remove_dir_all(&dir).ok();
        assert!(read_file(&dir).unwrap().is_none());
    }
}
