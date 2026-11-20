package e2e

import (
	"crypto/aes"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/binary"

	"github.com/gotd/ige"
)

// Role is a side of a secret chat: the admin (made the request; x = 0, seq role bit 0) or the participant (x = 8, role bit 1).
type Role int

const (
	Admin       Role = 0
	Participant Role = 1
)

func (r Role) x() int { return 8 * int(r) }

// Other is the peer's role.
func (r Role) Other() Role { return 1 - r }

func (r Role) String() string {
	if r == Participant {
		return "participant"
	}
	return "admin"
}

const (
	// KeyLen is the length of a chat key.
	KeyLen = NumLen
	// HeaderLen is the clear part of a sealed message: u64LE(key_fingerprint) || msg_key(16).
	HeaderLen = 8 + 16
	minPad    = 12
	maxPad    = 1024
	// MaxData is the largest sealed message the server relays (messages.sendEncrypted, MESSAGE_TOO_LONG above it).
	MaxData = 16384
)

// Plaintext is u32LE(len(inner)) || inner || padding (contract 2.4); padding must be 12..1024 bytes and make the total a multiple of 16.
func Plaintext(inner, padding []byte) ([]byte, error) {
	if len(padding) < minPad || len(padding) > maxPad {
		return nil, refuse("padding of %d bytes, 12..1024 expected", len(padding))
	}
	n := 4 + len(inner) + len(padding)
	if n%16 != 0 {
		return nil, refuse("plaintext of %d bytes is not a multiple of 16", n)
	}
	p := make([]byte, 0, n)
	p = binary.LittleEndian.AppendUint32(p, uint32(len(inner)))
	p = append(p, inner...)
	return append(p, padding...), nil
}

// PaddingLen is the shortest padding (12..27 bytes) that aligns inner, plus extra whole blocks (capped so it stays at most 1024).
func PaddingLen(innerLen, extraBlocks int) int {
	n := minPad + (16-(4+innerLen+minPad)%16)%16
	for ; extraBlocks > 0 && n+16 <= maxPad; extraBlocks-- {
		n += 16
	}
	return n
}

// MsgKey is the message key of a plaintext sealed by sender (MTProto 2.0, Telegram's secret-chat rule):
// msg_key = SHA256(key[88+x : 88+x+32] || plaintext)[8:24].
func MsgKey(key []byte, sender Role, plaintext []byte) [16]byte {
	x := sender.x()
	h := sha256.New()
	h.Write(key[88+x : 88+x+32])
	h.Write(plaintext)
	var mk [16]byte
	copy(mk[:], h.Sum(nil)[8:24])
	return mk
}

// AESKeys derives the AES-256-IGE key and iv from the chat key and a message key:
// sha256_a = SHA256(msg_key || key[x : x+36]), sha256_b = SHA256(key[40+x : 40+x+36] || msg_key),
// aes_key = a[0:8] || b[8:24] || a[24:32], aes_iv = b[0:8] || a[8:24] || b[24:32].
func AESKeys(key []byte, sender Role, msgKey [16]byte) (aesKey, iv [32]byte) {
	x := sender.x()
	ha := sha256.New()
	ha.Write(msgKey[:])
	ha.Write(key[x : x+36])
	a := ha.Sum(nil)
	hb := sha256.New()
	hb.Write(key[40+x : 40+x+36])
	hb.Write(msgKey[:])
	b := hb.Sum(nil)
	copy(aesKey[0:8], a[0:8])
	copy(aesKey[8:24], b[8:24])
	copy(aesKey[24:32], a[24:32])
	copy(iv[0:8], b[0:8])
	copy(iv[8:24], a[8:24])
	copy(iv[24:32], b[24:32])
	return aesKey, iv
}

// SealPlaintext encrypts a finished plaintext: u64LE(key_fingerprint) || msg_key || AES-256-IGE(plaintext).
func SealPlaintext(key []byte, fingerprint int64, sender Role, plaintext []byte) ([]byte, error) {
	if len(key) != KeyLen {
		return nil, refuse("key of %d bytes", len(key))
	}
	if len(plaintext) == 0 || len(plaintext)%16 != 0 {
		return nil, refuse("plaintext of %d bytes is not whole blocks", len(plaintext))
	}
	mk := MsgKey(key, sender, plaintext)
	k, iv := AESKeys(key, sender, mk)
	blk, err := aes.NewCipher(k[:])
	if err != nil {
		return nil, err
	}
	out := make([]byte, HeaderLen+len(plaintext))
	binary.LittleEndian.PutUint64(out, uint64(fingerprint))
	copy(out[8:], mk[:])
	ige.EncryptBlocks(blk, iv[:], out[HeaderLen:], plaintext)
	return out, nil
}

// Seal seals inner (a TL-encoded decryptedMessageLayer) with the given padding.
func Seal(key []byte, fingerprint int64, sender Role, inner, padding []byte) ([]byte, error) {
	p, err := Plaintext(inner, padding)
	if err != nil {
		return nil, err
	}
	return SealPlaintext(key, fingerprint, sender, p)
}

// SealRandom seals inner with fresh random padding (12..1024 bytes, a random number of extra blocks).
func SealRandom(key []byte, fingerprint int64, sender Role, inner []byte) ([]byte, error) {
	var b [1]byte
	if _, err := rand.Read(b[:]); err != nil {
		return nil, err
	}
	pad := make([]byte, PaddingLen(len(inner), int(b[0]%4)))
	if _, err := rand.Read(pad); err != nil {
		return nil, err
	}
	return Seal(key, fingerprint, sender, inner, pad)
}

// DataFingerprint is the key fingerprint a sealed message names (false: too short to carry one).
func DataFingerprint(data []byte) (int64, bool) {
	if len(data) < 8 {
		return 0, false
	}
	return int64(binary.LittleEndian.Uint64(data)), true
}

// Open decrypts a message sealed by sender with key and returns the inner TL bytes. It checks the fingerprint, the block layout, the message
// key (in constant time), the length prefix and the padding (12..1024); any failure is a *Refused and nothing of the message is returned.
func Open(key []byte, fingerprint int64, sender Role, data []byte) ([]byte, error) {
	if len(key) != KeyLen {
		return nil, refuse("key of %d bytes", len(key))
	}
	if len(data) < HeaderLen+16 || (len(data)-HeaderLen)%16 != 0 {
		return nil, refuse("sealed message of %d bytes is not header plus whole blocks", len(data))
	}
	if fp, _ := DataFingerprint(data); fp != fingerprint {
		return nil, refuse("key fingerprint does not match")
	}
	var mk [16]byte
	copy(mk[:], data[8:24])
	k, iv := AESKeys(key, sender, mk)
	blk, err := aes.NewCipher(k[:])
	if err != nil {
		return nil, err
	}
	plain := make([]byte, len(data)-HeaderLen)
	ige.DecryptBlocks(blk, iv[:], plain, data[HeaderLen:])
	want := MsgKey(key, sender, plain)
	if subtle.ConstantTimeCompare(want[:], mk[:]) != 1 {
		return nil, refuse("msg_key does not match")
	}
	n := int(binary.LittleEndian.Uint32(plain))
	if n < 0 || 4+n > len(plain)-minPad {
		return nil, refuse("length prefix %d does not fit %d bytes with 12 of padding", n, len(plain))
	}
	if pad := len(plain) - 4 - n; pad > maxPad {
		return nil, refuse("padding of %d bytes, at most 1024", pad)
	}
	return plain[4 : 4+n], nil
}
