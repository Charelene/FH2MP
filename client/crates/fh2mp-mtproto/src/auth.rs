// Copyright 2020 - developers of the `grammers` project.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

// FH2MP changes (marked "FH2MP:"), 2026-11-10: this is grammers-mtproto 0.12.0 `src/authentication.rs` with three changes. The server's RSA
// key is ours and given by the caller (`ServerKey`) instead of Telegram's built-in list; the key asked for is a temporary one
// (p_q_inner_data_temp_dc with dc and expires_in), the only kind our server makes; Telegram's test vectors are left out (they exercise the
// permanent-key flow against Telegram's keys).

//! Sans-IO implementation of the process to generate an Authorization Key.
//!
//! # Examples
//!
//! ```no_run
//! use grammers_mtproto::authentication;
//!
//! fn send_data_to_server<R>(request: &R) -> Result<R::Return, authentication::Error>
//! where R: grammers_tl_types::RemoteCall {
//!     unimplemented!()
//! }
//!
//! fn main() -> Result<(), authentication::Error> {
//!     let (request, data) = authentication::step1()?;
//!     let response = send_data_to_server(&request)?;
//!
//!     let (request, data) = authentication::step2(data, response)?;
//!     let response = send_data_to_server(&request)?;
//!
//!     let (request, data) = authentication::step3(data, response)?;
//!     let response = send_data_to_server(&request)?;
//!
//!     let authentication::Finished { auth_key, .. } = authentication::create_key(data, response)?;
//!     // Now you have a secure `auth_key` to send encrypted messages to server.
//!     Ok(())
//! }
//! ```

use std::fmt;
use std::time::{SystemTime, UNIX_EPOCH};

use grammers_crypto::{AuthKey, check_dh_prime, factorize, hex, rsa};

use crate::key::ServerKey;
use grammers_tl_types::{self as tl, Cursor, Deserializable, Serializable};
use num_bigint::BigUint;
use sha1::{Digest, Sha1};

// NOTE! Turning this on will leak the key generation process to stdout!
// Should only be used for debugging purposes and generating test cases.
const TRACE_AUTH_GEN: bool = false;

/// Current value of dh_prime (in big-endian byte order).
const CURRENT_DH_PRIME: [u8; 256] = [
    0xC7, 0x1C, 0xAE, 0xB9, 0xC6, 0xB1, 0xC9, 0x04, 0x8E, 0x6C, 0x52, 0x2F, 0x70, 0xF1, 0x3F, 0x73, 0x98, 0x0D, 0x40, 0x23, 0x8E, 0x3E, 0x21, 0xC1,
    0x49, 0x34, 0xD0, 0x37, 0x56, 0x3D, 0x93, 0x0F, 0x48, 0x19, 0x8A, 0x0A, 0xA7, 0xC1, 0x40, 0x58, 0x22, 0x94, 0x93, 0xD2, 0x25, 0x30, 0xF4, 0xDB,
    0xFA, 0x33, 0x6F, 0x6E, 0x0A, 0xC9, 0x25, 0x13, 0x95, 0x43, 0xAE, 0xD4, 0x4C, 0xCE, 0x7C, 0x37, 0x20, 0xFD, 0x51, 0xF6, 0x94, 0x58, 0x70, 0x5A,
    0xC6, 0x8C, 0xD4, 0xFE, 0x6B, 0x6B, 0x13, 0xAB, 0xDC, 0x97, 0x46, 0x51, 0x29, 0x69, 0x32, 0x84, 0x54, 0xF1, 0x8F, 0xAF, 0x8C, 0x59, 0x5F, 0x64,
    0x24, 0x77, 0xFE, 0x96, 0xBB, 0x2A, 0x94, 0x1D, 0x5B, 0xCD, 0x1D, 0x4A, 0xC8, 0xCC, 0x49, 0x88, 0x07, 0x08, 0xFA, 0x9B, 0x37, 0x8E, 0x3C, 0x4F,
    0x3A, 0x90, 0x60, 0xBE, 0xE6, 0x7C, 0xF9, 0xA4, 0xA4, 0xA6, 0x95, 0x81, 0x10, 0x51, 0x90, 0x7E, 0x16, 0x27, 0x53, 0xB5, 0x6B, 0x0F, 0x6B, 0x41,
    0x0D, 0xBA, 0x74, 0xD8, 0xA8, 0x4B, 0x2A, 0x14, 0xB3, 0x14, 0x4E, 0x0E, 0xF1, 0x28, 0x47, 0x54, 0xFD, 0x17, 0xED, 0x95, 0x0D, 0x59, 0x65, 0xB4,
    0xB9, 0xDD, 0x46, 0x58, 0x2D, 0xB1, 0x17, 0x8D, 0x16, 0x9C, 0x6B, 0xC4, 0x65, 0xB0, 0xD6, 0xFF, 0x9C, 0xA3, 0x92, 0x8F, 0xEF, 0x5B, 0x9A, 0xE4,
    0xE4, 0x18, 0xFC, 0x15, 0xE8, 0x3E, 0xBE, 0xA0, 0xF8, 0x7F, 0xA9, 0xFF, 0x5E, 0xED, 0x70, 0x05, 0x0D, 0xED, 0x28, 0x49, 0xF4, 0x7B, 0xF9, 0x59,
    0xD9, 0x56, 0x85, 0x0C, 0xE9, 0x29, 0x85, 0x1F, 0x0D, 0x81, 0x15, 0xF6, 0x35, 0xB1, 0x05, 0xEE, 0x2E, 0x4E, 0x15, 0xD0, 0x4B, 0x24, 0x54, 0xBF,
    0x6F, 0x4F, 0xAD, 0xF0, 0x34, 0xB1, 0x04, 0x03, 0x11, 0x9C, 0xD8, 0xE3, 0xB9, 0x2F, 0xCC, 0x5B,
];

/// Current value of g.
const CURRENT_G: i32 = 3;

/// The error type for Authorization Key generation process.
#[derive(Clone, Debug, PartialEq)]
pub enum Error {
    /// The server's nonce did not match ours.
    InvalidNonce {
        /// The unexpected nonce that we got.
        got: [u8; 16],

        /// The expected nonce.
        expected: [u8; 16],
    },

    /// The server's PQ number was not of the right size.
    InvalidPqSize {
        /// The unexpected size that we got.
        size: usize,
    },

    /// None of the server fingerprints are known to us.
    UnknownFingerprints {
        /// The list of fingerprint that we got.
        fingerprints: Vec<i64>,
    },

    /// The server failed to send the Diffie-Hellman parameters.
    DhParamsFail,

    /// The server's nonce has changed during the key exchange.
    InvalidServerNonce {
        /// The unexpected nonce that we got.
        got: [u8; 16],

        /// The expected nonce.
        expected: [u8; 16],
    },

    /// The server's `encrypted_data` is not correctly padded.
    EncryptedResponseNotPadded {
        /// The non-padded length of the response.
        len: usize,
    },

    /// An error occured while trying to read the DH inner data.
    InvalidDhInnerData {
        /// The inner error that occured when reading the data.
        error: tl::deserialize::Error,
    },

    /// The server's parameter `g` is invalid.
    InvalidG(i32),

    /// The server's `dh_prime` is invalid.
    InvalidDhPrime,

    /// Some parameter (`g`, `g_a` or `g_b`) was out of range.
    GParameterOutOfRange {
        value: BigUint,
        low: BigUint,
        high: BigUint,
    },

    // The generation of Diffie-Hellman parameters is to be retried.
    DhGenRetry,

    // The generation of Diffie-Hellman parameters failed.
    DhGenFail,

    /// The plaintext answer hash did not match.
    InvalidAnswerHash {
        /// The unexpected hash that we got.
        got: [u8; 20],

        /// The expected hash.
        expected: [u8; 20],
    },

    // The new nonce hash did not match.
    InvalidNewNonceHash {
        /// The unexpected nonce that we got.
        got: [u8; 16],

        /// The expected nonce.
        expected: [u8; 16],
    },
}

impl std::error::Error for Error {}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidNonce { got, expected } => {
                write!(f, "invalid nonce: got {got:?}, expected {expected:?}")
            }
            Self::InvalidPqSize { size } => write!(f, "invalid pq size {size}"),
            Self::UnknownFingerprints { fingerprints } => {
                write!(f, "all server fingerprints are unknown: {fingerprints:?}")
            }
            Self::DhParamsFail => write!(f, "the generation of DH parameters by the server failed"),
            Self::InvalidServerNonce { got, expected } => write!(f, "invalid server nonce: got {got:?}, expected {expected:?}"),
            Self::EncryptedResponseNotPadded { len } => {
                write!(f, "the encrypted server response was {len} bytes long, which is not correctly padded")
            }
            Self::InvalidDhInnerData { error } => {
                write!(f, "could not deserialize DH inner data: {error}")
            }
            Self::InvalidG(g) => write!(f, "the parameter g was invalid: {g}"),
            Self::InvalidDhPrime => write!(f, "the parameter dh_prime was invalid"),
            Self::GParameterOutOfRange { low, high, value } => write!(f, "the parameter g = {value} was not in the range {low}..{high}"),
            Self::DhGenRetry => write!(f, "the generation of DH parameters should be retried"),
            Self::DhGenFail => write!(f, "the generation of DH parameters failed"),
            Self::InvalidAnswerHash { got, expected } => {
                write!(f, "invalid answer hash: got {got:?}, expected {expected:?}")
            }
            Self::InvalidNewNonceHash { got, expected } => write!(f, "invalid new nonce hash: got {got:?}, expected {expected:?}"),
        }
    }
}

/// The data generated by [`step1`], needed for [`step2`].
pub struct Step1 {
    nonce: [u8; 16],
}

/// The data generated by [`step2`], needed for [`step3`].
pub struct Step2 {
    nonce: [u8; 16],
    server_nonce: [u8; 16],
    new_nonce: [u8; 32],
}

/// The data generated by [`step3`], needed for [`create_key`].
pub struct Step3 {
    nonce: [u8; 16],
    server_nonce: [u8; 16],
    new_nonce: [u8; 32],
    gab: BigUint,
    time_offset: i32,
}

/// The first step of the process to generate an Authorization Key.
pub fn step1() -> Result<(tl::functions::ReqPqMulti, Step1), Error> {
    let random_bytes = {
        let mut buffer = [0; 16];
        getrandom::fill(&mut buffer).expect("failed to generate secure data for auth key");
        buffer
    };

    if TRACE_AUTH_GEN {
        println!("r {}", hex::to_hex(&random_bytes));
    }

    let res = do_step1(&random_bytes);
    if TRACE_AUTH_GEN && let Ok((x, _)) = &res {
        println!("> {}", hex::to_hex(&x.to_bytes()));
    }
    res
}

// n.b.: the `do_step` functions are pure so that they can be tested.
fn do_step1(random_bytes: &[u8; 16]) -> Result<(tl::functions::ReqPqMulti, Step1), Error> {
    // Step 1. Generates a secure random nonce.
    let nonce = *random_bytes;
    Ok((tl::functions::ReqPqMulti { nonce }, Step1 { nonce }))
}

/// The second step of the process to generate an Authorization Key.
pub fn step2(
    data: Step1,
    response: tl::enums::ResPq,
    key: &ServerKey,
    dc: i32,
    expires_in: i32,
) -> Result<(tl::functions::ReqDhParams, Step2), Error> {
    if TRACE_AUTH_GEN {
        println!("< {}", hex::to_hex(&response.to_bytes()));
    }

    let random_bytes = {
        let mut buffer = [0; 32 + 224];
        getrandom::fill(&mut buffer).expect("failed to generate secure data for auth key");
        buffer
    };

    if TRACE_AUTH_GEN {
        println!("r {}", hex::to_hex(&random_bytes));
    }

    let res = do_step2(data, response, &random_bytes, key, dc, expires_in);
    if TRACE_AUTH_GEN && let Ok((x, _)) = &res {
        println!("> {}", hex::to_hex(&x.to_bytes()));
    }
    res
}

fn do_step2(
    data: Step1,
    response: tl::enums::ResPq,
    random_bytes: &[u8; 32 + 224],
    server_key: &ServerKey,
    dc: i32,
    expires_in: i32,
) -> Result<(tl::functions::ReqDhParams, Step2), Error> {
    // Step 2. Validate the PQ response. Return `(p, q)` if it's valid.
    let Step1 { nonce } = data;
    let tl::enums::ResPq::Pq(res_pq) = response;

    check_nonce(&res_pq.nonce, &nonce)?;

    if res_pq.pq.len() != 8 {
        return Err(Error::InvalidPqSize { size: res_pq.pq.len() });
    }

    let pq = {
        let mut buffer = [0; 8];
        buffer.copy_from_slice(&res_pq.pq);
        u64::from_be_bytes(buffer)
    };

    let (p, q) = factorize(pq);
    let new_nonce = {
        let mut buffer = [0; 32];
        buffer.copy_from_slice(&random_bytes[..32]);
        buffer
    };

    // Remove the now-used first part from our available random data.
    let random_bytes = {
        let mut buffer = [0; 224];
        buffer.copy_from_slice(&random_bytes[32..]);
        buffer
    };

    // Convert (p, q) to bytes using the least amount of space possible.
    // If we don't do this, Telegram will respond with -404 as the message.
    let p_bytes = {
        let mut buffer = p.to_be_bytes().to_vec();
        if let Some(pos) = buffer.iter().position(|&b| b != 0) {
            buffer = buffer[pos..].to_vec();
        }
        buffer
    };
    let q_bytes = {
        let mut buffer = q.to_be_bytes().to_vec();
        if let Some(pos) = buffer.iter().position(|&b| b != 0) {
            buffer = buffer[pos..].to_vec();
        }
        buffer
    };

    // "pq is a representation of a natural number (in binary big endian format)"
    // https://core.telegram.org/mtproto/auth_key#dh-exchange-initiation
    // FH2MP: a temporary key for our dc.
    let pq_inner_data = tl::enums::PQInnerData::TempDc(tl::types::PQInnerDataTempDc {
        pq: pq.to_be_bytes().to_vec(),
        p: p_bytes.clone(),
        q: q_bytes.clone(),
        nonce,
        server_nonce: res_pq.server_nonce,
        new_nonce,
        dc,
        expires_in,
    })
    .to_bytes();

    // sha_digest + data + random_bytes
    // FH2MP: only the pinned key is used.
    let fingerprint = server_key.fingerprint();
    if !res_pq.server_public_key_fingerprints.contains(&fingerprint) {
        return Err(Error::UnknownFingerprints {
            fingerprints: res_pq.server_public_key_fingerprints.clone(),
        });
    }
    let ciphertext = rsa::encrypt_hashed(&pq_inner_data, server_key.rsa(), &random_bytes);

    Ok((
        tl::functions::ReqDhParams {
            nonce,
            server_nonce: res_pq.server_nonce,
            p: p_bytes,
            q: q_bytes,
            public_key_fingerprint: fingerprint,
            encrypted_data: ciphertext,
        },
        Step2 {
            nonce,
            server_nonce: res_pq.server_nonce,
            new_nonce,
        },
    ))
}

/// The third step of the process to generate an Authorization Key.
pub fn step3(data: Step2, response: tl::enums::ServerDhParams) -> Result<(tl::functions::SetClientDhParams, Step3), Error> {
    if TRACE_AUTH_GEN {
        println!("< {}", hex::to_hex(&response.to_bytes()));
    }

    let random_bytes = {
        let mut buffer = [0; 256 + 16];
        getrandom::fill(&mut buffer).expect("failed to generate secure data for auth key");
        buffer
    };

    if TRACE_AUTH_GEN {
        println!("r {}", hex::to_hex(&random_bytes));
    }

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system time is before epoch")
        .as_secs() as i32;

    let res = do_step3(data, response, &random_bytes, now);
    if TRACE_AUTH_GEN && let Ok((x, _)) = &res {
        println!("> {}", hex::to_hex(&x.to_bytes()));
    }
    res
}

fn do_step3(
    data: Step2,
    response: tl::enums::ServerDhParams,
    random_bytes: &[u8; 256 + 16],
    now: i32,
) -> Result<(tl::functions::SetClientDhParams, Step3), Error> {
    let Step2 {
        nonce,
        server_nonce,
        new_nonce,
    } = data;
    let server_dh_params = response;

    // Step 3. Factorize PQ and construct the request for DH params.
    let mut server_dh_params = match server_dh_params {
        tl::enums::ServerDhParams::Fail(server_dh_params) => {
            // Even though this is a failing case, we should still perform
            // all the security checks.
            check_nonce(&server_dh_params.nonce, &nonce)?;
            check_server_nonce(&server_dh_params.server_nonce, &server_nonce)?;

            let sha = {
                let mut hasher = Sha1::new();
                hasher.update(new_nonce);
                hasher.finalize()
            };
            let new_nonce_hash = {
                let mut buffer = [0; 16];
                buffer.copy_from_slice(&sha[4..20]);
                buffer
            };
            check_new_nonce_hash(&server_dh_params.new_nonce_hash, &new_nonce_hash)?;

            return Err(Error::DhParamsFail);
        }
        tl::enums::ServerDhParams::Ok(x) => x,
    };

    check_nonce(&server_dh_params.nonce, &nonce)?;
    check_server_nonce(&server_dh_params.server_nonce, &server_nonce)?;

    if server_dh_params.encrypted_answer.len() % 16 != 0 {
        return Err(Error::EncryptedResponseNotPadded {
            len: server_dh_params.encrypted_answer.len(),
        });
    }

    // Complete DH Exchange
    let (key, iv) = grammers_crypto::generate_key_data_from_nonce(&server_nonce, &new_nonce);

    // sha1 hash + plain text + padding
    grammers_crypto::aes::ige_decrypt(&mut server_dh_params.encrypted_answer, &key, &iv);
    let plain_text_answer = server_dh_params.encrypted_answer;

    let got_answer_hash = {
        let mut buffer = [0; 20];
        buffer.copy_from_slice(&plain_text_answer[..20]);
        buffer
    };

    // Use a cursor explicitly so we know where it ends (and most importantly
    // where the padding starts).
    let mut plain_text_cursor = Cursor::from_slice(&plain_text_answer[20..]);
    let server_dh_inner = match tl::enums::ServerDhInnerData::deserialize(&mut plain_text_cursor) {
        Ok(tl::enums::ServerDhInnerData::Data(x)) => x,
        Err(error) => return Err(Error::InvalidDhInnerData { error }),
    };

    let expected_answer_hash = {
        let mut hasher = Sha1::new();
        hasher.update(&plain_text_answer[20..20 + plain_text_cursor.pos()]);
        hasher.finalize().into()
    };

    if got_answer_hash != expected_answer_hash {
        return Err(Error::InvalidAnswerHash {
            got: got_answer_hash,
            expected: expected_answer_hash,
        });
    }

    check_nonce(&server_dh_inner.nonce, &nonce)?;
    check_server_nonce(&server_dh_inner.server_nonce, &server_nonce)?;

    let dh_prime = BigUint::from_bytes_be(&server_dh_inner.dh_prime);
    if server_dh_inner.dh_prime != CURRENT_DH_PRIME || server_dh_inner.g != CURRENT_G {
        if !(2..=7).contains(&server_dh_inner.g) {
            return Err(Error::InvalidG(server_dh_inner.g));
        }

        if !check_dh_prime(&dh_prime, server_dh_inner.g).unwrap() {
            return Err(Error::InvalidDhPrime);
        }
    }

    // Safe to unwrap because the numbers are valid
    let g = BigUint::from(server_dh_inner.g as u32);
    let g_a = BigUint::from_bytes_be(&server_dh_inner.g_a);

    let time_offset = server_dh_inner.server_time - now;

    let b = BigUint::from_bytes_be(&random_bytes[..256]);
    let g_b = g.modpow(&b, &dh_prime);
    let gab = g_a.modpow(&b, &dh_prime);

    // Remove the now-used first part from our available random data.
    let random_bytes = {
        let mut buffer = [0u8; 16];
        buffer.copy_from_slice(&random_bytes[256..]);
        buffer
    };

    // IMPORTANT: Apart from the conditions on the Diffie-Hellman prime
    // dh_prime and generator g, both sides are to check that g, g_a and
    // g_b are greater than 1 and less than dh_prime - 1. We recommend
    // checking that g_a and g_b are between 2^{2048-64} and
    // dh_prime - 2^{2048-64} as well.
    // (https://core.telegram.org/mtproto/auth_key#dh-key-exchange-complete)
    let safety_range = BigUint::ONE << (2048 - 64);
    check_g_in_range(&g_a, &safety_range, &(&dh_prime - &safety_range))?;
    check_g_in_range(&g_b, &safety_range, &(&dh_prime - &safety_range))?;

    // Prepare client DH Inner Data
    let client_dh_inner = tl::enums::ClientDhInnerData::Data(tl::types::ClientDhInnerData {
        nonce,
        server_nonce,
        retry_id: 0, // TODO use an actual retry_id
        g_b: g_b.to_bytes_be(),
    })
    .to_bytes();

    // sha1(client_dh_inner).digest() + client_dh_inner
    let sha = {
        let mut hasher = Sha1::new();
        hasher.update(&client_dh_inner);
        hasher.finalize()
    };

    let mut client_dh_inner_hashed = {
        let mut buffer = Vec::with_capacity(20 + client_dh_inner.len() + 16);

        buffer.extend(&sha);
        buffer.extend(&client_dh_inner);

        // Make sure we pad it ourselves, or else `encrypt_ige` will,
        // introducing randomness.
        let pad_len = (16 - (buffer.len() % 16)) % 16;
        buffer.extend(&random_bytes[..pad_len]);

        buffer
    };

    grammers_crypto::aes::ige_encrypt(&mut client_dh_inner_hashed, &key, &iv);
    let client_dh_encrypted = client_dh_inner_hashed;

    Ok((
        tl::functions::SetClientDhParams {
            nonce,
            server_nonce,
            encrypted_data: client_dh_encrypted,
        },
        Step3 {
            nonce,
            server_nonce,
            new_nonce,
            gab,
            time_offset,
        },
    ))
}

/// The final result of doing the authorization handshake, generated by [`create_key`].
#[derive(Clone, Debug, PartialEq)]
pub struct Finished {
    pub auth_key: [u8; 256],
    pub time_offset: i32,
    pub first_salt: i64,
}

/// The last step of the process to generate an Authorization Key.
pub fn create_key(data: Step3, response: tl::enums::SetClientDhParamsAnswer) -> Result<Finished, Error> {
    if TRACE_AUTH_GEN {
        println!("< {}", hex::to_hex(&response.to_bytes()));
    }

    let Step3 {
        nonce,
        server_nonce,
        new_nonce,
        gab,
        time_offset,
    } = data;
    let dh_gen = response;

    struct DhGenData {
        nonce: [u8; 16],
        server_nonce: [u8; 16],
        new_nonce_hash: [u8; 16],
        nonce_number: u8,
    }

    let dh_gen = match dh_gen {
        tl::enums::SetClientDhParamsAnswer::DhGenOk(x) => DhGenData {
            nonce: x.nonce,
            server_nonce: x.server_nonce,
            new_nonce_hash: x.new_nonce_hash1,
            nonce_number: 1,
        },
        tl::enums::SetClientDhParamsAnswer::DhGenRetry(x) => DhGenData {
            nonce: x.nonce,
            server_nonce: x.server_nonce,
            new_nonce_hash: x.new_nonce_hash2,
            nonce_number: 2,
        },
        tl::enums::SetClientDhParamsAnswer::DhGenFail(x) => DhGenData {
            nonce: x.nonce,
            server_nonce: x.server_nonce,
            new_nonce_hash: x.new_nonce_hash3,
            nonce_number: 3,
        },
    };

    check_nonce(&dh_gen.nonce, &nonce)?;
    check_server_nonce(&dh_gen.server_nonce, &server_nonce)?;

    let auth_key = {
        let mut buffer = [0; 256];
        let gab_bytes = gab.to_bytes_be();
        let skip = buffer.len() - gab_bytes.len(); // gab might need less than 256 bytes
        buffer[skip..].copy_from_slice(&gab_bytes);
        AuthKey::from_bytes(buffer)
    };

    let new_nonce_hash = auth_key.calc_new_nonce_hash(&new_nonce, dh_gen.nonce_number);
    check_new_nonce_hash(&dh_gen.new_nonce_hash, &new_nonce_hash)?;

    let first_salt = {
        let mut buffer = [0; 8];
        buffer
            .iter_mut()
            .zip(&new_nonce[..8])
            .zip(&server_nonce[..8])
            .for_each(|((x, a), b)| *x = a ^ b);
        i64::from_le_bytes(buffer)
    };

    if TRACE_AUTH_GEN {
        println!("a {}", hex::to_hex(&auth_key.to_bytes()));
        println!("o {time_offset}");
        println!("s {first_salt}");
    }

    // 1 for DhGenOk
    if dh_gen.nonce_number == 1 {
        Ok(Finished {
            auth_key: auth_key.to_bytes(),
            time_offset,
            first_salt,
        })
    } else {
        Err(Error::DhGenFail)
    }
}

/// Helper function to avoid the boilerplate of checking for invalid nonce.
fn check_nonce(got: &[u8; 16], expected: &[u8; 16]) -> Result<(), Error> {
    if got == expected {
        Ok(())
    } else {
        Err(Error::InvalidNonce {
            got: *got,
            expected: *expected,
        })
    }
}

/// Helper function to avoid the boilerplate of checking for invalid
/// server nonce.
fn check_server_nonce(got: &[u8; 16], expected: &[u8; 16]) -> Result<(), Error> {
    if got == expected {
        Ok(())
    } else {
        Err(Error::InvalidServerNonce {
            got: *got,
            expected: *expected,
        })
    }
}

/// Helper function to avoid the boilerplate of checking for invalid
/// new nonce hash.
fn check_new_nonce_hash(got: &[u8; 16], expected: &[u8; 16]) -> Result<(), Error> {
    if got == expected {
        Ok(())
    } else {
        Err(Error::InvalidNewNonceHash {
            got: *got,
            expected: *expected,
        })
    }
}

/// Helper function to avoid the boilerplate of checking for `g` not being
/// inside a valid range.
fn check_g_in_range(value: &BigUint, low: &BigUint, high: &BigUint) -> Result<(), Error> {
    if low < value && value < high {
        Ok(())
    } else {
        Err(Error::GParameterOutOfRange {
            value: value.clone(),
            low: low.clone(),
            high: high.clone(),
        })
    }
}
