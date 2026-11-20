// Package e2e is the Go reference of the client crypto of end-to-end direct messages (board item "MTProto chat P3"): chat device
// certificates, the Diffie-Hellman key of a secret chat on the pinned RFC 3526 group 14, the request and accept signatures, sealing and
// opening a message, sequence numbers, re-keying and franking (sections 2.1-2.7 of the P3 contract). The chat server uses the public-value
// checks and the franking check of it; the cross-language vectors (native/testdata/e2e_vectors.json) and the test server's demo peer are
// made with all of it. The launcher has its own implementation (Rust, crates/fh2mp-mtproto) that must agree with the vectors byte for byte.
//
// Outside work this follows:
//   - Telegram's end-to-end encryption spec: core.telegram.org/api/end-to-end (the secret chat, MTProto 2.0 message keys with x = 0 / 8),
//     /api/end-to-end/seq_no (sequence numbers and Resend) and /api/end-to-end/pfs (re-keying with RequestKey / AcceptKey / CommitKey).
//   - RFC 3526 section 3: the 2048-bit MODP group (group 14), generator 2.
//   - AES-256-IGE from gotd/ige (MIT, github.com/gotd/ige); the TL types are gotd-generated (gotd/td, MIT) from proto/fh2mp.tl.
//
// math/big's modular exponentiation is not constant time: this package is a reference and a test peer, never a client that keeps a real
// person's secrets. Nothing here logs a key, a secret exponent or a plaintext.
package e2e

import "fmt"

// Refused is why a sealed message, a public value or a signature was not taken. Reason is short and fixed (it goes into
// "chat.e2e_refused reason=..." lines) and never carries key material or text.
type Refused struct{ Reason string }

func (r *Refused) Error() string { return "e2e refused: " + r.Reason }

func refuse(format string, a ...any) error { return &Refused{Reason: fmt.Sprintf(format, a...)} }
