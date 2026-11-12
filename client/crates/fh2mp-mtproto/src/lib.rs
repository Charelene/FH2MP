//! The FH2MP chat's MTProto 2.0 client (proto/fh2mp.tl, our own schema; no Telegram service is involved).
//!
//! - Transport: intermediate framing, inside obfuscated2 by default.
//! - Keys: a fresh temporary auth key per connection, made against the realm's pinned RSA key (`ServerKey`); nothing is stored.
//! - Session layer: grammers-mtproto's sans-IO `Encrypted` state (msg ids, salts, acks, containers).
//! - Sign-in: the account's Ed25519 key signs the server's challenge bound to this auth key (`key::sign_message`).
//! - Updates: pushed `Updates` are applied in pts order; gaps are filled with updates.getDifference.

pub mod auth;
pub mod client;
mod conn;
pub mod key;

pub use client::{Account, Client, Config, Event, start};
pub use fh2mp_tl as tl;
pub use key::{ServerKey, sign_message};

/// Why a request or a connection failed.
#[derive(Clone, Debug, PartialEq)]
pub enum Error {
    /// The server answered rpc_error (Telegram-style code and UPPER_SNAKE message, e.g. 401 AUTH_KEY_UNREGISTERED).
    Rpc { code: i32, message: String },
    /// The connection ended before the answer; the request may or may not have run.
    Disconnected(String),
    /// The server broke the protocol, or a message did not decode.
    Protocol(String),
    /// The network failed.
    Io(String),
    /// The auth-key exchange failed (wrong pinned key, refused by the server, ...).
    Exchange(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Rpc { code, message } => write!(f, "rpc error {code} {message}"),
            Error::Disconnected(w) => write!(f, "disconnected: {w}"),
            Error::Protocol(w) => write!(f, "protocol error: {w}"),
            Error::Io(w) => write!(f, "network: {w}"),
            Error::Exchange(w) => write!(f, "key exchange: {w}"),
        }
    }
}

impl std::error::Error for Error {}
