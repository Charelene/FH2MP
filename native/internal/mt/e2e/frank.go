package e2e

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
)

const (
	frankTag = "FH2MP-CHAT-FRANK-v1\x00"
	// FrankingKeyLen is the length of a decryptedMessage's franking_key and of a commitment.
	FrankingKeyLen = 32
)

// Commitment is a message's franking commitment (contract 2.7): HMAC-SHA256(franking_key, "FH2MP-CHAT-FRANK-v1\0" || u64LE(sender_id) ||
// u64LE(recipient_id) || u64LE(random_id) || text_utf8). The sender sends it with the sealed message; the server keeps it, and a recipient
// who reports the message hands over the text and franking_key so the server can recompute it.
func Commitment(frankingKey []byte, sender, recipient, randomID int64, text string) []byte {
	m := hmac.New(sha256.New, frankingKey)
	b := make([]byte, 0, len(frankTag)+24)
	b = append(b, frankTag...)
	b = binary.LittleEndian.AppendUint64(b, uint64(sender))
	b = binary.LittleEndian.AppendUint64(b, uint64(recipient))
	b = binary.LittleEndian.AppendUint64(b, uint64(randomID))
	m.Write(b)
	m.Write([]byte(text))
	return m.Sum(nil)
}

// CheckCommitment recomputes the commitment and compares in constant time. A franking key of any length but 32 is refused (HMAC pads short
// keys with zeros, so a shorter key could otherwise stand in for a 32-byte one that ends in zeros).
func CheckCommitment(commitment, frankingKey []byte, sender, recipient, randomID int64, text string) bool {
	if len(frankingKey) != FrankingKeyLen || len(commitment) != sha256.Size {
		return false
	}
	return hmac.Equal(commitment, Commitment(frankingKey, sender, recipient, randomID, text))
}
