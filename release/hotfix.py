#!/usr/bin/env python3
"""Signs the hotfix document the launcher fetches from /v1/hotfix (launcher/fh2mp_launcher/hotfix.py has the format).

    python3 -I release/hotfix.py sign doc.json signed.json [--key ~/.config/fh2mp-release/hotfix.key]
    python3 -I release/hotfix.py check signed.json

`sign` checks the document with the launcher's own rules before signing, and refuses a serial that is not higher than the one already in
signed.json (the launcher would ignore it). The signed file goes to the server as FH2MP_HOTFIX_FILE (/etc/fh2mp/<env>-hotfix.json); the
server re-reads it when it changes. The key is made once with `release/keygen.py ~/.config/fh2mp-release/hotfix.key`; its public half is
HOTFIX_PUBKEY_B64 in launcher/fh2mp_launcher/constants.py.
"""
import argparse
import base64
import json
import os
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launcher"))
from fh2mp_launcher import hotfix  # noqa: E402

DEFAULT_KEY = os.path.expanduser("~/.config/fh2mp-release/hotfix.key")


def sign(doc_path, out_path, key_path):
    with open(doc_path, "rb") as f:
        raw = f.read()
    try:
        doc = json.loads(raw)
    except ValueError as e:
        sys.exit(f"{doc_path} is not JSON: {e}")
    with open(key_path) as f:
        sk = Ed25519PrivateKey.from_private_bytes(base64.b64decode(f.read().strip()))
    signed = {"doc": base64.b64encode(raw).decode(), "sig": base64.b64encode(sk.sign(raw)).decode()}
    pub = base64.b64encode(sk.public_key().public_bytes_raw()).decode()
    try:
        hotfix.verify(signed, pub)
    except hotfix.HotfixError as e:
        sys.exit(f"not signed: {e}")
    if pub != hotfix.HOTFIX_PUBKEY_B64:
        print(f"warning: this key is not the one the launcher has built in (HOTFIX_PUBKEY_B64); launchers will refuse the document")
    if os.path.exists(out_path):
        try:
            with open(out_path, encoding="utf-8") as f:
                old = hotfix.verify(json.load(f), pub)
            if doc["serial"] <= old["serial"]:
                sys.exit(f"not signed: serial {doc['serial']} is not higher than {old['serial']} in {out_path}; launchers keep the newer one")
        except (OSError, ValueError, hotfix.HotfixError):
            pass
    tmp = out_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(signed, f)
    os.replace(tmp, out_path)
    rules = doc.get("rules", [])
    print(f"signed serial {doc['serial']} ({len(rules)} rule(s): {', '.join(r['id'] for r in rules) or 'none'}) -> {out_path}")


def check(path):
    with open(path, encoding="utf-8") as f:
        doc = hotfix.verify(json.load(f))
    print(json.dumps(doc, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sign")
    s.add_argument("doc")
    s.add_argument("out")
    s.add_argument("--key", default=DEFAULT_KEY)
    c = sub.add_parser("check")
    c.add_argument("signed")
    a = ap.parse_args()
    if a.cmd == "sign":
        sign(a.doc, a.out, a.key)
    else:
        try:
            check(a.signed)
        except hotfix.HotfixError as e:
            sys.exit(str(e))


if __name__ == "__main__":
    main()
