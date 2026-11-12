//! The frozen cross-language vectors (native/testdata/mt_vectors.json, written by the Go side): the sign-in message, TL encodings of our
//! schema and MTProto 2.0 encryption under a fixed key must come out byte-identical here.

use fh2mp_mtproto::sign_message;
use fh2mp_mtproto::tl::{Deserializable, Identifiable, Serializable, enums, functions};
use grammers_crypto::{AuthKey, decrypt_data_v2, encrypt_data_v2};
use serde_json::Value;

fn vectors() -> Value {
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../native/testdata/mt_vectors.json");
    serde_json::from_str(&std::fs::read_to_string(p).unwrap()).unwrap()
}

fn unhex(s: &Value) -> Vec<u8> {
    let s = s.as_str().unwrap();
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

#[test]
fn sign_message_matches_go() {
    for v in vectors()["sign_message"].as_array().unwrap() {
        let fields: Vec<Vec<u8>> = v["fields"].as_array().unwrap().iter().map(unhex).collect();
        let refs: Vec<&[u8]> = fields.iter().map(Vec::as_slice).collect();
        let got = sign_message(
            v["realm"].as_str().unwrap(),
            v["purpose"].as_str().unwrap(),
            &unhex(&v["challenge"]),
            &refs,
        );
        assert_eq!(got, unhex(&v["message"]));
    }
}

/// Decodes the Go bytes as the named type and encodes them again. A request decodes bare (grammers reads its constructor id first), so its
/// id is checked here and the rest decoded.
fn request<R: Identifiable + Deserializable + Serializable>(b: &[u8]) -> Vec<u8> {
    assert_eq!(u32::from_le_bytes(b[..4].try_into().unwrap()), R::CONSTRUCTOR_ID);
    R::from_bytes(&b[4..]).unwrap().to_bytes()
}

fn roundtrip(name: &str, b: &[u8]) -> Vec<u8> {
    match name {
        "help.init" => request::<functions::help::Init>(b),
        "help.config" => enums::help::Config::from_bytes(b).unwrap().to_bytes(),
        "auth.challenge" => enums::auth::Challenge::from_bytes(b).unwrap().to_bytes(),
        "auth.signIn" => request::<functions::auth::SignIn>(b),
        "auth.authorization" => enums::auth::Authorization::from_bytes(b).unwrap().to_bytes(),
        "updates.getDifference" => request::<functions::updates::GetDifference>(b),
        "updates" | "updateShort" => enums::Updates::from_bytes(b).unwrap().to_bytes(),
        "updates.differenceSlice" | "updates.differenceTooLong" => enums::updates::Difference::from_bytes(b).unwrap().to_bytes(),
        other => panic!("no Rust type mapped for vector {other}"),
    }
}

#[test]
fn tl_encodings_match_go() {
    for v in vectors()["tl"].as_array().unwrap() {
        let name = v["name"].as_str().unwrap();
        let b = unhex(&v["hex"]);
        assert_eq!(roundtrip(name, &b), b, "{name}");
    }
    // A field read back by value, not only by bytes.
    let v = vectors();
    let ups = v["tl"].as_array().unwrap().iter().find(|v| v["name"] == "updates").unwrap();
    let enums::Updates::Updates(u) = enums::Updates::from_bytes(&unhex(&ups["hex"])).unwrap() else {
        panic!("not updates")
    };
    assert_eq!((u.pts, u.pts_count, u.updates.len()), (43, 2, 2));
    let enums::Update::Notice(n) = &u.updates[0] else {
        panic!("not a notice")
    };
    assert_eq!(n.text, "hello ✓");
}

#[test]
fn mtproto2_encryption_matches_go() {
    let c = &vectors()["crypto"];
    let key: [u8; 256] = unhex(&c["auth_key"]).try_into().unwrap();
    let ak = AuthKey::from_bytes(key);
    assert_eq!(ak.key_id().to_vec(), unhex(&c["key_id"]));
    let plain = unhex(&c["plaintext"]);
    // Client to server (x = 0): our encryption gives the Go bytes.
    let mut buf = plain.clone();
    let mk = encrypt_data_v2(&mut buf, &ak);
    let mut frame = ak.key_id().to_vec();
    frame.extend(mk);
    frame.extend(buf);
    assert_eq!(frame, unhex(&c["client_to_server"]));
    // Server to client (x = 8): the Go bytes decrypt to the plaintext.
    let mut s2c = unhex(&c["server_to_client"]);
    assert_eq!(decrypt_data_v2(&mut s2c, &ak).unwrap().to_vec(), plain);
}
