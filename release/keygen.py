#!/usr/bin/env python3
"""Makes the release signing key pair. The PRIVATE key signs every release manifest and must never be committed or uploaded anywhere but
as a CI secret; the PUBLIC key goes into launcher/fh2mp_launcher/constants.py (RELEASE_PUBKEY_B64).

    python3 -I release/keygen.py ~/.config/fh2mp-release/signing.key      # writes the private key (0600), prints the public key
"""
import base64
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    if os.path.exists(path):
        sys.exit(f"{path} exists; refusing to overwrite a signing key")
    sk = Ed25519PrivateKey.generate()
    seed = sk.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(base64.b64encode(seed).decode() + "\n")
    pub = sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print("private key written to", path, "(keep a backup; losing it means shipping a new launcher to change keys)")
    print("RELEASE_PUBKEY_B64 =", base64.b64encode(pub).decode())


if __name__ == "__main__":
    main()
