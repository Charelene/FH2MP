//! The server's pinned RSA public key and the sign-in message.

use std::sync::Arc;

use base64::Engine;
use grammers_crypto::rsa;
use grammers_tl_types::Serializable;
use num_bigint::BigUint;
use sha1::{Digest, Sha1};

/// The chat server's RSA public key, pinned per realm. Its fingerprint is the low 64 bits of SHA1 over the TL-serialized n and e, as the
/// server lists it in resPQ.
#[derive(Clone)]
pub struct ServerKey {
    rsa: Arc<rsa::Key>,
    fingerprint: i64,
}

impl std::fmt::Debug for ServerKey {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ServerKey({:016x})", self.fingerprint as u64)
    }
}

impl ServerKey {
    /// From a PEM public key: PKCS#1 ("RSA PUBLIC KEY", what `openssl rsa -RSAPublicKey_out` writes) or SubjectPublicKeyInfo ("PUBLIC KEY").
    pub fn from_pem(pem: &str) -> Result<Self, String> {
        let b64: String = pem.lines().filter(|l| !l.starts_with("-----")).map(str::trim).collect();
        let der = base64::engine::general_purpose::STANDARD
            .decode(b64)
            .map_err(|e| format!("PEM body is not base64: {e}"))?;
        let (n, e) = parse_rsa_der(&der)?;
        Self::from_parts(&n, &e)
    }

    /// From the modulus and exponent as big-endian bytes.
    pub fn from_parts(n: &[u8], e: &[u8]) -> Result<Self, String> {
        let n = strip_zeros(n);
        let e = strip_zeros(e);
        if n.len() != 256 {
            return Err(format!("RSA modulus of {} bits, 2048 expected", n.len() * 8));
        }
        let mut tl = n.to_vec().to_bytes();
        tl.extend(e.to_vec().to_bytes());
        let h = Sha1::digest(&tl);
        let fingerprint = i64::from_le_bytes(h[12..20].try_into().unwrap());
        let key = rsa::Key::new(&BigUint::from_bytes_be(n).to_str_radix(10), &BigUint::from_bytes_be(e).to_str_radix(10))
            .ok_or("RSA key parameters do not parse")?;
        Ok(Self {
            rsa: Arc::new(key),
            fingerprint,
        })
    }

    pub fn fingerprint(&self) -> i64 {
        self.fingerprint
    }

    pub(crate) fn rsa(&self) -> &rsa::Key {
        &self.rsa
    }
}

fn strip_zeros(b: &[u8]) -> &[u8] {
    let i = b.iter().position(|&x| x != 0).unwrap_or(b.len());
    &b[i..]
}

/// Reads one DER element at *pos: (tag, content).
fn der<'a>(b: &'a [u8], pos: &mut usize) -> Result<(u8, &'a [u8]), String> {
    let short = || "DER: truncated".to_string();
    let tag = *b.get(*pos).ok_or_else(short)?;
    let mut len = *b.get(*pos + 1).ok_or_else(short)? as usize;
    *pos += 2;
    if len & 0x80 != 0 {
        let n = len & 0x7f;
        if n == 0 || n > 4 {
            return Err(format!("DER: length of {n} bytes"));
        }
        len = 0;
        for _ in 0..n {
            len = len << 8 | *b.get(*pos).ok_or_else(short)? as usize;
            *pos += 1;
        }
    }
    let end = pos.checked_add(len).filter(|&e| e <= b.len()).ok_or_else(short)?;
    let c = &b[*pos..end];
    *pos = end;
    Ok((tag, c))
}

fn parse_rsa_der(b: &[u8]) -> Result<(Vec<u8>, Vec<u8>), String> {
    let mut p = 0;
    let (tag, seq) = der(b, &mut p)?;
    if tag != 0x30 {
        return Err("not a DER SEQUENCE".into());
    }
    let mut q = 0;
    let (t1, first) = der(seq, &mut q)?;
    match t1 {
        // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
        0x02 => {
            let (t2, e) = der(seq, &mut q)?;
            if t2 != 0x02 {
                return Err("RSAPublicKey: exponent is not an INTEGER".into());
            }
            Ok((first.to_vec(), e.to_vec()))
        }
        // SubjectPublicKeyInfo ::= SEQUENCE { algorithm SEQUENCE, subjectPublicKey BIT STRING (RSAPublicKey) }
        0x30 => {
            let (t2, bits) = der(seq, &mut q)?;
            if t2 != 0x03 || bits.first() != Some(&0) {
                return Err("SubjectPublicKeyInfo: no BIT STRING key".into());
            }
            parse_rsa_der(&bits[1..])
        }
        _ => Err(format!("unexpected DER tag {t1:#x}")),
    }
}

/// The exact bytes an account key signs for a sign-in (Go: `auth.SignMessage`): "FH2MP-AUTH-v1\0" realm "\0" purpose "\0" challenge, then
/// each field as a big-endian u16 length and its bytes.
pub fn sign_message(realm: &str, purpose: &str, challenge: &[u8], fields: &[&[u8]]) -> Vec<u8> {
    let mut out = b"FH2MP-AUTH-v1\0".to_vec();
    out.extend_from_slice(realm.as_bytes());
    out.push(0);
    out.extend_from_slice(purpose.as_bytes());
    out.push(0);
    out.extend_from_slice(challenge);
    for f in fields {
        out.extend_from_slice(&(f.len() as u16).to_be_bytes());
        out.extend_from_slice(f);
    }
    out
}
